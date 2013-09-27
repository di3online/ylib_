/* Copyright (c) 2012-2013 Yoran Heling

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

#include <ylog.c> /* Include ylog.c directly, we're testing static functions */
#include <assert.h>


int main(int argc, char **argv) {

	/* ylog_set_file_name() */
#define T(in, out) do {\
		ylog_file_t file;\
		ylog_set_file_name(&file, in);\
		assert(strcmp(file.name, out) == 0);\
		free((char *)file.name);\
	} while(0)
	T("", "");
	T("abc", "abc");
	T("abc.c", "abc");
	T("abc.cpp", "abc");
	T("x.cc", "x");
	T("/some/file.h", "/some/file");
	T("abc.pl", "abc.pl");
#undef T

	/* ylog_set_file_level() */
	ylog_default_level = -1;
#define T(fn, pat, lvl) do {\
		ylog_file_t file;\
		file.name = fn;\
		ylog_pattern = strdup(pat);\
		ylog_set_file_level(&file);\
		assert(file.level == lvl);\
		assert(strcmp(ylog_pattern, pat) == 0);\
		free(ylog_pattern);\
	} while(0)
	do { /* Test NULL handling */
		ylog_file_t file;
		file.name = NULL;
		ylog_pattern = NULL;
		ylog_set_file_level(&file);
		assert(file.level == -1);
	} while(0);
	T("", "5", 5);
	T("", "*:5", 5);
	T("", "*:0", 0);
	T("", "*:9999", 9999); /* YLOG_MAX */
	T("", "*:10000", -1);
	T("", "*:-5", -1);
	T("", "*:+5", -1);
	T("", "*:009", 9); /* 0-prefixing can be allowed, I suppose */
	T("some/file", "*:4", 4);
	T("some/file", "some:4", -1);
	T("some/file", "some/fil:4", -1);
	T("some/file", "some/file/:4", -1);
	T("some/file", "some/file:4", 4);
	T("some/file", "file:4", 4);
	T("some/file", "fil*:4", 4);
	T("some/file", "f*:4", 4);
	T("some/file", "some/*:4", 4);
	T("some/file", "so*/file:4", 4);
	T("some/file", "*/file:4", 4);
	T("some/file", "abc:3,file:4", 4);
	T("some/file", "abc:3,*:4", 4);
	T("some/file", "abc:3,some/file:4", 4);
	T("some/file", "abc:3,some/*:4", 4);
	T("somex/file", "abc:3,some/*:4", -1);
	T("somex/file", "abc:3,some/*:4,*:1", 1);
	T("some/file", "abc:3,some/*:4,*:1", 4);
	T("somex/file", "abc:3,some/*:4,1", 1);
	T("some/file", "abc:3,some/*:4,1", 4);
	T("some/file", "*:3,some/file:4", 3);
	T("some/file", "3,some/file:4", 3);
	T("some/file", "abc:1,*:3,some/file:4", 3);
	T("some/file", "abc:1,3,some/file:4", 3);
#undef T

	return 0;
}

/* vim: set noet sw=4 ts=4: */
