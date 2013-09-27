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

#ifdef NDEBUG
#error These tests should not be compiled with -DNDEBUG!
#endif


#include "ecbuf.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>


int main(int argc, char **argv) {
	ecbuf_t(int) lst, cpy;
	int i, j, r, w, *p;

	/* Handy debugging function:
		fprintf(stderr, "l = %d, o = %d, b = %d, cn = %d, bn = %d\n", cpy.v.l, cpy.v.o, cpy.v.b, cpy.v.cn, cpy.v.bn);
		fprintf(stderr, "l = %d, o = %d, b = %d, cn = %d, bn = %d\n", lst.v.l, lst.v.o, lst.v.b, lst.v.cn, lst.v.bn);
	*/

	ecbuf_init(lst);
	for(i=0; i<100; i++) {
		assert(ecbuf_empty(lst));
		assert(ecbuf_len(lst) == 0);
		ecbuf_push(lst, i);
		assert(ecbuf_len(lst) == 1);

		cpy = lst;
		assert(ecbuf_unpush(cpy) == i);
		assert(ecbuf_len(cpy) == 0);

		assert(ecbuf_pop(lst) == i);
		assert(ecbuf_empty(lst));
		assert(ecbuf_len(lst) == 0);
	}
	ecbuf_destroy(lst);

	ecbuf_init(lst);
	assert(ecbuf_empty(lst));
	for(i=0; i<100; i++) {
		ecbuf_push(lst, i);
		assert(!ecbuf_empty(lst));
		assert(ecbuf_len(lst) == i+1);
	}
	cpy = lst;
	for(i=0; i<100; i++) {
		assert(!ecbuf_empty(cpy));
		assert(ecbuf_unpush(cpy) == 99-i);
		assert(ecbuf_len(cpy) == 99-i);
	}
	assert(ecbuf_empty(cpy));
	for(i=0; i<100; i++) {
		assert(!ecbuf_empty(lst));
		assert(ecbuf_pop(lst) == i);
		assert(ecbuf_len(lst) == 99-i);
	}
	assert(ecbuf_empty(lst));
	ecbuf_destroy(lst);

	ecbuf_init(lst);
	for(i=0; i<100; i++) {
		for(j=0; j<i; j++)
			ecbuf_push(lst, (i<<16) + j);
		for(j=0; j<i; j++)
			assert(ecbuf_unpush(lst) == (i<<16) + i - 1 - j);
		for(j=0; j<i; j++)
			ecbuf_push(lst, (i<<16) + j);
		for(j=0; j<i; j++)
			assert(ecbuf_pop(lst) == (i<<16) + j);
	}
	ecbuf_destroy(lst);

	ecbuf_init(lst);
	for(i=0; i<31; i++)
		ecbuf_push(lst, i);
	assert(ecbuf_len(lst) == 31);    /* [0..30] */
	for(i=0; i<10; i++)
		assert(ecbuf_pop(lst) == i);
	assert(ecbuf_len(lst) == 21);    /* [10..30] */
	for(i=0; i<20; i++)
		ecbuf_push(lst, 1000+i);
	assert(ecbuf_len(lst) == 21+20); /* [10..30, 1000..1019] */
	for(i=10; i<31; i++)
		assert(ecbuf_pop(lst) == i);
	assert(ecbuf_len(lst) == 20);    /* [1000..1019] */
	for(i=0; i<20; i++)
		assert(ecbuf_pop(lst) == 1000+i);
	assert(ecbuf_len(lst) == 0);     /* [] */
	ecbuf_destroy(lst);

	ecbuf_init(lst);
	r = w = 0;
	for(i=0; i<100; i++) {
		for(j=0; j<100; j++) {
			ecbuf_push(lst, w);
			w++;
			assert(ecbuf_len(lst) == w-r);
		}
		for(j=0; j<99; j++) {
			assert(ecbuf_len(lst) == w-r);
			assert(ecbuf_peek(lst) == r);
			assert(ecbuf_pop(lst) == r++);
		}
	}
	assert(ecbuf_len(lst) == 100);
	for(i=0; i<100; i++)
		assert(ecbuf_pop(lst) == r++);
	assert(ecbuf_empty(lst));
	ecbuf_destroy(lst);

	/* Some tests that specifically bring out some special cases. */
	ecbuf_init(lst);
	j = lst.v.bn*2;
	for(i=0; i<j; i++)
		ecbuf_push(lst, 2);
	assert(lst.v.bn == j);
	for(i=0; i<5; i++)
		assert(ecbuf_pop(lst) == 2);
	assert(ecbuf_pushp(lst) == lst.a); /* It should have wrapped */
	assert(lst.v.bn == j);
	ecbuf_destroy(lst);

	/* Add 32, rm 10, add 32+10, rm 32-10-3. In this situation the buffer is
	 * not "full", but a push should still expand the buffer because b is set.
	 */
	ecbuf_init(lst);
	assert(lst.v.bn == 32);
	for(i=0; i<32; i++)
		ecbuf_push(lst, 2);
	for(i=0; i<10; i++)
		assert(ecbuf_pop(lst) == 2);
	for(i=0; i<32+10; i++)
		ecbuf_push(lst, 2);
	assert(lst.v.bn == 32*2);
	for(i=0; i<32-10-3; i++)
		assert(ecbuf_pop(lst) == 2);
	assert(lst.v.l == 64-(32-10-3));
	p = ecbuf_pushp(lst);
	*p = 1; /* Hits a valgrind error if the buffer hasn't been expanded correctly. */
	assert(p == lst.a+64);
	assert(lst.v.bn == 128);
	ecbuf_destroy(lst);

	memset(&lst, 0, sizeof(lst)); /* Let valgrind detect a leak */
	return 0;
}

/* vim: set noet sw=4 ts=4: */
