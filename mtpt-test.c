#include "mtpt.h"
#include <stdio.h>
#include <stdlib.h>

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
  printf("<<< %s\n", path);
  return NULL;
}

static void * file(void *arg, const char *path, const struct stat *st) {
  printf("    %s\n", path);
  return NULL;
}

static void * error(void *arg, const char *path, const struct stat *st) {
  perror(path);
  if(st) fprintf(stderr, "%o\n", st->st_mode);
  return NULL;
}

int main(int argc, char **argv) {
  int rc;
  rc = mtpt(
    4,         // nthreads
    MTPT_CONFIG_FILE_TASKS, // config
    argv[1],   // path
    dir_enter, // dir_enter_method
    dir_exit,  // dir_exit_method
    file,      // file_method
    error,     // error_method
    NULL,      // arg
    NULL       // data
  );
  if(rc) {
    perror(argv[1]);
    return 1;
  }
  return 0;
}
