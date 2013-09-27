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


#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "ecbuf.h"

/* A quick-and-dirty linked list-based queue. Uses some ugly pointer/typeof
 * tricks for efficient genericity. */
#define llbuf_t(type) struct { void *next, *prev; type t; }
#define llbuf_init(e) do { (e).next = (e).prev = NULL; } while(0)
#define llbuf_destroy(e) /* I'm assuming it's empty, so no destroy necessary */
#define llbuf_empty(e) ((e).next == NULL)
#define llbuf_push(e, v) do {\
		typeof(&e) n = malloc(sizeof(e));\
		n->t = (v);\
		n->next = (e).next;\
		n->prev = NULL;\
		(e).next = n;\
		if(!(e).prev) (e).prev = n;\
		if(n->next) ((typeof(&e))n->next)->prev = n;\
	} while(0)
#define llbuf_pop(e) do {\
		typeof(&e) n = (e).prev;\
		if((e).next == n) (e).next = NULL;\
		if(n->prev) ((typeof(&e))n->prev)->next = NULL;\
		(e).prev = n->prev;\
		free(n);\
	} while(0)


int main(int argc, char **argv) {

#define COUNT 10000000
#define RUN(name, type, val) do {\
		int j; name##_t(type) lst; name##_init(lst);\
		for(j=0; j<rounds; j++) {\
			int k;\
			for(k=0; k<num; k++)\
				name##_push(lst, val);\
			for(k=0; k<num; k++)\
				name##_pop(lst);\
		}\
		name##_destroy(lst);\
	} while(0)
#define T(type, tname, val) do {\
		int i;\
		for(i=0; i<4; i++) {\
			int num = ((int[]){1, 10, 100, 1000, 10000})[i];\
			int rounds = COUNT/num;\
			clock_t t = clock();\
			RUN(ecbuf, type, val);\
			float et = ((float)(clock()-t))/CLOCKS_PER_SEC;\
			t = clock();\
			RUN(llbuf, type, val);\
			float lt = ((float)(clock()-t))/CLOCKS_PER_SEC;\
			printf("ecbuf: %.3fs, llbuf: %.3fs -- Push/pop of %d " tname " repeated %d times.\n", et, lt, num, rounds);\
		}\
	} while(0)

	struct { int64_t a,b; } s16 = {0,0};
	struct { int64_t a,b,c,d; } s32 = {0,0,0,0};
	struct { typeof(s32) a,b; } s64 = {s32,s32};

	T(char,        "chars", 1);
	T(int,         "ints", 1);
	T(int64_t,     "64-bit ints", 1);
	T(typeof(s16), "16-byte structs", s16);
	T(typeof(s32), "32-byte structs", s32);
	T(typeof(s64), "64-byte structs", s64);

#undef T

	return 0;
}

/* vim: set noet sw=4 ts=4: */
