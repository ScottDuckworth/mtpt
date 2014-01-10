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

#define _BSD_SOURCE
#define _FILE_OFFSET_BITS 64
#include "mtpt.h"
#include "threadpool.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#define IO_BUFFER_SIZE (1<<20) // 1 MB

struct traverse_arg {
  const char *src_root;
  const char *dst_root;
  size_t src_root_len;
  size_t dst_root_len;
};

struct sync_file_arg {
  struct stat src_st;
  struct stat dst_st;
  int dst_exists;
  int dst_fd;
  char src[PATH_MAX];
  char dst[PATH_MAX];
};

static struct threadpool g_tp;
static int g_error = 0;
static int g_verbose = 0;
static int g_delete = 1;

static void usage(FILE *file, const char *arg0) {
  fprintf(file,
    "Usage: %s [options] source destination\n"
    "Options:\n"
    "  -h    Print this message\n"
    "  -v    Be verbose\n"
    "  -D    Do not delete files not in source from destination\n"
    "  -j N  Copy N files a a time\n"
    , arg0);
}

static void unlink_dir(const char *path) {
  int rc;
  DIR *d;
  struct dirent *dirp;
  struct stat st;
  char p[PATH_MAX];

  d = opendir(path);
  if(!d) {
    g_error = 1;
    return;
  }
  while((dirp = readdir(d))) {
    if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0) {
      continue;
    }
    snprintf(p, PATH_MAX, "%s/%s", path, dirp->d_name);
#ifdef _DIRENT_HAVE_D_TYPE
    switch(dirp->d_type) {
    case DT_UNKNOWN:
      break;
    case DT_DIR:
      goto delete_dir;
    default:
      goto delete_other;
    }
#endif
    rc = lstat(p, &st);
    if(rc) {
      perror(p);
      g_error = 1;
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
    delete_dir:
      unlink_dir(p);
    } else {
    delete_other:
      unlink(p);
    }
  }
  closedir(d);
  rc = rmdir(path);
  if(rc) {
    perror(path);
    g_error = 1;
  }
}

static int cmpstringp(const void *p1, const void *p2) {
  return strcmp(*(char * const *) p1, *(char * const *) p2);
}

static inline int samemtime(const struct stat *a, const struct stat *b) {
  return a->st_mtime == b->st_mtime
#ifdef __linux__
         && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec
#endif
  ;
}

static int settimes(const char *path, const struct stat *st, int symlink) {
  struct timeval tv[2];
  tv[0].tv_sec = st->st_atime;
  tv[1].tv_sec = st->st_mtime;
#ifdef __linux__
  tv[0].tv_usec = st->st_atim.tv_nsec / 1000;
  tv[1].tv_usec = st->st_mtim.tv_nsec / 1000;
#else
  tv[0].tv_usec = 0;
  tv[1].tv_usec = 0;
#endif
#if defined __linux__ || defined __FreeBSD__
  return lutimes(path, tv);
#else
  // lutimes() does not exist so there is no way to set times of a symlink
  if(symlink) return 0;
  return utimes(path, tv);
#endif
}

