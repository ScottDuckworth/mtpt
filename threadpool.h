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

  /// task queue
  struct threadpool_task *q;
  size_t qsize;
  size_t qhead;
  size_t qcount;
  size_t qmax;
};

/// initialize a threadpool
int threadpool_init(struct threadpool *tp, size_t nthreads, size_t qmax);

/// add a task to a threadpool
int threadpool_add(struct threadpool *tp, void (*routine)(void *), void *arg);

/// stop and destroy a threadpool object
int threadpool_destroy(struct threadpool *tp);

#endif
