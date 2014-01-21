/*
 * Copyright (c) 2014, Clemson University
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of Clemson University nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mtpt.h"
#include "threadpool.h"
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct mtpt {
  struct threadpool tp;
  mtpt_dir_enter_method_t dir_enter_method;
  mtpt_dir_exit_method_t dir_exit_method;
  mtpt_file_method_t file_method;
  mtpt_error_method_t error_method;
  void *arg;
  int config;
  size_t spinlock_countdown;
  pthread_mutex_t mutex;
  pthread_barrier_t finished;
} mtpt_t;

typedef enum mtpt_task_type {
  TASK_TYPE_DIR_ENTER,
  TASK_TYPE_FILE,
  TASK_TYPE_DIR_EXIT
} mtpt_task_type_t;

typedef struct mtpt_dir_task {
  mtpt_task_type_t type;
  pthread_mutex_t mutex;
  mtpt_t *mtpt;
  void **data;
  struct mtpt_dir_task *parent;
  void *continuation;
  mtpt_dir_entry_t **entries;
  size_t entries_count;
  size_t children;
  struct stat st;
  char path[1];
} mtpt_dir_task_t;

typedef struct mtpt_file_task {
  mtpt_task_type_t type;
  mtpt_t *mtpt;
  void **data;
  struct mtpt_dir_task *parent;
  struct stat st;
  char path[1];
} mtpt_file_task_t;

static void mtpt_dir_exit_task_handler(void *arg);

static void mtpt_root_task_finished(mtpt_t *mtpt) {
  pthread_barrier_wait(&mtpt->finished);
}

static mtpt_file_task_t * mtpt_file_task_new(const char *path) {
  mtpt_file_task_t *task;

  task = malloc(sizeof(mtpt_file_task_t) + strlen(path));
  if(!task) return NULL;
  task->type = TASK_TYPE_FILE;
  strcpy(task->path, path);
  return task;
}

static mtpt_dir_task_t * mtpt_dir_task_new(const char *path) {
  mtpt_dir_task_t *task;
  int rc;

  task = malloc(sizeof(mtpt_dir_task_t) + strlen(path));
  if(!task) return NULL;
  rc = pthread_mutex_init(&task->mutex, NULL);
  if(rc) {
    free(task);
    errno = rc;
    return NULL;
  }
  task->type = TASK_TYPE_DIR_ENTER;
  task->continuation = NULL;
  task->entries = NULL;
  task->entries_count = 0;
  task->children = 0;
  strcpy(task->path, path);
  return task;
}

static void mtpt_dir_task_delete(mtpt_dir_task_t *task) {
  size_t i;

  pthread_mutex_destroy(&task->mutex);
  if(task->entries) {
    for(i = 0; i < task->entries_count; ++i) {
      free(task->entries[i]);
    }
    free(task->entries);
  }
  free(task);
}

static mtpt_dir_entry_t * mtpt_dir_entry_new(const char *name) {
  mtpt_dir_entry_t *entry = malloc(sizeof(mtpt_dir_entry_t) + strlen(name));
  if(!entry) return NULL;
  entry->data = NULL;
  strcpy(entry->name, name);
  return entry;
}

static int mtpt_dir_entry_pcmp(const void *p1, const void *p2) {
  const mtpt_dir_entry_t * const *e1 = p1;
  const mtpt_dir_entry_t * const *e2 = p2;
  return strcmp((*e1)->name, (*e2)->name);
}

static void mtpt_dir_task_child_finished(mtpt_dir_task_t *task) {
  mtpt_t *mtpt = task->mtpt;
  int rc;

  pthread_mutex_lock(&task->mutex);
  if(--task->children == 0) {
    task->type = TASK_TYPE_DIR_EXIT;
    rc = threadpool_add(&mtpt->tp, mtpt_dir_exit_task_handler, task);
    if(rc) {
      /* 
       * Getting here is bad.  This task has no more children and needs to be
       * put back in the queue so that its dir_exit_method can be run, but
       * there's not enough resources to add it back to the queue.  This will
       * have a cascading effect back up to the root task, and since the main
       * thread is blocked waiting on it to finish, this will lead to deadlock.
       *
       * To try and avoid this situation, print an error message and keep
       * trying to re-add this task's parent to the queue (a spinlock of
       * sorts).  It is possible that all threads end up at this point, at
       * which point we'd have a livelock on our hands.
       *
       * The livelock can be detected by keeping track of how many threads are
       * in this spinlock.  When livelock happens, our only choice is to abort
       * the process.
       */
      pthread_mutex_lock(&mtpt->mutex);
      if(--mtpt->spinlock_countdown == 0) {
        fprintf(stderr, "All threads have reached spinlock. Aborting.\n");
        abort();
      }
      fprintf(stderr,
        "Cannot requeue %s for processing because %s. Will keep trying.\n",
        task->path,
        strerror(rc)
      );
      pthread_mutex_unlock(&mtpt->mutex);
      do {
        sleep(1); // so we don't chew up the CPU
        rc = threadpool_add(&mtpt->tp, mtpt_dir_exit_task_handler, task);
      } while(rc);
      fprintf(stderr,
        "Successfully requeued %s for processing.\n",
        task->path
      );
      pthread_mutex_lock(&mtpt->mutex);
      ++mtpt->spinlock_countdown;
      pthread_mutex_unlock(&mtpt->mutex);
    }
  }
  pthread_mutex_unlock(&task->mutex);
}

