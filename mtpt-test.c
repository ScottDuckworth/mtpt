#include "mtpt.h"
#include <stdio.h>
#include <stdlib.h>

struct totals {
  off_t filesize;
  size_t dirs;
  size_t files;
  size_t symlinks;
};

static int dir_enter(
  void *arg,
  const char *path,
  const struct stat *st,
  void **continuation
) {
  printf(">>> %s\n", path);
  return 1;
}

static void * dir_exit(
  void *arg,
  const char *path,
  const struct stat *st,
  void *continuation,
  mtpt_dir_entry_t **entries,
  size_t entries_count
) {
  struct totals *t = malloc(sizeof(struct totals));
  struct totals *p;
  size_t i;

  t->filesize = 0;
  t->dirs = 1;
  t->files = 0;
  t->symlinks = 0;

  for(i = 0; i < entries_count; ++i) {
    p = entries[i]->data;
    // p might be NULL if this entry had an error (error() returns NULL)
    if(p) {
      t->filesize += p->filesize;
      t->dirs += p->dirs;
      t->files += p->files;
      t->symlinks += p->symlinks;
      free(p);
    }
  }
  printf("<<< %s\n", path);
  return t;
}

static void * file(void *arg, const char *path, const struct stat *st) {
  struct totals *t = malloc(sizeof(struct totals));
  t->filesize = 0;
  t->dirs = 0;
  t->files = 0;
  t->symlinks = 0;
  if(S_ISREG(st->st_mode)) {
    t->filesize = st->st_size;
    t->files = 1;
  } else if(S_ISLNK(st->st_mode)) {
    t->symlinks = 1;
  }
  printf("    %s\n", path);
  return t;
}

static void * error(void *arg, const char *path, const struct stat *st, void *continuation) {
  perror(path);
  return NULL;
}

int main(int argc, char **argv) {
  struct totals *t;
  int rc;

  rc = mtpt(
    4,         // nthreads
    MTPT_CONFIG_FILE_TASKS | MTPT_CONFIG_SORT, // config
    argv[1],   // path
    dir_enter, // dir_enter_method
    dir_exit,  // dir_exit_method
    file,      // file_method
    error,     // error_method
    NULL,      // arg
    (void **) &t // data
  );
  if(rc) {
    perror(argv[1]);
    return 1;
  }
  if(t) {
    printf("Total file size:       %9ld\n", t->filesize);
    printf("Number of files:       %9ld\n", t->files);
    printf("Number of symlinks:    %9ld\n", t->symlinks);
    printf("Number of directories: %9ld\n", t->dirs);
    free(t);
  }
  return 0;
}
