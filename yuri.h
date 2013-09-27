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

/* This is a URI parser and validator that supports the following formats:
 * - <host>
 * - <host>:<port>
 * - <scheme>://<host>
 * - <scheme>://<host>:<port>
 * - <anything above></path><?query><#fragment>
 *
 * <scheme> must match /^[a-zA-Z][a-zA-Z0-9\.+-]{0,14}$/
 * <host> is either:
 *   - A full IPv4 address (1.2.3.4)
 *   - An IPv6 address within square brackets
 *   - A domain name. That is, something like /^([a-zA-Z0-9-]{1,63}\.)+$/, with
 *     a maximum length of 255 characters. Actual parser is a bit more strict
 *     than the above regex.
 * <port> must be a decimal number between 1 and 65535 (both inclusive)
 * </path> is an escaped string not containing '?' or '#'
 * <?query> is an escaped string not containing '#'
 * <#fragment> is an escaped string not containing '#'
 * Any </path>, <?query> and/or <#fragment> components may be absent.
 *
 * The format of the <?query> part is highly dependent on the application and
 * is therefore not automatically parsed. However, a simple parser is available
 * for the common key=param style.
 *
 * Not supported (yet):
 * - Username / password parts
 * - Symbolic port names
 * - Internationalized domain names. Parsing only succeeds when the address
 *   is in the ASCII form.
 * - Relative references (Protocol relative URLs), e.g. "//domain.com/"
 * - Percent encoding in <host> and <port> is not handled. Even though the
 *   RFC's seem to imply that this is allowed.
 *
 * URI unescaping is supported, but the %00 escape is explicitely NOT allowed
 * and will cause parsing to fail with a validation error.  This makes this
 * library unsuitable for schemes that use URI escaping to send binary data,
 * such as BitTorrent tracker announcements. Non-standard %uxxxx escapes are
 * not supported, either.
 *
 * RFC1738 and RFC3986 have been used as reference, but strict adherence to
 * those specifications isn't a direct goal. In particular, this parser allows
 * <scheme> to be absent and requires <host> to be present and limited to an
 * IPv4/IPv6/DNS address. This makes the parser suitable for schemes like
 * irc://, http://, ftp:// and adc://, but unsuitable for stuff like mailto:
 * and magnet:.
 */

#ifndef YURI_H
#define YURI_H

#include <stdint.h>
#include <stdlib.h>


typedef enum {
	YURI_IPV6,
	YURI_IPV4,
	YURI_DOMAIN
} yuri_hosttype_t;


/* See description above for the supported URI formats. */
typedef struct {
	/* Pointer to the start of the buffer. This is the buffer given to
	 * yuri_parse(), or a newly created buffer in the case of
	 * yuri_parse_copy(). */
	char *buf;
	/* All the pointers below point into the *buf memory area. */

	/* Empty string if there was no scheme in the URI. Uppercase characters
	 * (A-Z) are automatically converted to lowercase (a-z). */
	char *scheme;

	/* Hostname part of the URI. hosttype indicates what kind of hostname this
	 * is (IPv4, IPv6 or a domain name).
	 * No normalization or case modification on the host is performed. Any
	 * square brackets around the IPv6 address in the URI are not considered
	 * part of the hostname.  E.g. the URI "http://[::]/" has YURI_IPV6 and
	 * host = "::". */
	char *host;
	yuri_hosttype_t hosttype;

	/* 0 if no port was included in the URI. */
	uint16_t port;

	/* Unmodified path, query and fragment parts of the URI, not including the
	 * first '/', '?' or '#' character, respectively. If a part was missing
	 * from the URI, its value here is set to an empty string. Note that it is
	 * not possible to distinguish between a missing part, or a present but
	 * empty part. For example, both "http://blicky.net" and
	 * "http://blicky.net/?#" will have all the fields below set to an empty
	 * string.
	 *
	 * These parts are passed through in the same form as they are present in
	 * the URI. Unescaping is not automatically performed by yuri_parse()
	 * because these components may include schema-specific delimiters and
	 * encoding rules. If you just want their unescaped string representation,
	 * you can always use yuri_unescape() on these fields. If you know that the
	 * query string is in key=value format (most common), use the
	 * yuri_query_parse() function to parse it. */
	char *path;
	char *query;
	char *fragment;
} yuri_t;


