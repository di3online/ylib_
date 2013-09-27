/* Copyright (c) 2013 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if defined(EV_CONFIG_H)
#include EV_CONFIG_H
#elif defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include "evtp.h"

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>


typedef struct evtp_queue_t {
	evtp_work_t *first;
	evtp_work_t *last;
} evtp_queue_t;


struct evtp_t {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t die_cond;
	evtp_queue_t work, results;
	int maxthreads, threads, idle, kill;
	ev_async async;
#if EV_MULTIPLICITY
	struct ev_loop *loop;
#endif
};


static evtp_work_t *evtp_dequeue(evtp_queue_t *queue) {
    evtp_work_t *item;

    if(queue->first == NULL)
        return NULL;

    item = queue->first;
    queue->first = item->next;
    if(item->next == NULL)
        queue->last = NULL;
    return item;
}


static void evtp_enqueue(evtp_queue_t *queue, evtp_work_t *item) {
    item->next = NULL;
    if(queue->last)
        queue->last->next = item;
    else
        queue->first = item;
    queue->last = item;
}


static void evtp_async(EV_P_ ev_async *async, int revents) {
	evtp_t *tp = async->data;
	evtp_work_t *items;

	pthread_mutex_lock(&tp->lock);
	items = tp->results.first;
	tp->results.first = NULL;
	tp->results.last = NULL;
	pthread_mutex_unlock(&tp->lock);

	while(items) {
		evtp_work_t *first = items;
		evtp_func_t func = first->done_func;
		items = items->next;
		/* Don't use 'first' after this function, application may have free()d it. */
		func(first);
		ev_unref(EV_A);
	}
}


evtp_t *evtp_create(EV_P_ int maxthreads) {
	evtp_t *tp = calloc(1, sizeof(evtp_t));
	tp->maxthreads = maxthreads;

	pthread_mutex_init(&tp->lock, NULL);
	pthread_cond_init(&tp->cond, NULL);
	pthread_cond_init(&tp->die_cond, NULL);

	ev_async_init(&tp->async, evtp_async);
	ev_async_start(EV_A_ &tp->async);
	ev_unref(EV_A);
	tp->async.data = tp;
#if EV_MULTIPLICITY
	tp->loop = loop;
#endif

	return tp;
}


static void *evtp_thread(void *data) {
	evtp_t *tp = data;
#if EV_MULTIPLICITY
	struct ev_loop *loop = tp->loop;
#endif

	pthread_mutex_lock(&tp->lock);
	while(1) {
		if(tp->kill > 0) {
			tp->kill--;
			if(tp->kill > 0)
				pthread_cond_signal(&tp->cond);
			pthread_cond_signal(&tp->die_cond);
			break;
		}

		evtp_work_t *work = evtp_dequeue(&tp->work);
		if(work) {
			pthread_mutex_unlock(&tp->lock);
			work->work_func(work);
			pthread_mutex_lock(&tp->lock);
			evtp_enqueue(&tp->results, work);
			ev_async_send(EV_A_ &tp->async);
			continue;
		}

		tp->idle++;
		pthread_cond_wait(&tp->cond, &tp->lock);
		tp->idle--;
	}

	tp->threads--;
	pthread_mutex_unlock(&tp->lock);
	return NULL;
}


/* Must be called while the lock is held */
static int evtp_spawn(evtp_t *tp) {
	if(tp->kill) { /* Why spawn a new thread when we've just attempted to kill one? */
		tp->kill--;
		return 1;
	}
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(&thread, &attr, evtp_thread, tp);
	if(r) {
		errno = r;
		return tp->threads ? 0 : -1;
	}
	tp->threads++;
	return 1;
}


int evtp_maxthreads(evtp_t *tp, int maxthreads) {
	int r = 1;
	pthread_mutex_lock(&tp->lock);
	tp->maxthreads = maxthreads;

	if(tp->threads > maxthreads) {
		tp->kill = tp->threads - maxthreads;
		if(tp->idle)
			pthread_cond_signal(&tp->cond);
	}

	evtp_work_t *work = tp->work.first;
	while(work && tp->threads-tp->kill < maxthreads && r >= 0) {
		r = evtp_spawn(tp);
		work = work->next;
	}

	pthread_mutex_unlock(&tp->lock);
	return r;
}


int evtp_submit(evtp_work_t *work, evtp_t *tp, evtp_func_t work_func, evtp_func_t done_func) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = tp->loop;
#endif
	int r = 1;

	work->work_func = work_func;
	work->done_func = done_func;

	pthread_mutex_lock(&tp->lock);
	if(tp->idle <= tp->kill && tp->threads-tp->kill < tp->maxthreads)
		r = evtp_spawn(tp);

	if(r >= 0) {
		evtp_enqueue(&tp->work, work);
		ev_ref(EV_A);
		if(tp->idle)
			pthread_cond_signal(&tp->cond);
	}

	pthread_mutex_unlock(&tp->lock);
	return r;
}


int evtp_destroy(evtp_t *tp, int force) {
#if EV_MULTIPLICITY
	struct ev_loop *loop = tp->loop;
#endif

	pthread_mutex_lock(&tp->lock);
	if(!force && (tp->work.first || tp->results.first)) {
		pthread_mutex_unlock(&tp->lock);
		return -1;
	}

	tp->kill = tp->threads;
	pthread_cond_signal(&tp->cond);
	while(tp->threads > 0)
		pthread_cond_wait(&tp->die_cond, &tp->lock);
	pthread_mutex_unlock(&tp->lock);

	pthread_cond_destroy(&tp->cond);
	pthread_cond_destroy(&tp->die_cond);
	pthread_mutex_destroy(&tp->lock);
	ev_ref(EV_A);
	ev_async_stop(EV_A_ &tp->async);
	free(tp);
	return 1;
}

/* vim: set noet sw=4 ts=4: */
