#define _BSD_SOURCE
#define _FILE_OFFSET_BITS 64
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

struct copy_task {
  struct stat src_st;
  struct stat dst_st;
  int dst_exists;
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

    rc = lstat(p, &st);
    if(rc) {
      perror(p);
      g_error = 1;
      continue;
    }

    if(S_ISDIR(st.st_mode)) {
      unlink_dir(p);
    } else {
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
#endif
#if defined __linux__ || defined __FreeBSD__
  return lutimes(path, tv);
#else
  // lutimes() does not exist so there is no way to set times of a symlink
  if(symlink) return 0;
  return utimes(path, tv);
#endif
}

static void copy_file(void *arg) {
  struct copy_task *task = arg;
  ssize_t a, b, c;
  off_t length;
  int src_fd, dst_fd, rc;
  char buf[IO_BUFFER_SIZE];

  // open src for reading
  src_fd = open(task->src, O_RDONLY);
  if(src_fd == -1) {
    perror(task->src);
    goto out1;
  }

  // open dst for writing
  dst_fd = open(task->dst, O_WRONLY | O_CREAT, 0600);
  if(dst_fd == -1) {
    perror(task->dst);
    goto out2;
  }

  // copy the data
  length = 0;
  while(1) {
    a = read(src_fd, buf, sizeof(buf));
    if(a == -1) {
      // read error
      perror(task->src);
      g_error = 1;
      goto out3;
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
          perror(task->dst);
          g_error = 1;
          goto out3;
        } else {
          c += b;
        }
      } while(c < a);
    }
  }
  rc = ftruncate(dst_fd, length);
  if(rc) {
    perror(task->dst);
    g_error = 1;
  }

out3:
  // set mode
  if(!task->dst_exists ||
     task->src_st.st_mode != task->dst_st.st_mode
  ) {
    rc = fchmod(dst_fd, task->src_st.st_mode);
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
    rc = fchown(dst_fd, task->src_st.st_uid, task->src_st.st_gid);
    if(rc) {
      perror(task->dst);
      g_error = 1;
    }
  }

  // close file
  close(dst_fd);

  // set mtime
  rc = settimes(task->dst, &task->src_st, 0);
  if(rc) {
    perror(task->dst);
    g_error = 1;
  }
out2:
  close(src_fd);
out1:
  free(task);
}

static int cmpstringp(const void *p1, const void *p2) {
  return strcmp(*(char * const *) p1, *(char * const *) p2);
}

