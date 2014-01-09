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

#include "threadpool.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

static void * threadpool_consumer(void *arg) {
  struct threadpool *tp = arg;
  struct threadpool_task task;

  pthread_mutex_lock(&tp->mutex);
  while(1) {
    // wait for a task to arrive in the queue
    while(tp->qcount == 0) {
      if(tp->stop) {
        pthread_mutex_unlock(&tp->mutex);
        return NULL;
      }
      pthread_cond_wait(&tp->consumer, &tp->mutex);
    }
    task = tp->q[tp->qhead];
    tp->qhead = (tp->qhead + 1) & (tp->qsize - 1);
    if(tp->qcount-- == tp->qmax)
      pthread_cond_signal(&tp->producer);
    ++tp->running;
    pthread_mutex_unlock(&tp->mutex);
    (*task.routine)(task.arg);
    pthread_mutex_lock(&tp->mutex);
    --tp->running;
  }
}

static inline int ilog2(size_t val) {
  int i;
  assert(val != 0);
  assert(val <= ~(((size_t)-1) >> 1));
  for(i = -1; val; ++i) val >>= 1;
  return i;
}

int threadpool_init(struct threadpool *tp, size_t nthreads, size_t qmax) {
  int i, rc;

  assert(nthreads > 0);

  rc = pthread_mutex_init(&tp->mutex, NULL);
  if(rc) goto err0;
  rc = pthread_cond_init(&tp->producer, NULL);
  if(rc) goto err1;
  rc = pthread_cond_init(&tp->consumer, NULL);
  if(rc) goto err2;

  tp->stop = 0;
  tp->nthreads = nthreads;
  tp->running = 0;
  tp->qsize = qmax == 0 ? 8 : 1 << (ilog2(qmax-1)+1);
  tp->qhead = 0;
  tp->qcount = 0;
  tp->qmax = qmax;
  tp->q = malloc(sizeof(struct threadpool_task) * tp->qsize);
  if(!tp->q) goto err3;
  tp->threads = malloc(sizeof(pthread_t) * nthreads);
  if(!tp->threads) goto err4;
  pthread_mutex_lock(&tp->mutex);
  for(i = 0; i < nthreads; ++i) {
    rc = pthread_create(&tp->threads[i], NULL, threadpool_consumer, tp);
    if(rc) goto err5;
  }
  pthread_mutex_unlock(&tp->mutex);
  return 0;

err5:
  pthread_mutex_unlock(&tp->mutex);
  for(--i; i >= 0; --i) {
    pthread_cancel(tp->threads[i]);
    pthread_join(tp->threads[i], NULL);
  }
  free(tp->threads);
err4:
  free(tp->q);
err3:
  pthread_cond_destroy(&tp->consumer);
err2:
  pthread_cond_destroy(&tp->producer);
err1:
  pthread_mutex_destroy(&tp->mutex);
err0:
  return rc;
}

int threadpool_add(struct threadpool *tp, void (*routine)(void *), void *arg) {
  int rc;
  size_t i, mask;
  struct threadpool_task *task;

  rc = pthread_mutex_lock(&tp->mutex);
  if(rc) return rc;
  if(tp->stop) {
    pthread_mutex_unlock(&tp->mutex);
    return EINVAL;
  }
  mask = tp->qsize - 1;
  if(tp->qmax == 0) {
    if(tp->qcount == tp->qsize) {
      if(tp->qsize == ~(((size_t)-1) >> 1)) return ENOMEM;
      task = malloc(sizeof(struct threadpool_task) * (tp->qsize << 1));
      if(task == NULL) return errno;
      for(i = 0; i < tp->qsize; ++i) {
        task[i] = tp->q[(tp->qhead + tp->qcount + i) & mask];
      }
      free(tp->q);
      tp->q = task;
      tp->qsize <<= 1;
      tp->qhead = 0;
      mask = tp->qsize - 1;
    }
  } else {
    while(tp->qcount == tp->qmax) {
      pthread_cond_wait(&tp->producer, &tp->mutex);
    }
  }
  task = &tp->q[(tp->qhead + tp->qcount) & mask];
  task->routine = routine;
  task->arg = arg;
  if(tp->qcount++ == 0)
    pthread_cond_signal(&tp->consumer);
  pthread_mutex_unlock(&tp->mutex);
  return 0;
}

int threadpool_destroy(struct threadpool *tp) {
  int i, rc;

  pthread_mutex_lock(&tp->mutex);
  tp->stop = 1;
  pthread_cond_broadcast(&tp->consumer);
  pthread_mutex_unlock(&tp->mutex);

  for(i = 0; i < tp->nthreads; ++i) {
    rc = pthread_join(tp->threads[i], NULL);
    if(rc) return rc;
  }
  free(tp->threads);
  free(tp->q);
  rc = pthread_cond_destroy(&tp->consumer);
  if(rc) return rc;
  rc = pthread_cond_destroy(&tp->producer);
  if(rc) return rc;
  rc = pthread_mutex_destroy(&tp->mutex);
  if(rc) return rc;
  return 0;
}
