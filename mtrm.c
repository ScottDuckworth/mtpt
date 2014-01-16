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
#include "exclude.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_NTHREADS 4

static int g_error = 0;
static int g_verbose = 0;
static const char **g_exclude = NULL;
static size_t g_exclude_count = 0;

static void usage(FILE *file, const char *arg0) {
  fprintf(file,
    "Usage: %s [options] path ...\n"
    "Options:\n"
    "  -h    Print this message\n"
    "  -v    Be verbose\n"
    "  -j N  Operate on N files at a time (default %d)\n"
    "  -e P  Exclude files matching P\n"
    , arg0, DEFAULT_NTHREADS);
}

static int traverse_dir_enter(
  void *arg,
  const char *path,
  const struct stat *st,
  void **continuation
) {
  const char *rel_path;

  rel_path = path + (size_t) arg;
  if(*rel_path) {
    ++rel_path;
  } else {
    rel_path = ".";
  }

  return !excluded(g_exclude, g_exclude_count, rel_path, 1);
}

static void * traverse_dir_exit(
  void *arg,
  const char *path,
  const struct stat *st,
  void *continuation,
  mtpt_dir_entry_t **entries,
  size_t entries_count
) {
  int rc;
  size_t i;

  for(i = 0; i < entries_count; ++i) {
    if(entries[i]->data == NULL) return NULL;
  }
  rc = rmdir(path);
  if(rc) {
    perror(path);
    g_error = 1;
    return NULL;
  }
  if(g_verbose) printf("removed directory: `%s'\n", path);
  return (void *) -1l;
}

static void * traverse_file(
  void *arg,
  const char *path,
  const struct stat *st
) {
  int rc;
  const char *rel_path;

  rel_path = path + (size_t) arg;
  if(*rel_path) {
    ++rel_path;
  } else {
    const char *p;
    p = rel_path = path;
    while(*p) {
      if(*p++ == '/') rel_path = p;
    }
  }

  if(excluded(g_exclude, g_exclude_count, rel_path, 0))
    return NULL;

  rc = unlink(path);
  if(rc) {
    perror(path);
    g_error = 1;
    return NULL;
  }
  if(g_verbose) printf("removed `%s'\n", path);
  return (void *) -1l;
}

static void * traverse_error(
  void *arg,
  const char *path,
  const struct stat *st,
  void *continuation
) {
  perror(path);
  g_error = 1;
  return NULL;
}

int main(int argc, char **argv) {
  int rc, opt;
  size_t threads;

  threads = DEFAULT_NTHREADS;

  while((opt = getopt(argc, argv, "hvj:e:")) != -1) {
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
    case 'e':
      g_exclude = realloc(g_exclude, (g_exclude_count+1) * sizeof(char *));
      g_exclude[g_exclude_count++] = optarg;
      break;
    default:
      usage(stderr, argv[0]);
      exit(2);
    }
  }

  if(argc == optind) {
    fprintf(stderr, "Error: path not given\n");
    usage(stderr, argv[0]);
    exit(2);
  }

  for(; optind < argc; ++optind) {
    rc = mtpt(
      threads,
      MTPT_CONFIG_FILE_TASKS | MTPT_CONFIG_SORT,
      argv[optind],
      traverse_dir_enter,
      traverse_dir_exit,
      traverse_file,
      traverse_error,
      (void *) strlen(argv[optind]),
      NULL
    );
    if(rc) {
      perror(argv[optind]);
      g_error = 1;
    }
  }
  if(g_exclude) free(g_exclude);
  return g_error;
}