static void sync_dir(const char *src_path, const char *dst_path, const char *rel_path) {
  DIR *d;
  char *p;
  int rc, dst_exists;
  struct stat src_st, dst_st;
  struct dirent *dirp;
  char **src_contents;
  size_t src_contents_size, src_contents_count, i;
  char src_p[PATH_MAX];
  char dst_p[PATH_MAX];

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

  // synchronize files from src to dst
  for(i = 0; i < src_contents_count; ++i) {
    p = src_contents[i];
    snprintf(src_p, PATH_MAX, "%s/%s", src_path, p);
    snprintf(dst_p, PATH_MAX, "%s/%s", dst_path, p);

    // stat src
    rc = lstat(src_p, &src_st);
    if(rc) {
      perror(src_p);
      g_error = 1;
      continue;
    }

    // stat dst
    rc = lstat(dst_p, &dst_st);
    dst_exists = 1;
    if(rc) {
      if(errno != ENOENT) {
        perror(dst_p);
        g_error = 1;
        continue;
      }
      dst_exists = 0;
    }

    if(S_ISDIR(src_st.st_mode)) {
      char rel_p[PATH_MAX];

      // remove dst if not a directory
      if(dst_exists && !S_ISDIR(dst_st.st_mode)) {
        unlink(dst_p);
        dst_exists = 0;
      }

      // create dst
      if(!dst_exists) {
        rc = mkdir(dst_p, 0700);
        if(rc && errno != EEXIST) {
          perror(dst_p);
          g_error = 1;
          continue;
        }
      }

      // recurse
      snprintf(rel_p, PATH_MAX, "%s%s/", rel_path, p);
      sync_dir(src_p, dst_p, rel_p);

      // set mode
      if(!dst_exists ||
         src_st.st_mode != dst_st.st_mode
      ) {
        rc = chmod(dst_p, src_st.st_mode);
        if(rc) {
          perror(dst_p);
          g_error = 1;
        }
      }

      // set uid and gid
      if(!dst_exists ||
         src_st.st_uid != dst_st.st_uid ||
         src_st.st_gid != dst_st.st_gid
      ) {
        rc = chown(dst_p, src_st.st_uid, src_st.st_gid);
        if(rc) {
          perror(dst_p);
          g_error = 1;
        }
      }

      // set mtime
      rc = settimes(dst_p, &src_st, 0);
      if(rc) {
        perror(dst_p);
        g_error = 1;
      }
    } else if(S_ISLNK(src_st.st_mode)) {
      ssize_t src_len, dst_len;
      char src_target[PATH_MAX];
      char dst_target[PATH_MAX];

      // read src target
      src_len = readlink(src_p, src_target, PATH_MAX-1);
      if(src_len == -1) {
        if(errno == ENOENT) {
          // src was removed
          if(dst_exists) {
            unlink(dst_p);
          }
        } else {
          perror(src_p);
          g_error = 1;
        }
        continue;
      }
      src_target[src_len] = '\0';

      // remove dst if not a symlink
      if(dst_exists && !S_ISLNK(dst_st.st_mode)) {
        if(S_ISDIR(dst_st.st_mode)) {
          unlink_dir(dst_p);
        } else {
          unlink(dst_p);
        }
        dst_exists = 0;
      }

      if(dst_exists) {
        // read dst target
        dst_len = readlink(dst_p, dst_target, PATH_MAX-1);
        if(dst_len == -1) {
          if(errno == ENOENT) {
            dst_exists = 0;
          } else {
            perror(dst_p);
            g_error = 1;
            continue;
          }
        } else {
          dst_target[dst_len] = '\0';
          // remove dst if target differs
          if(src_len != dst_len || strcmp(src_target, dst_target) != 0) {
            unlink(dst_p);
            dst_exists = 0;
          }
        }
      }

      // create dst
      if(!dst_exists) {
        if(g_verbose) printf("%s%s\n", rel_path, p);
        rc = symlink(src_target, dst_p);
        if(rc) {
          perror(dst_p);
          g_error = 1;
          continue;
        }
      }

      // set uid and gid
      if(!dst_exists ||
         src_st.st_uid != dst_st.st_uid ||
         src_st.st_gid != dst_st.st_gid
      ) {
        rc = lchown(dst_p, src_st.st_uid, src_st.st_gid);
        if(rc) {
          perror(dst_p);
          g_error = 1;
        }
      }

      // set mtime
      rc = settimes(dst_p, &src_st, 1);
      if(rc) {
        perror(dst_p);
        g_error = 1;
      }
    } else if(S_ISREG(src_st.st_mode)) {
      // remove dst if not a regular file
      if(dst_exists && !S_ISREG(dst_st.st_mode)) {
        if(S_ISDIR(dst_st.st_mode)) {
          unlink_dir(dst_p);
        } else {
          unlink(dst_p);
        }
        dst_exists = 0;
      }

      if(!dst_exists ||
         src_st.st_size  != dst_st.st_size ||
         !samemtime(&src_st, &dst_st)
      ) { // dst does not exist or file size or mtime differ
        if(g_verbose) printf("%s%s\n", rel_path, p);
        struct copy_task *task = malloc(sizeof(struct copy_task));
        task->src_st = src_st;
        task->dst_st = dst_st;
        strcpy(task->src, src_p);
        strcpy(task->dst, dst_p);
        threadpool_add(&g_tp, copy_file, task);
      } else { // file size and mtime are the same
        // set mode
        if(src_st.st_mode != dst_st.st_mode) {
          rc = chmod(dst_p, src_st.st_mode);
          if(rc) {
            perror(dst_p);
            g_error = 1;
          }
        }

        // set uid and gid
        if(src_st.st_uid != dst_st.st_uid ||
           src_st.st_gid != dst_st.st_gid
        ) {
          rc = chown(dst_p, src_st.st_uid, src_st.st_gid);
          if(rc) {
            perror(dst_p);
            g_error = 1;
          }
        }
      }
    } else {
      fprintf(stderr, "file type not supported: %s\n", src_p);
      g_error = 1;
    }
  }

  if(g_delete) {
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
        if(dirp->d_type == DT_DIR) {
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink_dir(dst_p);
          continue;
        } else if(dirp->d_type == DT_REG) {
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink(dst_p);
          continue;
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
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink_dir(dst_p);
        } else {
          if(g_verbose) printf("deleting %s\n", dst_p);
          unlink(dst_p);
        }
      }
    }
    closedir(d);
  }

  // free src contents
  for(i = 0; i < src_contents_count; ++i) {
    p = src_contents[i];
    free(p);
  }
  free(src_contents);
}

int main(int argc, char *argv[]) {
  int rc, opt, threads;
  const char *src_path, *dst_path;
  struct stat src_st;

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

  // create destination if does not exist
  rc = mkdir(dst_path, 0700);
  if(rc && errno != EEXIST) {
    perror(dst_path);
    exit(1);
  }

  // start thread pool
  rc = threadpool_init(&g_tp, threads, threads);
  if(rc) {
    errno = rc;
    perror("threadpool_init()");
    exit(2);
  }

  sync_dir(src_path, dst_path, "");

  // shutdown thread pool
  rc = threadpool_destroy(&g_tp);
  if(rc) {
    errno = rc;
    perror("threadpool_destroy()");
    exit(2);
  }

  // set mode
  rc = chmod(dst_path, src_st.st_mode);
  if(rc) {
    perror(dst_path);
    g_error = 1;
  }

  // set uid and gid
  rc = chown(dst_path, src_st.st_uid, src_st.st_gid);
  if(rc) {
    perror(dst_path);
    g_error = 1;
  }

  // set mtime
  rc = settimes(dst_path, &src_st, 0);
  if(rc) {
    perror(dst_path);
    g_error = 1;
  }

  exit(g_error);
}
