#ifndef EXCLUDE_H
#define EXCLUDE_H

#include <sys/types.h>

int excluded(
  const char * const *patterns,
  size_t npatterns,
  const char *path,
  int isdir
);

#endif
