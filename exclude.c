#include "exclude.h"
#include <fnmatch.h>
#include <limits.h>
#include <string.h>

int excluded(
  const char * const *patterns,
  size_t npatterns,
  const char *path,
  int isdir
) {
  size_t i, l;
  const char *p, *tail;
  char pattern[PATH_MAX];

  for(i = 0; i < npatterns; ++i) {
    p = patterns[i];
    l = strlen(p);
    if(p[l-1] == '/') {
      if(!isdir) return 0;
      strcpy(pattern, p);
      pattern[--l] = '\0';
      p = pattern;
    }
    if(p[0] == '/') {
      if(fnmatch(p+1, path, FNM_PATHNAME) == 0) return 1;
    } else {
      if(fnmatch(p, path, FNM_PATHNAME) == 0) return 1;
      tail = path;
      while(*tail) {
        if(*tail++ == '/') {
          if(fnmatch(p, tail, FNM_PATHNAME) == 0) return 1;
        }
      }
    }
  }
  return 0;
}
