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

#include "yuri.h"
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>


/* The ctype.h functions are locale-dependent. We don't want that. */
#define y_isalpha(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z'))
#define y_isnum(x)   ((x) >= '0' && (x) <= '9')
#define y_isalnum(x) (y_isalpha(x) || y_isnum(x))
#define y_tolower(x) ((x) < 'A' || (x) > 'Z' ? (x) : (x)+0x20)

#define y_ishex(x)    (((x) >= 'a' && (x) <= 'f') || ((x) >= 'A' && (x) <= 'F') || y_isnum(x))
#define y_isscheme(x) ((x) == '+' || (x) == '-' || (x) == '.' || y_isalnum(x))
#define y_isdomain(x) ((x) == '-' || y_isalnum(x))
#define y_hexval(x)   ((x) >= '0' && (x) <= '9' ? (x)-'0' : (x) >= 'A' && (x) <= 'F' ? (x)-'A'+10 : (x)-'a'+10)


/* Parses the "<scheme>://" part, if it exists, and advances the buf pointer.
 */
static void yuri__scheme(char **buf, yuri_t *out) {
	const char *end = *buf;
	if(!y_isalpha(**buf)) {
		out->scheme = *buf + strlen(*buf);
		return;
	}
	do
		++end;
	while(end <= *buf+15 && y_isscheme(*end));
	if(end > *buf+15 || *end != ':' || end[1] != '/' || end[2] != '/') {
		out->scheme = *buf + strlen(*buf);
		return;
	}
	/* Valid scheme, lowercase it and advance *buf. */
	out->scheme = *buf;
	while(*buf != end) {
		**buf = y_tolower(**buf);
		(*buf)++;
	}
	**buf = 0;
	*buf += 3;
}


/* Parses the ":<port>" part in buf and, if it exists, sets the ':' to zero to
 * ensure that buf is a complete host string. */
static void yuri__port(char *buf, size_t len, yuri_t *out) {
	uint32_t res = 0, mul = 1;
	out->port = 0;
	if(!len)
		return;
	/* Read backwards */
	while(--len > 0 && y_isnum(buf[len])) {
		if(mul >= 100000)
			return;
		res += mul * (buf[len]-'0');
		if(res > 65535)
			return;
		mul *= 10;
	}
	if(!res || !len || buf[len] != ':' || buf[len+1] == '0')
		return;
	out->port = res;
	buf[len] = 0;
}


/* RFC1034 section 3.5 has an explanation of a (commonly used) domain syntax,
 * but I suspect it may be overly strict. This implementation will suffice, I
 * suppose. */
static int yuri__validate_domain(const char *str, int len) {
	int haslabel = 0, /* whether we've seen a label */
		lastishyp = 0, /* whether the last seen character in the label is a hyphen */
		startdig = 0, /* whether the last seen label starts with a digit (Not allowed per RFC1738, a sensible restriction IMO) */
		llen = 0; /* length of the current label */

	/* In the case of percent encoding, the length of the domain may be much
	 * larger. But this implementation (currently) does not accept percent
	 * encoding in the domain name. Similarly, the character limit applies to
	 * the ASCII form of the domain, in the case of an IDN, this check doesn't
	 * really work either. This implementation (currently) does not support
	 * IDN. In fact, this function should be "validate-and-normalize" instead
	 * of just validate in such a case. */
	if(len > 255)
		return -1;

	for(; len > 0; str++, len--) {
		if(*str == '.') {
			if(!llen || lastishyp)
				return -1;
			llen = 0;
			continue;
		} else if(llen >= 63)
			return -1;
		if(!y_isdomain(*str))
			return -1;
		lastishyp = *str == '-';
		if(llen == 0) {
			if(lastishyp) /* That is, don't start with a hyphen */
				return -1;
			startdig = y_isnum(*str);
		}
		haslabel = 1;
		llen++;
	}
	return haslabel && !startdig ? 0 : -1;
}


