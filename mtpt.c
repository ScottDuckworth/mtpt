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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct mtpt {
  struct threadpool tp;
  mtpt_method_t dir_enter_method;
  mtpt_method_t dir_exit_method;
  mtpt_method_t file_method;
  mtpt_method_t error_method;
  void *arg;
  int config;
  pthread_mutex_t mutex;
  pthread_cond_t finished;
} mtpt_t;

typedef struct mtpt_task {
  mtpt_t *mtpt;
  pthread_mutex_t mutex;
  struct mtpt_task *parent;
  size_t children;
  int visited;
  struct stat st;
  char path[0];
} mtpt_task_t;

static mtpt_task_t * mtpt_task_new(const char *path) {
  mtpt_task_t *task;
  int rc;

  task = malloc(sizeof(mtpt_task_t) + strlen(path) + 1);
  if(!task) return NULL;
  rc = pthread_mutex_init(&task->mutex, NULL);
  if(rc) {
    errno = rc;
    free(task);
    return NULL;
  }
  strcpy(task->path, path);
  return task;
}

static void mtpt_task_delete(mtpt_task_t *task) {
  pthread_mutex_destroy(&task->mutex);
  free(task);
}

static int cmpstringp(const void *p1, const void *p2) {
  return strcmp(*(char * const *) p1, *(char * const *) p2);
}

static void task_handler(void *arg) {
  mtpt_task_t *task = arg;
  mtpt_t *mtpt = task->mtpt;
  DIR *d;
  struct dirent *dirp;
  int rc;

  pthread_mutex_lock(&task->mutex);
  if(S_ISDIR(task->st.st_mode)) {
    if(task->visited) {
      if(mtpt->dir_exit_method)
        (*mtpt->dir_exit_method)(mtpt->arg, task->path, &task->st);
    } else {
      rc = 1;
      if(mtpt->dir_enter_method)
        rc = (*mtpt->dir_enter_method)(mtpt->arg, task->path, &task->st);
      if(rc) {
        d = opendir(task->path);
        if(!d) {
          if(mtpt->error_method)
            (*mtpt->error_method)(mtpt->arg, task->path, &task->st);
        } else {
          char *p;
          char **contents;
          size_t contents_size, contents_count, i;

          // read contents into a sorted array
          contents_size = 256;
          contents_count = 0;
          contents = malloc(sizeof(char *) * contents_size);
          while((dirp = readdir(d))) {
            if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0)
              continue;
            p = malloc(strlen(dirp->d_name)+1);
            strcpy(p, dirp->d_name);
            contents[contents_count] = p;
            if(++contents_count == contents_size) {
              contents_size <<= 1;
              contents = realloc(contents, sizeof(char *) * contents_size);
            }
          }
          closedir(d);
          if(mtpt->config & MTPT_CONFIG_SORT)
            qsort(contents, contents_count, sizeof(char *), cmpstringp);

          // loop through contents
          for(i = 0; i < contents_count; ++i) {
            struct stat st;
            char path[PATH_MAX];

            p = contents[i];
            snprintf(path, PATH_MAX, "%s/%s", task->path, p);
            rc = lstat(path, &st);
            if(rc) {
              if(errno != ENOENT) {
                if(mtpt->error_method)
                  (*mtpt->error_method)(mtpt->arg, path, NULL);
              }
              continue;
            }

            if(S_ISDIR(st.st_mode) || mtpt->config & MTPT_CONFIG_FILE_TASKS) {
              mtpt_task_t *t = mtpt_task_new(path);
              t->mtpt = mtpt;
              t->parent = task;
              t->children = 0;
              t->visited = 0;
              t->st = st;
              rc = threadpool_add(&mtpt->tp, task_handler, t);
              if(rc) {
                errno = rc;
                if(mtpt->error_method)
                  (*mtpt->error_method)(mtpt->arg, path, &st);
                mtpt_task_delete(t);
              } else {
                ++task->children;
              }
            } else {
              if(mtpt->file_method)
                (*mtpt->file_method)(mtpt->arg, path, &st);
            }
          }

          // free contents
          for(i = 0; i < contents_count; ++i) {
            p = contents[i];
            free(p);
          }
          free(contents);
        }
      }
      if(task->children == 0) {
        if(mtpt->dir_exit_method)
          (*mtpt->dir_exit_method)(mtpt->arg, task->path, &task->st);
      } else {
        task->visited = 1;
        pthread_mutex_unlock(&task->mutex);
        return;
      }
    }
  } else {
    if(mtpt->file_method)
      (*mtpt->file_method)(mtpt->arg, task->path, &task->st);
  }

  if(task->parent) {
    pthread_mutex_lock(&task->parent->mutex);
    if(--task->parent->children == 0) {
      rc = threadpool_add(&mtpt->tp, task_handler, task->parent);
      if(rc) {
        errno = rc;
        if(mtpt->error_method)
          (*mtpt->error_method)(mtpt->arg, task->parent->path, &task->parent->st);
        pthread_mutex_unlock(&task->parent->mutex);
        mtpt_task_delete(task->parent);
      }
    }
    pthread_mutex_unlock(&task->parent->mutex);
  } else {
    // the root task is finished
    pthread_mutex_lock(&mtpt->mutex);
    pthread_cond_signal(&mtpt->finished);
    pthread_mutex_unlock(&mtpt->mutex);
  }
  pthread_mutex_unlock(&task->mutex);
  mtpt_task_delete(task);
}

int mtpt(
  size_t nthreads,
  int config,
  const char *path,
  mtpt_method_t dir_enter_method,
  mtpt_method_t dir_exit_method,
  mtpt_method_t file_method,
  mtpt_method_t error_method,
  void *arg
) {
  mtpt_t mtpt;
  mtpt_task_t *task;
  int rc;

  // create task for path
  task = mtpt_task_new(path);
  if(task == NULL) return -1;
  task->mtpt = &mtpt;
  task->parent = NULL;
  task->children = 0;
  task->visited = 0;

  // stat path
  rc = stat(path, &task->st);
  if(rc) return -1;

  // initialize the mtpt structure
  rc = threadpool_init(&mtpt.tp, nthreads, 0);
  if(rc) {
    mtpt_task_delete(task);
    errno = rc;
    return -1;
  }
  mtpt.dir_enter_method = dir_enter_method;
  mtpt.dir_exit_method = dir_exit_method;
  mtpt.file_method = file_method;
  mtpt.error_method = error_method;
  mtpt.arg = arg;
  mtpt.config = config;
  pthread_mutex_init(&mtpt.mutex, NULL);
  pthread_cond_init(&mtpt.finished, NULL);

  // 3...2...1...GO!
  rc = threadpool_add(&mtpt.tp, task_handler, task);
  if(rc) {
    threadpool_destroy(&mtpt.tp);
    mtpt_task_delete(task);
    errno = rc;
    return -1;
  }

  // wait for all tasks to finish and shut down
  pthread_mutex_lock(&mtpt.mutex);
  pthread_cond_wait(&mtpt.finished, &mtpt.mutex);
  pthread_mutex_unlock(&mtpt.mutex);
  pthread_mutex_destroy(&mtpt.mutex);
  pthread_cond_destroy(&mtpt.finished);
  threadpool_destroy(&mtpt.tp);

  return 0;
}
