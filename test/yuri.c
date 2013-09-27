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

#include <yuri.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>


#define F(s) do {\
		yuri_t uri;\
		assert(yuri_parse_copy(s, &uri) == -1);\
	} while(0)

#define inbound(s, n) assert(uri.n >= uri.buf && uri.n <= uri.buf + sizeof s)

#define T(s, vscheme, vhost, vhosttype, vport, vpath, vquery, vfragment) do {\
		yuri_t uri;\
		assert(yuri_parse_copy(s, &uri) == 0);\
		inbound(s, scheme);\
		inbound(s, host);\
		inbound(s, path);\
		inbound(s, query);\
		inbound(s, fragment);\
		assert(strcmp(uri.scheme, vscheme) == 0);\
		assert(strcmp(uri.host, vhost) == 0);\
		assert(uri.hosttype == vhosttype);\
		assert(uri.port == vport);\
		assert(strcmp(uri.path, vpath) == 0);\
		assert(strcmp(uri.query, vquery) == 0);\
		assert(strcmp(uri.fragment, vfragment) == 0);\
		free(uri.buf);\
	} while(0)


static void t_parse() {
	F("");

	/* Scheme */
#define FS(s) F(s"host")
#define TS(s, a) T(s"host", a, "host", YURI_DOMAIN, 0, "", "", "")
	FS(":");
	FS("://");
	FS("//");
	FS(":/");
	FS("a:");
	FS("a:/");
	FS(".://");
	FS("abcdefghijklmnop://");
	FS("9abc://");
	FS("abc_d://");
	TS("http://", "http");
	TS("hTtp://", "http");
	TS("abcdefghijklmno://", "abcdefghijklmno");
	TS("ADC+adCs://", "adc+adcs");
	TS("x://", "x");
	TS("x.://", "x.");
	TS("a.b+C://", "a.b+c");
#undef TS
#undef FS

	/* Port */
#define FP(s) F("host:"s)
#define TP(s, v) T("host:"s, "", "host", YURI_DOMAIN, v, "", "", "")
	FP("");
	FP(":");
	FP("0");
	FP("012");
	FP("65536");
	FP("111111");
	FP("-1");
	FP("+1");
	FP("9a7");
	TP("1", 1);
	TP("15", 15);
	TP("65535", 65535);
#undef FP
#undef TP

	/* IPv4 */
#define F4(s) F("abc://"s"/")
#define T4(s) T("abc://"s"/", "abc", s, YURI_IPV4, 0, "", "", "")
	F4("");
	F4("0");
	F4("0.0.0.0.");
	F4(".0.0.0");
	F4(".0.0.0.0");
	F4("0.0..0.0");
	F4("256.255.255.255");
	F4("0.310.0.3");
	F4("-1.0.0.1");
	F4("10.0.a0.0");
	T4("0.0.0.0");
	T4("1.2.3.4");
	T4("0.9.10.50");
	T4("127.0.0.1");
	T4("255.255.255.255");
	T4("249.200.199.253");
#undef T4
#undef F4

	/* IPv6 */
	F("::");
	F("::1");
	F("::0.0.0.0");
	F("0:0:0:0:0:0:0:0");
#define F6(s) F("abc://["s"]/")
#define T6(s) T("abc://["s"]/", "abc", s, YURI_IPV6, 0, "", "", "")
	F6("0");
	F6("0:0:0:0:0:0:0");
	F6("0:0:0:0:0:0:0:");
	F6(":0:0:0:0:0:0:0");
	F6("0:0:0:0:0:0:0:0:0");
	F6("0:0:0:0:0:0:0:0::");
	F6("::0:0:0:0:0:0:0:0");
	F6("0:0:0:0::0:0:0:0");
	F6("::0:0:0:0:0:0:0:0:0");
	F6("0:0:0:0:0:0:0::0");
	F6("::0::");
	F6("0::0::0");
	F6("::12345");
	F6("::FFFG");
	F6("[::]");
	F6("-::");
	F6("::-");
	F6("::0.0.0");
	F6("0:0:0:0:0:0.0.0.0");
	F6("0:0:0:0:0:0:0:0.0.0.0");
	F6("0:0:0:0:0:0.0.0.0:0");
	T6("::");
	T6("::0");
	T6("0::");
	T6("0::0");
	T6("::FFFF:1:12:123");
	T6("0:0:0::0:0:0:0");
	T6("0::0:0:0:0:0:0");
	T6("::0:0:0:0:0:0:0");
	T6("0:0:0:0:0:0:0::");
	T6("0:0:0:0:0:0::0");
	T6("0:0:0:0:0:0:0:0");
	T6("0000:0000:0000:0000:0000:0000:0000:0000");
	T6("000:000:000:000:000:000:000:000");
	T6("00:00:00:00:00:00:00:00");
	T6("::0.0.0.0");
	T6("0:0:0:0:0:0:0.0.0.0");
	T6("::0:0:0:0:0:0.0.0.0");
	/* Some examples from RFC3513 */
	T6("FEDC:BA98:7654:3210:FEDC:BA98:7654:3210");
	T6("1080:0:0:0:8:800:200C:417A");
	T6("FF01:0:0:0:0:0:0:101");
	T6("0:0:0:0:0:0:0:1");
	T6("1080::8:800:200C:417A");
	T6("FF01::101");
	T6("::1");
	T6("::13.1.68.3");
	T6("::FFFF:129.144.52.38");
#undef T6
#undef F6

	/* Domain */
#define FD(s) F("abc://"s"/")
#define TD(s) T("abc://"s"/", "abc", s, YURI_DOMAIN, 0, "", "", "")
	FD(".");
	FD(".com.");
	FD("a_c.com");
	FD("-ac.com");
	FD("ac-.com");
	FD("com.123");
	FD("com.1-2.3.");
	FD("abc@com");
	FD("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123456789012.com");
	FD("abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklm");
	TD("com");
	TD("com.");
	TD("ac.com");
	TD("a-c.com");
	TD("a--c.com");
	TD("123.com");
	TD("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ12345678901.com");
	TD("abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijklmnopqrstuvwxyz.abcdefghijkl");
#undef TD
#undef FD

	/* path, query, fragment */
#define FC(s) F("abc://domain"s)
#define TC(s, vp, vq, vf) T("abc://domain"s, "abc", "domain", YURI_DOMAIN, 0, vp, vq, vf)
	FC("/%0g");
	FC("?%0g");
	FC("#%0g");
	FC("##");
	TC("", "", "", "");
	TC("/?#", "", "", "");
	TC("/abc", "abc", "", "");
	TC("?abc", "", "abc", "");
	TC("#abc", "", "", "abc");
	TC("/%01?%02#%03", "%01", "%02", "%03");
	TC("/abc/?abc/?#abc/?", "abc/", "abc/?", "abc/?");
#undef TC
#undef FC

	/* Misc */
	F("/");
	F("blicky.net ");
	F(" blicky.net");
	F("//blicky.net");
	F("abcdefghijklmnop://blicky.net/");
}