static void mtpt_file_task_handler(void *arg) {
  mtpt_file_task_t *task = arg;
  mtpt_t *mtpt = task->mtpt;

  if(mtpt->file_method) {
    *task->data = (*mtpt->file_method)(mtpt->arg, task->path, &task->st);
  }

  // files (non-directories) will never be the root task and will always
  // have a parent
  mtpt_dir_task_child_finished(task->parent);
  free(task);
}

static void mtpt_dir_exit_task_handler(void *arg) {
  mtpt_dir_task_t *task = arg;
  mtpt_t *mtpt = task->mtpt;

  // wait until mtpt_dir_task_child_finished() is done with me
  pthread_mutex_lock(&task->mutex);
  pthread_mutex_unlock(&task->mutex);

  if(mtpt->dir_exit_method) {
    *task->data = (*mtpt->dir_exit_method)(
      mtpt->arg,
      task->path,
      &task->st,
      task->continuation,
      task->entries,
      task->entries_count
    );
  }

  if(task->parent) {
    mtpt_dir_task_child_finished(task->parent);
  } else {
    mtpt_root_task_finished(mtpt);
  }
  mtpt_dir_task_delete(task);
}

static void mtpt_dir_enter_task_handler(void *arg) {
  mtpt_dir_task_t *task = arg;
  mtpt_t *mtpt = task->mtpt;
  DIR *d;
  struct dirent *dirp;
  mtpt_dir_entry_t *entry;
  mtpt_dir_entry_t **entries;
  size_t entries_size, entries_count, i;
  int rc, no_children;

  if(mtpt->dir_enter_method) {
    rc = (*mtpt->dir_enter_method)(mtpt->arg, task->path, &task->st, &task->continuation);
    if(!rc) {
      if(task->parent) {
        mtpt_dir_task_child_finished(task->parent);
      } else {
        mtpt_root_task_finished(mtpt);
      }
      mtpt_dir_task_delete(task);
      return;
    }
  }

  // open the directory
  d = opendir(task->path);
  if(!d) {
    if(mtpt->error_method) {
      *task->data = (*mtpt->error_method)(mtpt->arg, task->path, &task->st, task->continuation);
    }
    mtpt_dir_task_delete(task);
    return;
  }

  // read entries into a sorted array
  entries_size = 256;
  entries_count = 0;
  entries = malloc(sizeof(mtpt_dir_entry_t *) * entries_size);
  if(!entries) goto entries_malloc_fail;
  while(errno = 0, (dirp = readdir(d))) {
    if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0)
      continue;
    if(entries_count == entries_size) {
      entries_size <<= 1;
      entries = realloc(entries, sizeof(char *) * entries_size);
      if(!entries) goto entries_realloc_fail;
    }
    entry = mtpt_dir_entry_new(dirp->d_name);
    if(!entry) goto entries_realloc_fail;
    entries[entries_count++] = entry;
  }
  if(errno) {
entries_realloc_fail:
    rc = errno;
    for(i = 0; i < entries_count; ++i) {
      free(entries[i]);
    }
    free(entries);
    errno = rc;
entries_malloc_fail:
    if(mtpt->error_method) {
      *task->data = (*mtpt->error_method)(mtpt->arg, task->path, &task->st, task->continuation);
    }
    closedir(d);
    mtpt_dir_task_delete(task);
    return;
  }
  closedir(d);
  if(mtpt->config & MTPT_CONFIG_SORT) {
    qsort(entries, entries_count, sizeof(mtpt_dir_entry_t *), mtpt_dir_entry_pcmp);
  }
  task->entries = entries;
  task->entries_count = entries_count;

  // loop through entries
  no_children = 1;
  pthread_mutex_lock(&task->mutex);
  for(i = 0; i < entries_count; ++i) {
    struct stat st;
    char path[PATH_MAX];

    entry = entries[i];
    snprintf(path, PATH_MAX, "%s/%s", task->path, entry->name);
    rc = lstat(path, &st);
    if(rc) {
      if(errno != ENOENT) {
        if(mtpt->error_method) {
          *task->data = (*mtpt->error_method)(mtpt->arg, path, NULL, NULL);
        }
      }
      continue;
    }

    if(S_ISDIR(st.st_mode)) {
      mtpt_dir_task_t *t = mtpt_dir_task_new(path);
      if(!t) goto dir_task_new_fail;
      t->mtpt = mtpt;
      t->data = &entry->data;
      t->parent = task;
      t->st = st;
      rc = threadpool_add(&mtpt->tp, mtpt_dir_enter_task_handler, t);
      if(rc) {
        mtpt_dir_task_delete(t);
        errno = rc;
dir_task_new_fail:
        if(mtpt->error_method) {
          *task->data = (*mtpt->error_method)(mtpt->arg, path, &st, NULL);
        }
      } else {
        ++task->children;
        no_children = 0;
      }
    } else if(mtpt->config & MTPT_CONFIG_FILE_TASKS) {
      mtpt_file_task_t *t = mtpt_file_task_new(path);
      if(!t) goto file_task_new_fail;
      t->mtpt = mtpt;
      t->data = &entry->data;
      t->parent = task;
      t->st = st;
      rc = threadpool_add(&mtpt->tp, mtpt_file_task_handler, t);
      if(rc) {
        free(t);
        errno = rc;
file_task_new_fail:
        if(mtpt->error_method) {
          *task->data = (*mtpt->error_method)(mtpt->arg, path, &st, NULL);
        }
      } else {
        ++task->children;
        no_children = 0;
      }
    } else {
      if(mtpt->file_method) {
        entry->data = (*mtpt->file_method)(mtpt->arg, path, &st);
      }
    }
  }
  pthread_mutex_unlock(&task->mutex);

  if(no_children) {
    mtpt_dir_exit_task_handler(task);
  }
}