static void sync_file_task(void *arg) {
  struct sync_file_arg *task = arg;
  ssize_t a, b, c;
  off_t length;
  int src_fd, rc;
  char buf[IO_BUFFER_SIZE];

  // open src for reading
  src_fd = open(task->src, O_RDONLY);
  if(src_fd == -1) {
    perror(task->src);
    g_error = 1;
    goto err1;
  }

  // copy the data
  length = 0;
  while(1) {
    a = read(src_fd, buf, sizeof(buf));
    if(a == -1) {
      // read error
      perror(task->src);
      g_error = 1;
      goto err2;
    } else if(a == 0) {
      // end of file
      break;
    } else {
      c = 0;
      length += a;
      do {
        b = write(task->dst_fd, buf + c, a - c);
        if(b == -1) {
          // write error
          perror(task->dst);
          g_error = 1;
          goto err2;
        } else {
          c += b;
        }
      } while(c < a);
    }
  }
  rc = ftruncate(task->dst_fd, length);
  if(rc) {
    perror(task->dst);
    g_error = 1;
  }

  // set mode
  if(!task->dst_exists ||
     task->src_st.st_mode != task->dst_st.st_mode
  ) {
    rc = fchmod(task->dst_fd, task->src_st.st_mode);
    if(rc) {
      perror(task->dst);
      g_error = 1;
    }
  }

  // set uid and gid
  if(!task->dst_exists ||
     task->src_st.st_uid != task->dst_st.st_uid ||
     task->src_st.st_gid != task->dst_st.st_gid
  ) {
    rc = fchown(task->dst_fd, task->src_st.st_uid, task->src_st.st_gid);
    if(rc) {
      perror(task->dst);
      g_error = 1;
    }
  }
  close(src_fd);
  close(task->dst_fd);

  // set mtime
  rc = settimes(task->dst, &task->src_st, 0);
  if(rc) {
    perror(task->dst);
    g_error = 1;
  }

  free(task);
  return;

err2:
  close(src_fd);
err1:
  close(task->dst_fd);
  free(task);
}

static void sync_file(
  const struct stat *src_st,
  const char *src_path,
  const char *dst_path,
  const char *rel_path
) {
  int rc, dst_exists;
  struct stat dst_st;

  // stat dst
  rc = lstat(dst_path, &dst_st);
  dst_exists = 1;
  if(rc) {
    if(errno != ENOENT) {
      perror(dst_path);
      g_error = 1;
      return;
    }
    dst_exists = 0;
  }

  // remove dst if not a regular file
  if(dst_exists && !S_ISREG(dst_st.st_mode)) {
    if(S_ISDIR(dst_st.st_mode)) {
      unlink_dir(dst_path);
    } else {
      unlink(dst_path);
    }
    dst_exists = 0;
  }

  if(!dst_exists ||
     src_st->st_size != dst_st.st_size ||
     !samemtime(src_st, &dst_st)
  ) { // dst does not exist or file size or mtime differ
    struct sync_file_arg *task;
    int dst_fd;

    // open dst for writing
    dst_fd = open(dst_path, O_WRONLY | O_CREAT, 0600);
    if(dst_fd == -1) {
      perror(dst_path);
      g_error = 1;
      return;
    }

    if(g_verbose) printf("%s\n", rel_path);
    task = malloc(sizeof(struct sync_file_arg));
    task->src_st = *src_st;
    task->dst_st = dst_st;
    task->dst_exists = dst_exists;
    task->dst_fd = dst_fd;
    strcpy(task->src, src_path);
    strcpy(task->dst, dst_path);
    threadpool_add(&g_tp, sync_file_task, task);
  } else { // file size and mtime are the same
    // set mode
    if(src_st->st_mode != dst_st.st_mode) {
      rc = chmod(dst_path, src_st->st_mode);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        return;
      }
    }

    // set uid and gid
    if(src_st->st_uid != dst_st.st_uid ||
       src_st->st_gid != dst_st.st_gid
    ) {
      rc = chown(dst_path, src_st->st_uid, src_st->st_gid);
      if(rc) {
        perror(dst_path);
        g_error = 1;
      }
    }
  }
}