#undef F
#undef T
#undef V



#define T(s, ...) do {\
		char *buf = strdup(s);\
		char *args[] = {__VA_ARGS__};\
		char *key, *value, *str = buf;\
		size_t i;\
		for(i=0; i<sizeof(args)/sizeof(*args); i+=2) {\
			assert(yuri_query_parse(&str, &key, &value) == 1);\
			assert(strcmp(key, args[i]) == 0);\
			assert(strcmp(value, args[i+1]) == 0);\
		}\
		assert(yuri_query_parse(&str, &key, &value) == 0);\
		free(buf);\
	} while(0)

static void t_query() {
	{ /* Should handle NULL */
		char *buf = NULL, *key, *value;
		assert(yuri_query_parse(&buf, &key, &value) == 0);
	}

	T("",);
	T("a", "a", "");
	T("k=v", "k", "v");
	T("key=value", "key", "value");
	T("%20=%6a", "\x20", "\x6a");
	T("k=v;k=v&k=v", "k", "v", "k", "v", "k", "v");
	T("a+b=b+a", "a b", "b a");
	T("key=value1=value2", "key", "value1=value2");
	T("====", "", "==="); /* Query strings can be odd... */
	T("abc=", "abc", "");
	T("=abc", "", "abc");
	T("a=b;a",  "a", "b", "a", "");
	T("a=b;a=", "a", "b", "a", "");
	T("a=b;=a", "a", "b", "", "a");
	T("&", "", "");
	T(";", "", "");
	T("&abc=val", "", "", "abc", "val");
	T("abc&k=v", "abc", "", "k", "v");
	T("ab=&k=v", "ab", "", "k", "v");
	T("a=b&&k=v", "a", "b", "", "", "k", "v");
	T("a=b;;k=v", "a", "b", "", "", "k", "v");
}

#undef T



int main(int argc, char **argv) {
	/* yuri_validate_escape() */
#define T(s) assert(yuri_validate_escape(s) == 0)
#define F(s) assert(yuri_validate_escape(s) == -1)
	T("");
	T("!@#$^&*()[]{}\\|=+-_,<>./?\"';:`~ \t\n");
	T("%01%02%03  %abx%ABy%aBz%Ab %9f %f9 %9F %F9 ");
	F("%00");
	F("%");
	F("%e");
	F("%gg");
	F("%1G");
	F("%G1");
	F("abc%f");
	F("%fgabc");
#undef T
#undef F

	/* yuri_unescape() */
	assert(yuri_unescape(NULL) == NULL);
#define T(s, a) do {\
		char *buf = strdup(s);\
		assert(yuri_unescape(buf) == buf);\
		assert(strcmp(buf, a) == 0);\
		free(buf);\
	} while(0)
	T("", "");
	T("abc", "abc");
	T("%20", "\x20");
	T("abc%A1%ab%ff%01", "abc\xa1\xab\xff\x01");
#undef T

	t_query();
	t_parse();
	return 0;
}

/* vim: set noet sw=4 ts=4: */
