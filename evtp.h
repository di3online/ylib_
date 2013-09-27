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

/* This is a thread pool implementation for convenient use with libev[1]. It
 * mimics the libev and libeio[2] API to some extent.
 *
 * The initial thread pool code was based on the threadpool library[3] by
 * Juliusz Chroboczek, but the current implementation is quite different.
 * Still, if you're looking for a generic thread pool that doesn't rely on
 * libev, that might be a good choice.
 *
 * 1. http://software.schmorp.de/pkg/libev
 * 2. http://software.schmorp.de/pkg/libeio
 * 3. https://github.com/jech/threadpool
 *
 * TODO:
 * - Automatically kill some idle threads? The application can already use
 *   evtp_maxthreads() to achieve the same effect.
 * - Cancellation?
 * - Allow done_func = NULL? (Implies auto-free()-work-object-when-done)
 */

#ifndef EVTP_H
#define EVTP_H

#include <ev.h>
#include <stdlib.h>

typedef struct evtp_t evtp_t; /* Opaque */
typedef struct evtp_work_t evtp_work_t;

typedef void (*evtp_func_t)(evtp_work_t *);

struct evtp_work_t {
	/* Public */
	void *data;
	/* Private */
	evtp_func_t work_func;
	evtp_func_t done_func;
	evtp_work_t *next;
};


/* Create a new thread pool. May return NULL if malloc() fails.
 *
 * The thread pool object itself does not hold a reference on the ev loop. Each
 * evtp_work_t object in the queue, however, does. This way, ev_run() may quit
 * even when a thread pool object is alive, but it will never quit as long
 * there is still work to do. */
evtp_t *evtp_create(EV_P_ int maxthreads);


/* Dynamically change the maximum number of threads. New threads will be
 * created when this value is increased and there is enough work to do. If this
 * value is decreased, some threads will be killed to ensure that the number of
 * threads will remain below the configured maximum.
 *
 * Note that a thread can only be killed when it's not running a work_func(),
 * so there may be a small delay before the new maxthreads value is honored.
 * Temporarily setting maxthreads to '0' is a valid way to pause processing of
 * queued work objects.
 * Temporarily setting maxthreads to '0' directly followed by resetting it to
 * its previous value is a valid way to kill all idle threads.
 *
 * Returns -1 if we have work queued, but pthread_create() failed and we have
 * no other threads running (fatal), returns 0 if pthread_create() failed but
 * we still have a worker thread (recoverable), or 1 if everything went fine.
 */
int evtp_maxthreads(evtp_t *evtp, int maxthreads);


/* Submit work to the thread pool. Returns -1 if pthread_create() failed and we
 * have no threads running (fatal), 0 if pthread_create() failed but we still
 * have a worker thread (recoverable), or 1 if everything went fine.
 *
 * work_func() will be called in a worker thread. A short while after
 * work_func() returns, done_func() will be called in the thread that runs
 * ev_run() on the ev loop given to evtp_create().
 *
 * The *work object must remain valid until done_func() is called.
 */
int evtp_submit(evtp_work_t *work, evtp_t *evtp, evtp_func_t work_func, evtp_func_t done_func);


/* Convenience function that allocates a new work object for you. You must
 * still free() the object in the done_func! */
static inline evtp_work_t *evtp_submit_new(evtp_t *evtp, evtp_func_t work_func, evtp_func_t done_func, void *data) {
	evtp_work_t *w = calloc(1, sizeof(evtp_work_t));
	w->data = data;
	if(evtp_submit(w, evtp, work_func, done_func) < 0) {
		free(w);
		w = NULL;
	}
	return w;
}


/* Destroy a thread pool. If there is still work scheduled, this function does
 * nothing and returns -1. If force is non-zero, then the thread pool is
 * destroyed even if there is still work scheduled. In that case, the work_func
 * and/or done_func callbacks will NOT be run for those objects.
 *
 * This function blocks until all threads have been destroyed and returns 1 on
 * success. */
int evtp_destroy(evtp_t *threadpool, int force);

#endif

/* vim: set noet sw=4 ts=4: */