static void sync_symlink(
  const struct stat *src_st,
  const char *src_path,
  const char *dst_path,
  const char *rel_path
) {
  int rc, dst_exists;
  struct stat dst_st;
  ssize_t src_len, dst_len;
  char src_target[PATH_MAX];
  char dst_target[PATH_MAX];

  // stat dst
  rc = lstat(dst_path, &dst_st);
  dst_exists = 1;
  if(rc) {
    if(errno != ENOENT) {
      perror(dst_path);
      g_error = 1;
      return;
    }
    dst_exists = 0;
  }

  // read src target
  src_len = readlink(src_path, src_target, PATH_MAX-1);
  if(src_len == -1) {
    if(errno == ENOENT) {
      // src was removed
      if(dst_exists) {
        unlink(dst_path);
      }
    } else {
      perror(src_path);
      g_error = 1;
    }
    return;
  }
  src_target[src_len] = '\0';

  // remove dst if not a symlink
  if(dst_exists && !S_ISLNK(dst_st.st_mode)) {
    if(S_ISDIR(dst_st.st_mode)) {
      unlink_dir(dst_path);
    } else {
      unlink(dst_path);
    }
    dst_exists = 0;
  }

  if(dst_exists) {
    // read dst target
    dst_len = readlink(dst_path, dst_target, PATH_MAX-1);
    if(dst_len == -1) {
      if(errno != ENOENT) {
        rc = unlink(dst_path);
        if(rc && errno != ENOENT) {
          perror(dst_path);
          g_error = 1;
        }
      }
      dst_exists = 0;
    } else {
      dst_target[dst_len] = '\0';
      // remove dst if target differs
      if(src_len != dst_len || strcmp(src_target, dst_target) != 0) {
        unlink(dst_path);
        dst_exists = 0;
      }
    }
  }

  // create dst
  if(!dst_exists) {
    if(g_verbose) printf("%s\n", rel_path);
    rc = symlink(src_target, dst_path);
    if(rc) {
      perror(dst_path);
      g_error = 1;
      return;
    }
  }

  // set uid and gid
  if(!dst_exists ||
     src_st->st_uid != dst_st.st_uid ||
     src_st->st_gid != dst_st.st_gid
  ) {
    rc = lchown(dst_path, src_st->st_uid, src_st->st_gid);
    if(rc) {
      perror(dst_path);
      g_error = 1;
    }
  }

  // set mtime
  rc = settimes(dst_path, src_st, 1);
  if(rc) {
    perror(dst_path);
    g_error = 1;
  }
}