int yuri__host(char *buf, yuri_t *out) {
	char addrbuf[16];

	/* IPv6 */
	if(*buf == '[') {
		if(buf[strlen(buf)-1] != ']')
			return -1;
		buf++;
		buf[strlen(buf)-1] = 0;
		if(inet_pton(AF_INET6, buf, addrbuf) != 1)
			return -1;
		out->hosttype = YURI_IPV6;
		out->host = buf;
		return 0;
	}

	/* IPv4 */
	if(inet_pton(AF_INET, buf, addrbuf) == 1) {
		out->hosttype = YURI_IPV4;
		out->host = buf;
		return 0;
	}

	/* Domain */
	out->hosttype = YURI_DOMAIN;
	out->host = buf;
	return yuri__validate_domain(buf, strlen(buf));
}


int yuri_parse(char *buf, yuri_t *out) {
	char *end, endc;

	out->buf = buf;
	yuri__scheme(&buf, out);

	/* Find the end of the authority component (RFC3986, section 3.2) */
	end = buf;
	while(*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	endc = *end;
	*end = 0;

	yuri__port(buf, end-buf, out);
	if(yuri__host(buf, out))
		return -1;

	/* path */
	if(endc == '/') {
		out->path = ++end;
		while(*end && *end != '?' && *end != '#')
			end++;
		endc = *end;
		*end = 0;
		if(yuri_validate_escape(out->path))
			return -1;
	} else
		out->path = end;

	/* query */
	if(endc == '?') {
		out->query = ++end;
		while(*end && *end != '#')
			end++;
		endc = *end;
		*end = 0;
		if(yuri_validate_escape(out->query))
			return -1;
	} else
		out->query = end;

	/* fragment */
	if(endc == '#') {
		out->fragment = ++end;
		while(*end)
			if(*(end++) == '#')
				return -1;
		if(yuri_validate_escape(out->fragment))
			return -1;
	} else
		out->fragment = end;

	return 0;
}


int yuri_parse_copy(const char *str, yuri_t *out) {
	char *buf = strdup(str);
	if(!buf)
		return -2;
	if(yuri_parse(buf, out)) {
		free(buf);
		return -1;
	}
	return 0;
}


int yuri_validate_escape(const char *str) {
	while(*str) {
		if(*str != '%') {
			str++;
			continue;
		}
		if(!y_ishex(str[1]) || !y_ishex(str[2]) || (str[1] == '0' && str[2] == '0'))
			return -1;
		str += 3;
	}
	return 0;
}


char *yuri_unescape(char *str) {
	unsigned char *src = (unsigned char *)str, *dest = (unsigned char *)str;
	if(!str)
		return NULL;
	while(*src) {
		if(*src != '%') {
			*(dest++) = *(src++);
			continue;
		}
		*(dest++) = (y_hexval(src[1])<<4) | y_hexval(src[2]);
		src += 3;
	}
	*dest = 0;
	return str;
}


/* Special unescape function for the query string. Differs from yuri_unescape()
 * in that it also converts '+' to a space. */
static char *yuri__query_unescape(char *str) {
	unsigned char *src = (unsigned char *)str, *dest = (unsigned char *)str;
	while(*src) {
		if(*src == '+') {
			*(dest++) = ' ';
			src++;
			continue;
		}
		if(*src != '%') {
			*(dest++) = *(src++);
			continue;
		}
		*(dest++) = (y_hexval(src[1])<<4) | y_hexval(src[2]);
		src += 3;
	}
	*dest = 0;
	return str;
}


int yuri_query_parse(char **str, char **key, char **value) {
	if(!str || !*str || !**str)
		return 0;

	/* Key */
	char *sep = *str;
	while(*sep && *sep != '=' && *sep != ';' && *sep != '&')
		sep++;
	if(!*sep || *sep == ';' || *sep == '&') { /* No value */
		*key = *str;
		*value = sep;
		*str = *sep ? sep+1 : sep;
		*sep = 0;
		yuri__query_unescape(*key);
		return 1;
	}
	*(sep++) = 0;
	*key = *str;
	yuri__query_unescape(*key);

	/* Value */
	*value = sep;
	while(*sep && *sep != ';' && *sep != '&')
		sep++;
	*str = *sep ? sep+1 : sep;
	*sep = 0;
	yuri__query_unescape(*value);
	return 1;
}

/* vim: set noet sw=4 ts=4: */
