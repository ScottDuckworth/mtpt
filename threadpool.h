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
  unsigned int n;

  /// number of tasks currently running
  unsigned int running;

  /// thread array
  pthread_t *threads;

  /// task queue
  struct threadpool_task *q;
  unsigned int qsize;
  unsigned int qhead;
  unsigned int qcount;
};

/// initialize a threadpool
int threadpool_init(struct threadpool *tp, unsigned int threads, unsigned int qsize);

/// add a task to a threadpool
int threadpool_add(struct threadpool *tp, void (*routine)(void *), void *arg);

/// stop and destroy a threadpool object
int threadpool_destroy(struct threadpool *tp);

#endif