static void sync_dir(
  const struct stat *src_st,
  const char *src_path,
  const char *dst_path,
  const char *rel_path
) {
  int rc, dst_exists;
  struct stat dst_st, content_st;
  DIR *d;
  char *p;
  struct dirent *dirp;
  char **src_contents;
  size_t src_contents_size, src_contents_count, i;
  unsigned int src_len;
  unsigned int dst_len;
  unsigned int rel_len;
  char src_p[PATH_MAX];
  char dst_p[PATH_MAX];
  char rel_p[PATH_MAX];

  // stat dst
  rc = lstat(dst_path, &dst_st);
  dst_exists = 1;
  if(rc) {
    if(errno != ENOENT) {
      perror(dst_path);
      g_error = 1;
      return;
    }
    dst_exists = 0;
  }

  // remove dst if not a directory
  if(dst_exists && !S_ISDIR(dst_st.st_mode)) {
    unlink(dst_path);
    dst_exists = 0;
  }

  // create dst
  if(!dst_exists) {
    rc = mkdir(dst_path, 0700);
    if(rc && errno != EEXIST) {
      perror(dst_path);
      g_error = 1;
      return;
    }
  }

  // read src contents into a sorted array
  d = opendir(src_path);
  if(!d) {
    perror(src_path);
    g_error = 1;
    return;
  }
  src_contents_size = 256;
  src_contents_count = 0;
  src_contents = malloc(sizeof(char *) * src_contents_size);
  while((dirp = readdir(d))) {
    if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0) {
      continue;
    }
    p = malloc(strlen(dirp->d_name)+1);
    strcpy(p, dirp->d_name);
    src_contents[src_contents_count] = p;
    if(++src_contents_count == src_contents_size) {
      src_contents_size <<= 1;
      src_contents = realloc(src_contents, sizeof(char *) * src_contents_size);
    }
  }
  closedir(d);
  qsort(src_contents, src_contents_count, sizeof(char *), cmpstringp);

  src_len = strlen(src_path);
  strcpy(src_p, src_path);
  src_p[src_len++] = '/';
  dst_len = strlen(dst_path);
  strcpy(dst_p, dst_path);
  dst_p[dst_len++] = '/';
  rel_len = strlen(rel_path);
  strcpy(rel_p, rel_path);

  // loop through src contents
  for(i = 0; i < src_contents_count; ++i) {
    p = src_contents[i];
    strncpy(src_p + src_len, p, PATH_MAX - src_len);
    strncpy(dst_p + dst_len, p, PATH_MAX - dst_len);

    // stat src
    rc = lstat(src_p, &content_st);
    if(rc) {
      perror(src_p);
      g_error = 1;
      continue;
    }

    if(S_ISDIR(content_st.st_mode)) {
      unsigned int l = rel_len + strlen(p);
      strcpy(rel_p + rel_len, p);
      rel_p[l] = '/';
      rel_p[l+1] = '\0';
      sync_dir(&content_st, src_p, dst_p, rel_p);
    } else if(S_ISREG(content_st.st_mode)) {
      snprintf(rel_p, PATH_MAX, "%s%s", rel_path, p);
      sync_file(&content_st, src_p, dst_p, rel_p);
    } else if(S_ISLNK(content_st.st_mode)) {
      snprintf(rel_p, PATH_MAX, "%s%s", rel_path, p);
      sync_symlink(&content_st, src_p, dst_p, rel_p);
    } else {
      fprintf(stderr, "file type not supported: %s\n", src_p);
      g_error = 1;
    }
  }

  if(g_delete && dst_exists && !samemtime(src_st, &dst_st)) {
    // delete files in dst that are not in src
    d = opendir(dst_path);
    if(!d) {
      perror(dst_path);
      g_error = 1;
      return;
    }
    while((dirp = readdir(d))) {
      if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0) {
        continue;
      }
      p = dirp->d_name;
      if(!bsearch(&p, src_contents, src_contents_count, sizeof(char *), cmpstringp)) {
        snprintf(dst_p, PATH_MAX, "%s/%s", dst_path, p);
#ifdef _DIRENT_HAVE_D_TYPE
        switch(dirp->d_type) {
        case DT_UNKNOWN:
          break;
        case DT_DIR:
          goto delete_dir;
        default:
          goto delete_other;
        }
#endif
        rc = lstat(dst_p, &dst_st);
        if(rc) {
          if(errno != ENOENT) {
            perror(dst_p);
            g_error = 1;
          }
          continue;
        }
        if(S_ISDIR(dst_st.st_mode)) {
        delete_dir:
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink_dir(dst_p);
        } else {
        delete_other:
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink(dst_p);
        }
      }
    }
    closedir(d);
  }

  // set mode
  if(!dst_exists ||
     src_st->st_mode != dst_st.st_mode
  ) {
    rc = chmod(dst_path, src_st->st_mode);
    if(rc) {
      perror(dst_path);
      g_error = 1;
    }
  }

  // set uid and gid
  if(!dst_exists ||
     src_st->st_uid != dst_st.st_uid ||
     src_st->st_gid != dst_st.st_gid
  ) {
    rc = chown(dst_path, src_st->st_uid, src_st->st_gid);
    if(rc) {
      perror(dst_path);
      g_error = 1;
    }
  }

  // set mtime
  rc = settimes(dst_path, src_st, 0);
  if(rc) {
    perror(dst_path);
    g_error = 1;
  }

  // free src contents
  for(i = 0; i < src_contents_count; ++i) {
    p = src_contents[i];
    free(p);
  }
  free(src_contents);
}

static int traverse_dir_enter(void *arg, const char *src_p, const struct stat *src_st) {
  traverse_arg *t = arg;
  const char *p, *rel_p;
  char dst_p[PATH_MAX];

  p = path + t->src_root_len;
  strcpy(dst_p, t->dst_root);
  strcpy(dst_p + t->dst_root_len, p);
  rel_p = p;
  while(*p && *p == '/') ++p;
  if(*p) rel_p = p;

  // stat dst
  rc = lstat(dst_path, &dst_st);
  dst_exists = 1;
  if(rc) {
    if(errno != ENOENT) {
      perror(dst_path);
      g_error = 1;
      return;
    }
    dst_exists = 0;
  }

  // remove dst if not a directory
  if(dst_exists && !S_ISDIR(dst_st.st_mode)) {
    unlink(dst_path);
    dst_exists = 0;
  }

  // create dst
  if(!dst_exists) {
    rc = mkdir(dst_path, 0700);
    if(rc && errno != EEXIST) {
      perror(dst_path);
      g_error = 1;
      return;
    }
  }

  return 1;
}