static int mtpt_task_priority_cmp(
  const struct threadpool_task *a,
  const struct threadpool_task *b
) {
  mtpt_task_type_t *type_a = a->arg;
  mtpt_task_type_t *type_b = b->arg;
  int diff = *type_a - *type_b;
  if(diff) return diff;
  if(*type_a == TASK_TYPE_FILE) {
    const mtpt_file_task_t *task_a = a->arg;
    if(!(task_a->mtpt->config & MTPT_CONFIG_SORT)) return 0;
    const mtpt_file_task_t *task_b = b->arg;
    return strcmp(task_b->path, task_a->path);
  } else {
    const mtpt_dir_task_t *task_a = a->arg;
    if(!(task_a->mtpt->config & MTPT_CONFIG_SORT)) return 0;
    const mtpt_dir_task_t *task_b = b->arg;
    return strcmp(task_b->path, task_a->path);
  }
}

int mtpt(
  size_t nthreads,
  int config,
  const char *path,
  mtpt_dir_enter_method_t dir_enter_method,
  mtpt_dir_exit_method_t dir_exit_method,
  mtpt_file_method_t file_method,
  mtpt_error_method_t error_method,
  void *arg,
  void **data
) {
  mtpt_t mtpt;
  struct stat st;
  void *d = NULL;
  mtpt_dir_task_t *root_task;
  int rc, ret = 0;

  // stat path
  rc = lstat(path, &st);
  if(rc) return -1;

  // if the root path is not a directory, just handle it in this thread
  if(!S_ISDIR(st.st_mode)) {
    d = (*file_method)(arg, path, &st);
    if(data) *data = d;
    return 0;
  }

  // create task for root path
  root_task = mtpt_dir_task_new(path);
  if(root_task == NULL) return -1;
  root_task->mtpt = &mtpt;
  root_task->data = &d;
  root_task->parent = NULL;
  root_task->st = st;

  // initialize the mtpt structure
  rc = pthread_mutex_init(&mtpt.mutex, NULL);
  if(rc) {
    errno = rc;
    ret = -1;
    goto out1;
  }
  rc = pthread_barrier_init(&mtpt.finished, NULL, 2);
  if(rc) {
    errno = rc;
    ret = -1;
    goto out2;
  }
  rc = threadpool_init_prio(&mtpt.tp, nthreads, 0, mtpt_task_priority_cmp);
  if(rc) {
    errno = rc;
    ret = -1;
    goto out3;
  }
  mtpt.dir_enter_method = dir_enter_method;
  mtpt.dir_exit_method = dir_exit_method;
  mtpt.file_method = file_method;
  mtpt.error_method = error_method;
  mtpt.arg = arg;
  mtpt.config = config;
  mtpt.spinlock_countdown = nthreads;

  // 3...2...1...GO!
  rc = threadpool_add(&mtpt.tp, mtpt_dir_enter_task_handler, root_task);
  if(rc) {
    errno = rc;
    ret = -1;
    goto out4;
  }
  root_task = NULL;

  // wait for all tasks to finish
  pthread_barrier_wait(&mtpt.finished);

  if(data) *data = d;

out4:
  threadpool_destroy(&mtpt.tp);
out3:
  pthread_barrier_destroy(&mtpt.finished);
out2:
  pthread_mutex_destroy(&mtpt.mutex);
out1:
  if(root_task) mtpt_dir_task_delete(root_task);

  return ret;
}
