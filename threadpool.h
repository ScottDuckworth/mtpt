/**
 * @file
 * @author Scott Duckworth <sduckwo@clemson.edu>
 * @brief  A thread pool
 *
 * @section LICENSE
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

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

struct threadpool_task {
  void (*routine)(void *);
  void *arg;
};

struct threadpool {
  /// mutex
  pthread_mutex_t mutex;

  /// wakes producer when signaled
  pthread_cond_t producer;

  /// wakes consumer when signaled
  pthread_cond_t consumer;

  /// non-zero when stopping
  int stop;

  /// number of threads
  size_t nthreads;

  /// number of tasks currently running
  size_t running;

  /// thread array
  pthread_t *threads;

  int (*priority_cmp)(const struct threadpool_task *, const struct threadpool_task *);

  /// task queue
  struct threadpool_task *q;
  size_t qsize;
  size_t qhead;
  size_t qcount;
  size_t qmax;
};

/// initialize a threadpool
int threadpool_init(struct threadpool *tp, size_t nthreads, size_t qmax);
int threadpool_init_prio(struct threadpool *tp, size_t nthreads, size_t qmax, int (*priority_cmp)(const struct threadpool_task *, const struct threadpool_task *));

/// add a task to a threadpool
int threadpool_add(struct threadpool *tp, void (*routine)(void *), void *arg);

/// stop and destroy a threadpool object
int threadpool_destroy(struct threadpool *tp);

#endif