static int traverse_dir_exit(void *arg, const char *src_p, const struct stat *src_st) {
  traverse_arg *t = arg;
  const char *p, *rel_p;
  char dst_p[PATH_MAX];

  p = path + t->src_root_len;
  strcpy(dst_p, t->dst_root);
  strcpy(dst_p + t->dst_root_len, p);
  rel_p = p;
  while(*p && *p == '/') ++p;
  if(*p) rel_p = p;

  return 0;
}

static int traverse_file(void *arg, const char *src_p, const struct stat *src_st) {
  traverse_arg *t = arg;
  const char *p, *rel_p;
  char dst_p[PATH_MAX];

  p = path + t->src_root_len;
  strcpy(dst_p, t->dst_root);
  strcpy(dst_p + t->dst_root_len, p);
  rel_p = p;
  while(*p && *p == '/') ++p;
  if(*p) rel_p = p;

  if(S_ISREG(st->st_mode)) {
    sync_file(src_st, src_p, dst_p, rel_p);
  } else if(S_ISLNK(st->st_mode)) {
    sync_symlink(src_st, src_p, dst_p, rel_p);
  } else {
    fprintf(stderr, "file type not supported: %s\n", path);
    g_error = 1;
  }
  return 0;
}

static int traverse_error(void *arg, const char *src_p, const struct stat *src_st) {
  perror(src_p);
  g_error = 1;
  return 0;
}

int main(int argc, char *argv[]) {
  int rc, opt, threads;
  const char *src_path, *dst_path;
  struct stat src_st;
  struct traverse_arg t;

  threads = 4;

  while((opt = getopt(argc, argv, "hvDj:")) != -1) {
    switch(opt) {
    case 'h':
      usage(stdout, argv[0]);
      exit(0);
    case 'v':
      g_verbose = 1;
      break;
    case 'D':
      g_delete = 0;
      break;
    case 'j':
      threads = atoi(optarg);
      if(threads <= 0) {
        fprintf(stderr, "Error: number of threads (-j) must be a positive integer\n");
        exit(2);
      }
      break;
    default:
      usage(stderr, argv[0]);
      exit(2);
    }
  }

  if(argc - optind != 2) {
    fprintf(stderr, "Error: incorrect number of arguments\n");
    usage(stderr, argv[0]);
    exit(2);
  }

  src_path = argv[optind];
  dst_path = argv[optind+1];

  // stat source
  rc = stat(src_path, &src_st);
  if(rc) {
    perror(src_path);
    exit(1);
  }
  if(!S_ISDIR(src_st.st_mode)) {
    fprintf(stderr, "Error: %s is not a directory\n", src_path);
    exit(2);
  }

  // start thread pool
  rc = threadpool_init(&g_tp, threads, threads);
  if(rc) {
    errno = rc;
    perror("threadpool_init()");
    exit(2);
  }

  t.src_root = src_path;
  t.dst_root = dst_path;
  t.src_root_len = strlen(src_path);
  t.dst_root_len = strlen(dst_path);
  mtpt(
    threads,
    MTPT_CONFIG_SORT,
    src_path,
    traverse_dir_enter,
    traverse_dir_exit,
    traverse_file,
    traverse_error,
    &t
  );
  //sync_dir(&src_st, src_path, dst_path, "");

  // shutdown thread pool
  rc = threadpool_destroy(&g_tp);
  if(rc) {
    errno = rc;
    perror("threadpool_destroy()");
    exit(2);
  }

  exit(g_error);
}