/* Returns -1 if the URI isn't valid, 0 on success. The given string should be
 * zero-terminated and will be modified in-place.
 *
 * If the URI is invalid, both the `str' and `out' arguments may have been
 * partially written to and may therefore contain rubbish.
 *
 * This function attempts to do as much (sane) validation as possible. */
int yuri_parse(char *str, yuri_t *out);


/* Similar to yuri_parse(), but makes an internal copy of the string before
 * processing. Returns -2 on OOM.
 *
 * When this function returns 0, you must call free(out->buf) after you're done
 * with the parsed results. */
int yuri_parse_copy(const char *str, yuri_t *out);


/* Validates whether a string has been correctly escaped. This function should
 * be used before calling yuri_unescape() on a string obtained from an
 * untrusted source. Note that validation on the 'path', 'query' and 'fragment'
 * fields in the yuri_t struct is not necessary, as yuri_parse() will do this
 * already.
 * A string is considered valid if any % characters are followed by two hex
 * characters and there is no %00 escape. */
int yuri_validate_escape(const char *str);


/* Unescapes the given string in-place. That is, it converts %XX escapes into
 * their byte representation. Returns the string given as first argument, so
 * you can use it as yuri_unescape(strdup(str)) if you want to allocate a new
 * string. This function simply passes through NULL if str is NULL.
 *
 * IMPORTANT: This function does not perform any validation. Behaviour is
 * undefined when used on an invalid string. Use yuri_validate_escape() if you
 * do not know whether the string is valid or not.
 *
 * IMPORTANT#2: You should only call this function on the same string once. For
 * example, you can do a:
 *   char *unescaped_path = yuri_unscape(uri->path);
 * to get the path once. After that you can access the unescaped path directly
 * through uri->path. The original path is then not available anymore, and
 * calling yuri_unescape(uri->path) another time is an error.
 *
 * IMPORTANT#3: You can't expect the returned string to be valid UTF-8 or to
 * not contain any weird (e.g. control) characters. If you want to do any
 * further validation on the strings obtained from a URI, you must do so AFTER
 * calling this function. */
char *yuri_unescape(char *str);



/* Simple query string parser. Parses both "a=b&c=d", "a=b;c=d" and a mixture
 * of the two styles. This function is used as follows:
 *
 *   yuri_t uri;
 *   if(yuri_parse(str, &uri))
 *     // handle error
 *
 *   char *key, *value;
 *   while(yuri_query_parse(&uri.query, &key, &value)) {
 *     // Do something with key and value
 *   }
 *
 * This function takes a pointer to a query string buffer as argument, parses
 * one key/value pair, stores pointers into this buffer in *key and *value, and
 * advances the *str pointer to the next pair, or to the end of the string if
 * there is no next pair. Returns 1 if a key/value pair has been extracted, 0
 * if !*str || !**str.
 *
 * The given *str is modified in-place. If you wish to re-use the query string
 * later on or want to iterate multiple times over the same query string, you
 * need to make a copy of the string (e.g. with strdup()) and iterate over
 * that.
 *
 * The strings returned in *key and *value are unescaped, as in
 * yuri_unescape(). Additionally, the '+' character is converted into a space
 * as well. Both the key and value can be set to an empty string. This happens
 * for empty pairs ("&&" or "&=&"), empty keys ("=abc") or empty/absent values
 * ("abc" or "abc=").
 *
 * IMPORTANT: The given string is assumed to contain valid URI escapes, as in
 * yuri_validate_escape(), so run that function first if the string comes from
 * an untrusted source.
 *
 * The IMPORTANT#3 note of yuri_unescape() applies here, too.
 */
int yuri_query_parse(char **str, char **key, char **value);

#endif

/* vim: set noet sw=4 ts=4: */
