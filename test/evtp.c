/* Copyright (c) 2012 Yoran Heling

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

#if !defined(BENCH) && defined(NDEBUG)
#error These tests should not be compiled with -DNDEBUG!
#endif

#ifdef WORK
#define BENCH_LOOPS 8000
#else
#define BENCH_LOOPS 50000
#endif

#include "evtp.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static evtp_t *tp;
static char data[51];

static void done_cb(evtp_work_t *w);
static void work_cb(evtp_work_t *w);


static void done_cb(evtp_work_t *w) {
	static int done = 0;
#ifdef BENCH
	static int loops = 0;
#endif
	int i;

	assert(*((char *)w->data) == 1);
	free(w);
	done++;

	if(done == 1) {
		evtp_maxthreads(tp, 4);
		for(i=1; i<50; i++)
			evtp_submit_new(tp, work_cb, done_cb, data+i);
	}

#ifdef BENCH
	if(done == 50) {
		loops++;
		if(loops < BENCH_LOOPS) {
			done = 0;
			memset(data, 0, sizeof(data));
			evtp_submit_new(tp, work_cb, done_cb, data);
		}
	}
#endif
}


static void work_cb(evtp_work_t *w) {
	char *d = w->data;
	assert(*d == 0);
#ifndef WORK
	*d = 1;
#else
	double x = (double)(((char *)w->data)-data);
	int i;
	for(i = 0; i<100; i++)
		x = cos(x);
	*d = x > -10.0; /* Always true, but I hope that the compiler doesn't know that */
#endif
}


int main(int argc, char **argv) {
	ev_default_loop(0);
	tp = evtp_create(EV_DEFAULT_ 0);

	evtp_submit_new(tp, work_cb, done_cb, data);
	evtp_maxthreads(tp, 1);

	ev_run(EV_DEFAULT_ 0);

	evtp_destroy(tp, 1);
	tp = NULL;

	int i;
	for(i=0; i<50; i++)
		assert(data[i] == 1);

	return 0;
}

/* vim: set noet sw=4 ts=4: */
