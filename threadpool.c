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
    if(++tp->qhead == tp->qsize)
      tp->qhead = 0;
    if(tp->qcount-- == tp->qsize)
      pthread_cond_signal(&tp->producer);
    ++tp->running;
    pthread_mutex_unlock(&tp->mutex);
    (*task.routine)(task.arg);
    pthread_mutex_lock(&tp->mutex);
    --tp->running;
  }
}

int threadpool_init(struct threadpool *tp, unsigned int threads, unsigned int qsize) {
  int i, rc;

  assert(threads > 0);
  assert(qsize > 0);

  rc = pthread_mutex_init(&tp->mutex, NULL);
  if(rc) goto err0;
  rc = pthread_cond_init(&tp->producer, NULL);
  if(rc) goto err1;
  rc = pthread_cond_init(&tp->consumer, NULL);
  if(rc) goto err2;

  tp->stop = 0;
  tp->n = threads;
  tp->running = 0;
  tp->qsize = qsize;
  tp->qhead = 0;
  tp->qcount = 0;
  tp->q = malloc(sizeof(struct threadpool_task) * qsize);
  if(!tp->q) goto err3;
  tp->threads = malloc(sizeof(pthread_t) * threads);
  if(!tp->threads) goto err4;
  pthread_mutex_lock(&tp->mutex);
  for(i = 0; i < threads; ++i) {
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
  unsigned int n;

  rc = pthread_mutex_lock(&tp->mutex);
  if(rc) return rc;
  if(tp->stop) {
    pthread_mutex_unlock(&tp->mutex);
    return EINVAL;
  }
  while(tp->qcount == tp->qsize) {
    pthread_cond_wait(&tp->producer, &tp->mutex);
  }
  n = tp->qhead + tp->qcount;
  if(n >= tp->qsize)
    n -= tp->qsize;
  tp->q[n].routine = routine;
  tp->q[n].arg = arg;
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

  for(i = 0; i < tp->n; ++i) {
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
