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
#include <pthread.h>
#include "mtpt.h"
#include "exclude.h"
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
#define DEFAULT_NTHREADS 4

struct traverse_arg {
  const char *src_root;
  const char *dst_root;
  size_t src_root_len;
  size_t dst_root_len;
};

struct traverse_continuation {
  int dst_exists;
  struct stat dst_st;
  struct stat src_st;
};

struct hardlink_entry {
  dev_t src_dev;
  ino_t src_ino;
  dev_t dst_dev;
  ino_t dst_ino;
  char *dst_path;
};

static uid_t g_euid;
static int g_error = 0;
static int g_verbose = 0;
static int g_preserve_mode = 0;
static int g_preserve_ownership = 0;
static int g_preserve_mtime = 0;
static int g_preserve_hardlinks = 0;
static int g_delete = 1;
static const char **g_exclude = NULL;
static size_t g_exclude_count = 0;
static const char **g_exclude_delete = NULL;
static size_t g_exclude_delete_count = 0;
#ifdef __linux__
static int g_subsecond = 0;
#endif
static int g_modify_window = 0;
static int g_one_file_system = 0;
static dev_t g_dev;
static struct hardlink_entry **g_hardlinks;
static size_t g_hardlinks_size, g_hardlinks_count;
static pthread_mutex_t g_hardlinks_mutex;

static void usage(FILE *file, const char *arg0) {
  fprintf(file,
    "Usage: %s [options] source destination\n"
    "Options:\n"
    "  -h    Print this message\n"
    "  -v    Be verbose\n"
    "  -j N  Copy N files at a time (default %d)\n"
    "  -a    Archive; equals -pot\n"
    "  -p    Preserve permissions\n"
    "  -o    Preserve ownership (only preserves user if root)\n"
    "  -t    Preserve modification times\n"
    "  -H    Preserve hard links\n"
    "  -D    Do not delete files not in source from destination\n"
    "  -e P  Exclude files matching P\n"
    "  -E P  Exclude and delete from destination files matching P\n"
#ifdef __linux__
    "  -s    Use sub-second precision when comparing mtimes\n"
#endif
    "  -w S  mtime can be within S seconds to assume equal\n"
    "  -x    Do not cross file system boundaries\n"
    , arg0, DEFAULT_NTHREADS);
}

static void *xmalloc(size_t size) {
  void *p = malloc(size);
  if(p == NULL) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }
  return p;
}

static void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if(p == NULL) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }
  return p;
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
#ifdef _DIRENT_HAVE_D_TYPE
    delete_dir:
