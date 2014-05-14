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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#define DEFAULT_NTHREADS 4
#define STACKSIZE (2<<20) // 2 MB
#define KiB (1lu << 10)
#define MiB (1lu << 20)
#define GiB (1lu << 30)
#define TiB (1lu << 40)

static int g_error = 0;
static const char **g_exclude = NULL;
static size_t g_exclude_count = 0;
static int g_apparent_size = 0;
static int g_summarize = 0;
static int g_all_files = 0;
static int g_print_total = 0;
static size_t g_total = 0;
static int g_block_size = 1<<10;
static int g_human_readable = 0;
static char g_line_terminator = '\n';
static int g_one_file_system = 0;
static dev_t g_dev;

struct file_data {
  size_t size;
};

static void usage(FILE *file, const char *arg0) {
  fprintf(file,
    "Usage: %s [options] [path] ...\n"
    "Options:\n"
    "  -H    Print this message\n"
    "  -j N  Operate on N files at a time (default %d)\n"
    "  -e P  Exclude files matching P\n"
    "  -A    Print apparent sizes rather than disk usage\n"
    "  -b    Print sizes in bytes\n"
    "  -k    Print sizes in KiB (default)\n"
    "  -m    Print sizes in MiB\n"
    "  -h    Print sizes in human readable format\n"
    "  -a    Print size for all files, not just directories\n"
    "  -s    Only display a total for each argument\n"
    "  -c    Produce a grand total\n"
    "  -0    Terminate each item with a null character rather than newline\n"
    "  -x    Do not cross file system boundaries\n"
    , arg0, DEFAULT_NTHREADS);
}

static int print_size(size_t size, const char *path) {
  float f;
  if(g_human_readable) {
    if(size < KiB) { // under 1K
      return printf("%lu\t%s%c", size, path, g_line_terminator);
    }
    if(size < 10lu * KiB) { // under 10K
      f = ceilf(size * (10.0f / KiB)) * 0.1f;
      return printf("%.1fK\t%s%c", f, path, g_line_terminator);
    }
    if(size < MiB) { // under 1M
      f = ceilf(size * (1.0f / KiB));
      return printf("%dK\t%s%c", (int) f, path, g_line_terminator);
    }
    if(size < 10lu * MiB) { // under 10M
      f = ceilf(size * (10.0f / MiB)) * 0.1f;
      return printf("%.1fM\t%s%c", f, path, g_line_terminator);
    }
    if(size < GiB) { // under 1G
      f = ceilf(size * (1.0f / MiB));
      return printf("%dM\t%s%c", (int) f, path, g_line_terminator);
    }
    if(size < 10lu * GiB) { // under 10G
      f = ceilf(size * (10.0f / GiB)) * 0.1f;
      return printf("%.1fG\t%s%c", f, path, g_line_terminator);
    }
    if(size < TiB) { // under 1T
      f = ceilf(size * (1.0f / GiB));
      return printf("%dG\t%s%c", (int) f, path, g_line_terminator);
    }
    if(size < 10lu * TiB) { // under 10T
      f = ceilf(size * (10.0f / TiB)) * 0.1f;
      return printf("%.1fT\t%s%c", f, path, g_line_terminator);
    }
    f = ceilf(size * (1.0f / TiB));
    return printf("%dT\t%s%c", (int) f, path, g_line_terminator);
  } else {
    return printf("%lu\t%s%c", (size-1)/g_block_size+1, path, g_line_terminator);
  }
}

static int traverse_dir_enter(
  void *arg,
  const char *path,
  const struct stat *st,
  void *pcontinuation,
  void **continuation
) {
  const char *rel_path;

  if(g_one_file_system && g_dev != st->st_dev) return 0;

  rel_path = path + *((size_t *) arg);
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
  size_t i, size;
  struct file_data *data;

  if(g_apparent_size) {
    size = st->st_size;
  } else {
    size = st->st_blocks * DEV_BSIZE;
  }

  for(i = 0; i < entries_count; ++i) {
    data = entries[i]->data;
    if(data) {
      size += data->size;
      free(data);
    }
  }

  if(!g_summarize) {
    print_size(size, path);
  }

  data = malloc(sizeof(struct file_data));
  data->size = size;

  return data;
}

static void * traverse_file(
  void *arg,
  const char *path,
  const struct stat *st,
  void *continuation
) {
  const char *rel_path;
  struct file_data *data;
  size_t size;

  rel_path = path + *((size_t *) arg);
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

  if(g_apparent_size) {
    size = st->st_size;
  } else {
    size = st->st_blocks * DEV_BSIZE;
  }

  if(g_all_files) {
    print_size(size, path);
  }

  data = malloc(sizeof(struct file_data));
  data->size = size;

  return data;
}

static void * traverse_error(
  void *arg,
  const char *path,
  const struct stat *st,
  void *continuation
) {
  perror(path);
  g_error = 1;
  free(continuation);
  return NULL;
}

static void process_path(const char *path, size_t threads) {
  int rc;
  size_t l = strlen(path);
  struct file_data *data = NULL;
  struct stat st;

  rc = lstat(path, &st);
  if(rc) {
    perror(path);
    exit(1);
  }

  if(g_one_file_system) {
    g_dev = st.st_dev;
  }

  rc = mtpt(
    threads,
    STACKSIZE,
    MTPT_CONFIG_SORT,
    path,
    traverse_dir_enter,
    traverse_dir_exit,
    traverse_file,
    traverse_error,
    &l,
    (void **) &data
  );
  if(rc) {
    perror(path);
    g_error = 1;
  }
  if(data) {
    if(g_summarize || !S_ISDIR(st.st_mode)) {
      print_size(data->size, path);
    }
    g_total += data->size;
    free(data);
  }
}

int main(int argc, char **argv) {
  int opt;
  size_t threads;

  threads = DEFAULT_NTHREADS;

  while((opt = getopt(argc, argv, "Hj:e:aAbchkm0sx")) != -1) {
    switch(opt) {
    case 'H':
      usage(stdout, argv[0]);
      exit(0);
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
    case 'a':
      g_all_files = 1;
      break;
    case 'A':
      g_apparent_size = 1;
      break;
    case 'b':
      g_block_size = 1;
      break;
    case 'c':
      g_print_total = 1;
      break;
    case 'h':
      g_human_readable = 1;
      break;
    case 'k':
      g_block_size = 1<<10;
      break;
    case 'm':
      g_block_size = 1<<20;
      break;
    case '0':
      g_line_terminator = '\0';
      break;
    case 's':
      g_summarize = 1;
      break;
    case 'x':
      g_one_file_system = 1;
      break;
    default:
      usage(stderr, argv[0]);
      exit(2);
    }
  }

  if(g_all_files && g_summarize) {
    fprintf(stderr, "%s: cannot both summarize and show all entries\n", argv[0]);
    exit(2);
  }

  if(argc == optind) {
    process_path(".", threads);
  } else {
    for(; optind < argc; ++optind) {
      process_path(argv[optind], threads);
    }
  }

  if(g_print_total) {
    print_size(g_total, "total");
  }

  if(g_exclude) free(g_exclude);
  return g_error;
}
