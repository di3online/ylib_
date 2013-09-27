/* Copyright (c) 2012 Yoran Heling & Sybren van Elderen

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

/* This is an efficient circular buffer implementation that automatically
 * expands the buffer when it's full. As such, it behaves like any other
 * unbounded FIFO queue.
 *
 * Usage:
 *
 *    ecbuf_t(int) queue, iter;
 *    ecbuf_init(queue);
 *
 *    // Writing (by value)
 *    ecbuf_push(queue, 3);
 *    ecbuf_push(queue, 5);
 *    ecbuf_push(queue, 7);
 *    // Writing (by pointer)
 *    int *ptr = ecbuf_pushp(queue);
 *    *ptr = 11;
 *
 *    // = 3
 *    printf("Least recently queued = %d\n", ecbuf_peek(queue));
 *    // = 4
 *    printf("Queue length = %d\n", ecbuf_len(queue));
 *
 *    // Iterating over the items in the same order that they were _push()ed.
 *    iter = queue;
 *    while(!ecbuf_empty(iter))
 *        printf("Item: %d\n", ecbuf_pop(iter));
 *
 *    // Iterating over the items in the reverse order.
 *    iter = queue;
 *    while(!ecbuf_empty(iter))
 *        printf("Item: %d\n", ecbuf_unpush(iter));
 *
 *    // Reading items in the same order that they were _push()ed.
 *    // Only difference is that this doesn't use an iterator. Same can be
 *    // done with _unpush() to read from the other end of the queue.
 *    while(!ecbuf_empty(queue))
 *        printf("Item: %d\n", ecbuf_pop(queue));
 *
 *    ecbuf_destroy(queue);
 *
 *    // Similar to ecbuf_pushp(), there is ecbuf_popp() and ecbuf_unpushp()
 *    // which return a pointer into the circular buffer. These remain valid
 *    // until the next ecbuf_push(). Iterators, as illustrated above, also
 *    // remain valid until the next ecbuf_push(). Modifying a value returned
 *    // from an iterator will modify it in the main list, too.
 *
 * This library requires the 'typeof' operator of C99 and may therefore not
 * work in C++. A workaround for this should be quite trivial, however.
 *
 * The concept is explained at
 * http://blog.labix.org/2010/12/23/efficient-algorithm-for-expanding-circular-buffers
 *
 * This implementation is slightly different, in that it offers more operations
 * and requires one less variable to keep track of.
 */

#ifndef ECBUF_H
#define ECBUF_H

#include <stdlib.h>


/* The variables are:
 *  l: Number of items in the queue
 *  o: Index we're going to read from in the next pop()
 *  b: Index of the last written item before the buffer has been expanded.
 * cn: Number of slots in the cirbular buffer
 * bn: Number of slots in the complete buffer
 */
typedef struct {
	int l, o, b, cn, bn;
} ecbuf_vars_t;

#define ecbuf_t(type) struct {\
		ecbuf_vars_t v;\
		type *a;\
	}


#define ecbuf_init(e) do {\
		(e).v.bn = (e).v.cn = 32;\
		(e).v.o = (e).v.l = 0;\
		(e).v.b = -1;\
		(e).a = malloc((e).v.bn*sizeof(*(e).a));\
	} while(0)

#define ecbuf_destroy(e) free((e).a)

/* Number of items queued. */
#define ecbuf_len(e) ((e).v.l)

#define ecbuf_empty(e) (ecbuf_len(e) == 0)

/* Peek into the queue, requires !ecbuf_empty(v) */
#define ecbuf_peek(e) ((e).a[(e).v.o])


#if defined(__GNUC__) && (__GNUC__ > 2) && defined(__OPTIMIZE__)
#define ecbuf__unlikely(expr) (__builtin_expect(expr, 0))
#else
#define ecbuf__unlikely(expr) (expr)
#endif

static inline void *ecbuf__push(ecbuf_vars_t *v, void **a, size_t alen) {
	/* The algortihm is something like:
	 * 1. If the buffer is full, "grow" it
	 * 2. Calculate next write position
	 * 3. If write position is outside of buffer (can happen), grow it
	 * It may be possible to combine some of these steps and shorten the
	 * function, but that doesn't look very easy. :-(
	 */
	int i, obn = v->bn;
	/* 1 */
	if(ecbuf__unlikely(v->l == v->bn)) {
		v->bn <<= 1;
		if(v->cn == obn) {
			if(v->o) v->b = (v->o - 1 + v->cn) & (v->cn-1);
			else     v->cn = v->bn;
		}
	}
	/* 2 */
	i = v->l + v->o - v->b - 1;
	if(v->bn == v->cn)    i &= v->cn-1;
	else if(v->o <= v->b) i += v->cn;
	/* 3 */
	if(ecbuf__unlikely(i >= v->bn)) v->bn <<= 1;
	if(ecbuf__unlikely(v->bn != obn)) *a = realloc(*a, v->bn*alen);
	v->l++;
	return ((char *)*a)+alen*i;
}

#define ecbuf_pushp(e) ((typeof((e).a))ecbuf__push(&(e).v, (void **)&(e).a, sizeof(*(e).a)))
#define ecbuf_push(e, x) (*ecbuf_pushp(e) = (x))


static inline int ecbuf__unpush(ecbuf_vars_t *v) {
	int i = v->l + v->o - 1;
	if(v->bn != v->cn) i -= v->b + 1;
	if(v->o <= v->b)   i += v->cn;
	i &= v->bn-1;
	v->l--;
	if(ecbuf__unlikely(i == v->cn)) {
		v->b = -1;
		/* This change causes bn to be smaller than the actual size of the
		 * allocated buffer, but this isn't too important. If the extra space
		 * is needed in the future, _push() will just do a (no-op) realloc()
		 * and the extra space will be available again. */
		v->bn = v->cn;
	}
	return i;
}

#define ecbuf_unpushp(e) ((e).a + ecbuf__unpush(&(e).v))
#define ecbuf_unpush(e) (*ecbuf_unpushp(e))


static inline int ecbuf__pop(ecbuf_vars_t *v) {
	int l = v->o;
	v->l--;
	if(ecbuf__unlikely(v->o == v->b)) {
		v->o = v->cn;
		v->cn = v->bn;
		v->b = -1;
	} else if(v->o == v->cn-1)
		v->o = 0;
	else
		v->o++;
	return l;
}

#define ecbuf_popp(e) ((e).a + ecbuf__pop(&(e).v))
#define ecbuf_pop(e) (*ecbuf_popp(e))

#endif

/* vim: set noet sw=4 ts=4: */