#endif
      unlink_dir(p);
    } else {
#ifdef _DIRENT_HAVE_D_TYPE
    delete_other:
#endif
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

static inline int samemtime(const struct stat *a, const struct stat *b) {
  long int diff_s = a->st_mtime - b->st_mtime;

#ifdef __linux__
  if(g_subsecond) {
    long int diff_ns = a->st_mtim.tv_nsec - b->st_mtim.tv_nsec;
    if(g_modify_window) {
      if(labs(diff_ns) >= 1000) {
        if(diff_ns < 0) diff_s -= 1;
        if(diff_s < 0) diff_s = -diff_s - 1;
      }
      return diff_s < g_modify_window;
    } else {
      return diff_s == 0 && labs(diff_ns) < 1000;
    }
  }
#endif

  if(g_modify_window) {
    return labs(diff_s) <= g_modify_window;
  } else {
    return diff_s == 0;
  }
}

static int settimes(const char *path, const struct stat *st) {
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
  return utimes(path, tv);
}

static int hardlink_entry_pcmp_src(const void *a, const void *b) {
  const struct hardlink_entry * const *hla = a;
  const struct hardlink_entry * const *hlb = b;
  return memcmp(*hla, *hlb, sizeof(dev_t) + sizeof(ino_t));
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

  if(excluded(g_exclude_delete, g_exclude_delete_count, rel_path, 0)) {
    if(dst_exists) {
      if(S_ISDIR(dst_st.st_mode)) {
        unlink_dir(dst_path);
      } else {
        unlink(dst_path);
      }
    }
    return;
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
    ssize_t a, b, c;
    off_t length;
    int src_fd, dst_fd;
    char buf[IO_BUFFER_SIZE];

    // open src for reading
    src_fd = open(src_path, O_RDONLY);
    if(src_fd == -1) {
      if(errno != ENOENT) {
        perror(src_path);
        g_error = 1;
      }
      return;
    }

    if(g_verbose) printf("%s\n", rel_path);

    if(dst_exists && g_euid != 0) {
      // make sure I can write to dst
      rc = access(dst_path, W_OK);
      if(rc) {
        if(errno == EACCES) {
          mode_t m = dst_st.st_mode | S_IWUSR;
          if(dst_st.st_uid != g_euid) {
            // if I'm not the owner of the file then perhaps I have access
            // through the group
            m |= S_IWGRP;
          }
          rc = chmod(dst_path, m);
          if(rc) {
            perror(dst_path);
            g_error = 1;
            close(src_fd);
            return;
          }
        } else if(errno == ENOENT) {
          dst_exists = 0;
        } else {
          perror(dst_path);
          g_error = 1;
          close(src_fd);
          return;
        }
      }
    }

    // open dst for writing
    dst_fd = open(dst_path, O_WRONLY | O_CREAT, 0600);
    if(dst_fd == -1) {
      perror(dst_path);
      g_error = 1;
      close(src_fd);
      return;
    }

    // copy the data
    length = 0;
    while(1) {
      a = read(src_fd, buf, sizeof(buf));
      if(a == -1) {
        // read error
        perror(src_path);
        g_error = 1;
        close(src_fd);
        close(dst_fd);
        return;
      } else if(a == 0) {
        // end of file
        break;
      } else {
        c = 0;
        length += a;
        do {
          b = write(dst_fd, buf + c, a - c);
          if(b == -1) {
            // write error
            perror(dst_path);
            g_error = 1;
            close(src_fd);
            close(dst_fd);
            return;
          } else {
            c += b;
          }
        } while(c < a);
      }
    }
    close(src_fd);
    rc = ftruncate(dst_fd, length);
    if(rc) {
      perror(dst_path);
      g_error = 1;
      close(dst_fd);
      return;
    }

    if(g_preserve_mode) {
      if(!dst_exists ||
         src_st->st_mode != dst_st.st_mode
      ) {
        rc = fchmod(dst_fd, src_st->st_mode);
        if(rc) {
          perror(dst_path);
          g_error = 1;
          close(dst_fd);
          return;
        }
      }
    }

    if(g_preserve_ownership) {
      if(!dst_exists ||
         (g_euid == 0 && src_st->st_uid != dst_st.st_uid) ||
         src_st->st_gid != dst_st.st_gid
      ) {
        uid_t uid = g_euid == 0 ? src_st->st_uid : (uid_t)-1;
        rc = fchown(dst_fd, uid, src_st->st_gid);
        if(rc) {
          perror(dst_path);
          g_error = 1;
          close(dst_fd);
          return;
        }
      }
    }

    close(dst_fd);

    if(g_preserve_mtime) {
      rc = settimes(dst_path, src_st);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        return;
      }
    }
  } else { // file size and mtime are the same
    if(g_preserve_mode) {
      if(src_st->st_mode != dst_st.st_mode) {
        rc = chmod(dst_path, src_st->st_mode);
        if(rc) {
          perror(dst_path);
          g_error = 1;
          return;
        }
      }
    }

    if(g_preserve_ownership) {
      if((g_euid == 0 && src_st->st_uid != dst_st.st_uid) ||
         src_st->st_gid != dst_st.st_gid
      ) {
        uid_t uid = g_euid == 0 ? src_st->st_uid : (uid_t)-1;
        rc = chown(dst_path, uid, src_st->st_gid);
        if(rc) {
          perror(dst_path);
          g_error = 1;
          return;
        }
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

  if(excluded(g_exclude_delete, g_exclude_delete_count, rel_path, 0)) {
    if(dst_exists) {
      if(S_ISDIR(dst_st.st_mode)) {
        unlink_dir(dst_path);
      } else {
        unlink(dst_path);
      }
    }
    return;
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
          return;
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

  if(g_preserve_ownership) {
    if(!dst_exists ||
       (g_euid == 0 && src_st->st_uid != dst_st.st_uid) ||
       src_st->st_gid != dst_st.st_gid
    ) {
      uid_t uid = g_euid == 0 ? src_st->st_uid : (uid_t)-1;
      rc = lchown(dst_path, uid, src_st->st_gid);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        return;
      }
    }
  }
}

static void sync_special(
  const struct stat *src_st,
  const char *src_path,
  const char *dst_path,
  const char *rel_path,
  mode_t fmt,
  int usedev
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

  if(excluded(g_exclude_delete, g_exclude_delete_count, rel_path, 0)) {
    if(dst_exists) {
      if(S_ISDIR(dst_st.st_mode)) {
        unlink_dir(dst_path);
      } else {
        unlink(dst_path);
      }
    }
    return;
  }

  // remove dst if format differs
  if(dst_exists && (S_IFMT & dst_st.st_mode) != fmt) {
    if(S_ISDIR(dst_st.st_mode)) {
      unlink_dir(dst_path);
    } else {
      unlink(dst_path);
    }
    dst_exists = 0;
  }

  if(usedev) {
    if(dst_exists && src_st->st_dev != dst_st.st_dev) {
      unlink(dst_path);
      dst_exists = 0;
    }
  }

  // create dst
  if(!dst_exists) {
    if(g_verbose) printf("%s\n", rel_path);
    if(usedev) {
      rc = mknod(dst_path, src_st->st_mode, src_st->st_dev);
    } else {
      rc = mknod(dst_path, src_st->st_mode, 0);
    }
    if(rc) {
      perror(dst_path);
      g_error = 1;
      return;
    }
  } else if(g_preserve_mode && src_st->st_mode != dst_st.st_mode) {
    rc = chmod(dst_path, src_st->st_mode);
    if(rc) {
      perror(dst_path);
      g_error = 1;
      return;
    }
  }

  if(g_preserve_ownership) {
    if(!dst_exists ||
       (g_euid == 0 && src_st->st_uid != dst_st.st_uid) ||
       src_st->st_gid != dst_st.st_gid
    ) {
      uid_t uid = g_euid == 0 ? src_st->st_uid : (uid_t)-1;
      rc = chown(dst_path, uid, src_st->st_gid);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        return;
      }
    }
  }
}

static int traverse_dir_enter(
  void *arg,
  const char *src_path,
  const struct stat *src_st,
  void *pcontinuation,
  void **continuation
) {
  struct traverse_arg *t = arg;
  struct traverse_continuation *cont;
  const char *p, *rel_path;
  int rc, dst_exists;
  struct stat dst_st;
  char dst_path[PATH_MAX];

  if(g_one_file_system && g_dev != src_st->st_dev) return 0;

  strcpy(dst_path, t->dst_root);
  p = src_path + t->src_root_len;
  if(*p) {
    strcpy(dst_path + t->dst_root_len, p);
    rel_path = p + 1;
  } else {
    rel_path = ".";
  }

  if(excluded(g_exclude, g_exclude_count, rel_path, 1)) return 0;

  if(g_verbose > 1) printf(">>> %s/\n", src_path);

  // stat dst
  rc = lstat(dst_path, &dst_st);
  dst_exists = 1;
  if(rc) {
    if(errno != ENOENT) {
      perror(dst_path);
      g_error = 1;
      return 0;
    }
    dst_exists = 0;
  }

  if(excluded(g_exclude_delete, g_exclude_delete_count, rel_path, 1)) {
    if(dst_exists) {
      if(S_ISDIR(dst_st.st_mode)) {
        unlink_dir(dst_path);
      } else {
        unlink(dst_path);
      }
    }
    return 0;
  }

  // remove dst if not a directory
  if(dst_exists && !S_ISDIR(dst_st.st_mode)) {
    unlink(dst_path);
    dst_exists = 0;
  }

  // create dst
  if(!dst_exists) {
    if(g_verbose) printf("%s/\n", rel_path);

    rc = mkdir(dst_path, 0700);
    if(rc && errno != EEXIST) {
      perror(dst_path);
      g_error = 1;
      return 0;
    }
  }

  cont = malloc(sizeof(struct traverse_continuation));
  cont->dst_exists = dst_exists;
  cont->dst_st = dst_st;
  cont->src_st = *src_st;
  *continuation = cont;

  return 1;
}

int find_entry(const void *p1, const void *p2) {
  const char *key = p1;
  const mtpt_dir_entry_t * const *entry = p2;
  return strcmp(key, (*entry)->name);
}

static void * traverse_dir_exit(
  void *arg,
  const char *src_path,
  const struct stat *src_st,
  void *continuation,
  mtpt_dir_entry_t **entries,
  size_t entries_count
) {
  struct traverse_arg *t = arg;
  struct traverse_continuation *cont = continuation;
  const char *p;
  DIR *d;
  struct dirent *dirp;
  int rc;
  struct stat st;
  char dst_path[PATH_MAX];
  char dst_p[PATH_MAX];

  p = src_path + t->src_root_len;
  strcpy(dst_path, t->dst_root);
  strcpy(dst_path + t->dst_root_len, p);

  if(g_delete && cont->dst_exists && !samemtime(&cont->src_st, &cont->dst_st)) {
    // delete files in dst that are not in src
    d = opendir(dst_path);
    if(!d) {
      perror(dst_path);
      g_error = 1;
      goto out;
    } else {
      while((dirp = readdir(d))) {
        if(strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0) {
          continue;
        }
        p = dirp->d_name;
        if(!bsearch(p, entries, entries_count, sizeof(mtpt_dir_entry_t *), find_entry)) {
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
          rc = lstat(dst_p, &st);
          if(rc) {
            if(errno != ENOENT) {
              perror(dst_p);
              g_error = 1;
            }
            continue;
          }
          if(S_ISDIR(st.st_mode)) {
#ifdef _DIRENT_HAVE_D_TYPE
          delete_dir:
#endif
            if(g_verbose) printf("deleting %s\n", dst_p);
            unlink_dir(dst_p);
          } else {
#ifdef _DIRENT_HAVE_D_TYPE
          delete_other:
#endif
            if(g_verbose) printf("deleting %s\n", dst_p);
            unlink(dst_p);
          }
        }
      }
      closedir(d);
    }
  }

  if(g_verbose > 1) printf("<<< %s/\n", src_path);

  if(g_preserve_mode) {
    if(!cont->dst_exists ||
       cont->src_st.st_mode != cont->dst_st.st_mode
    ) {
      rc = chmod(dst_path, cont->src_st.st_mode);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        goto out;
      }
    }
  }

  if(g_preserve_ownership) {
    if(!cont->dst_exists ||
       (g_euid == 0 && cont->src_st.st_uid != cont->dst_st.st_uid) ||
       cont->src_st.st_gid != cont->dst_st.st_gid
    ) {
      uid_t uid = g_euid == 0 ? cont->src_st.st_uid : (uid_t)-1;
      rc = chown(dst_path, uid, cont->src_st.st_gid);
      if(rc) {
        perror(dst_path);
        g_error = 1;
        goto out;
      }
    }
  }

  if(g_preserve_mtime) {
    rc = settimes(dst_path, src_st);
    if(rc) {
      perror(dst_path);
      g_error = 1;
      goto out;
    }
  }

out:
  free(cont);
  return NULL;
}

static void * traverse_file(
  void *arg,
  const char *src_path,
  const struct stat *src_st,
  void *continuation
) {
  struct traverse_arg *t = arg;
  const char *p, *rel_path;
  struct hardlink_entry hl, *hlp, **hlpp;
  struct stat dst_st;
  int rc;
  char dst_path[PATH_MAX];

  strcpy(dst_path, t->dst_root);
  p = src_path + t->src_root_len;
  if(*p) {
    strcpy(dst_path + t->dst_root_len, p);
    rel_path = p + 1;
  } else {
    p = rel_path = src_path;
    while(*p) {
      if(*p++ == '/') rel_path = p;
    }
  }

  if(excluded(g_exclude, g_exclude_count, rel_path, 0)) return NULL;

  if(g_preserve_hardlinks && src_st->st_nlink > 1) {
    hl.src_dev = src_st->st_dev;
    hl.src_ino = src_st->st_ino;
    hlp = &hl;
    pthread_mutex_lock(&g_hardlinks_mutex);
    hlpp = bsearch(&hlp, g_hardlinks, g_hardlinks_count, sizeof(struct hardlink_entry *), hardlink_entry_pcmp_src);
    if(hlpp) {
      hlp = *hlpp;
      // the inode has already been sync'd, just link to it
      rc = lstat(dst_path, &dst_st);
      if(rc == 0) {
        if(hlp->dst_dev == dst_st.st_dev && hlp->dst_ino == dst_st.st_ino) {
          // hardlink is already present
          pthread_mutex_unlock(&g_hardlinks_mutex);
          return NULL;
        }
        // another file is present, remove it first
        if(S_ISDIR(dst_st.st_mode)) {
          unlink_dir(dst_path);
        } else {
          unlink(dst_path);
        }
      } else if(errno != ENOENT) {
        // error stat'ing dst, give up
        perror(dst_path);
        g_error = 1;
        pthread_mutex_unlock(&g_hardlinks_mutex);
        return NULL;
      }
      if(g_verbose) printf("%s\n", rel_path);
      // make the link
      rc = link(hlp->dst_path, dst_path);
      if(rc) {
        perror(dst_path);
        g_error = 1;
      }
      pthread_mutex_unlock(&g_hardlinks_mutex);
      return NULL;
    }
    // hold g_hardlinks_mutex until later
  }

  if(S_ISREG(src_st->st_mode)) {
    sync_file(src_st, src_path, dst_path, rel_path);
  } else if(S_ISLNK(src_st->st_mode)) {
    sync_symlink(src_st, src_path, dst_path, rel_path);
  } else if(S_ISFIFO(src_st->st_mode)) {
    sync_special(src_st, src_path, dst_path, rel_path, S_IFIFO, 0);
  } else if(S_ISBLK(src_st->st_mode)) {
    sync_special(src_st, src_path, dst_path, rel_path, S_IFBLK, 1);
  } else if(S_ISCHR(src_st->st_mode)) {
    sync_special(src_st, src_path, dst_path, rel_path, S_IFCHR, 1);
  } else if(S_ISSOCK(src_st->st_mode)) {
    sync_special(src_st, src_path, dst_path, rel_path, S_IFSOCK, 0);
  } else {
    fprintf(stderr, "file type not supported: %s\n", rel_path);
    g_error = 1;
  }

  if(g_preserve_hardlinks && src_st->st_nlink > 1) {
    rc = lstat(dst_path, &dst_st);
    if(rc) {
      perror(dst_path);
      g_error = 1;
      return NULL;
    }
    if(g_hardlinks_count == g_hardlinks_size) {
      g_hardlinks_size <<= 1;
      g_hardlinks = xrealloc(g_hardlinks, sizeof(struct hardlink_entry) * g_hardlinks_size);
    }
    hlp = xmalloc(sizeof(hl));
    hlp->src_dev = hl.src_dev;
    hlp->src_ino = hl.src_ino;
    hlp->dst_dev = dst_st.st_dev;
    hlp->dst_ino = dst_st.st_ino;
    hlp->dst_path = xmalloc(strlen(dst_path) + 1);
    strcpy(hlp->dst_path, dst_path);
    g_hardlinks[g_hardlinks_count++] = hlp;
    qsort(g_hardlinks, g_hardlinks_count, sizeof(struct hardlink_entry *), hardlink_entry_pcmp_src);
    pthread_mutex_unlock(&g_hardlinks_mutex);
  }

  return NULL;
}

static void * traverse_error(
  void *arg,
  const char *src_path,
  const struct stat *src_st,
  void *continuation
) {
  perror(src_path);
  g_error = 1;
  return NULL;
}

int main(int argc, char *argv[]) {
  int rc, opt;
  size_t threads;
  const char *src_path, *dst_path;
  struct traverse_arg t;
  struct stat st;

  g_euid = geteuid();
  threads = DEFAULT_NTHREADS;

  while((opt = getopt(argc, argv, "hvj:apotHDe:E:sw:x")) != -1) {
    switch(opt) {
    case 'h':
      usage(stdout, argv[0]);
      exit(0);
    case 'v':
      g_verbose += 1;
      break;
    case 'j':
      threads = atoi(optarg);
      if(threads <= 0) {
        fprintf(stderr, "Error: number of threads (-j) must be a positive integer\n");
        exit(2);
      }
      break;
    case 'a':
      g_preserve_mode = 1;
      g_preserve_ownership = 1;
      g_preserve_mtime = 1;
      break;
    case 'p':
      g_preserve_mode = 1;
      break;
    case 'o':
      g_preserve_ownership = 1;
      break;
    case 't':
      g_preserve_mtime = 1;
      break;
    case 'H':
      g_preserve_hardlinks = 1;
      break;
    case 'D':
      g_delete = 0;
      break;
    case 'e':
      g_exclude = realloc(g_exclude, (g_exclude_count+1) * sizeof(char *));
      g_exclude[g_exclude_count++] = optarg;
      break;
    case 'E':
      g_exclude_delete = realloc(g_exclude_delete, (g_exclude_delete_count+1) * sizeof(char *));
      g_exclude_delete[g_exclude_delete_count++] = optarg;
      break;
    case 's':
#ifdef __linux__
      g_subsecond = 1;
#else
      fprintf(stderr, "Error: -m only valid on Linux\n");
      exit(2);
#endif
      break;
    case 'w':
      g_modify_window = atoi(optarg);
      if(g_modify_window < 0) {
        fprintf(stderr, "Error: mtime window (-w) must be a non-negative integer\n");
        exit(2);
      }
      break;
    case 'x':
      g_one_file_system = 1;
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

  rc = lstat(src_path, &st);
  if(rc) {
    perror(src_path);
    exit(1);
  }

  if(g_one_file_system) {
    g_dev = st.st_dev;
  }

  if(g_preserve_hardlinks) {
    g_hardlinks_count = 0;
    g_hardlinks_size = 32;
    g_hardlinks = xmalloc(sizeof(struct hardlink_entry *) * g_hardlinks_size);
    pthread_mutex_init(&g_hardlinks_mutex, NULL);
  }

  t.src_root = src_path;
  t.dst_root = dst_path;
  t.src_root_len = strlen(src_path);
  t.dst_root_len = strlen(dst_path);
  rc = mtpt(
    threads,
    MTPT_CONFIG_FILE_TASKS | MTPT_CONFIG_SORT,
    src_path,
    traverse_dir_enter,
    traverse_dir_exit,
    traverse_file,
    traverse_error,
    &t,
    NULL
  );
  if(rc) {
    perror(src_path);
    g_error = 1;
  }

  if(g_preserve_hardlinks) {
    size_t i;
    for(i = 0; i < g_hardlinks_count; ++i) {
      free(g_hardlinks[i]->dst_path);
      free(g_hardlinks[i]);
    }
    free(g_hardlinks);
  }

  if(g_exclude) free(g_exclude);
  if(g_exclude_delete) free(g_exclude_delete);
  exit(g_error);
}
