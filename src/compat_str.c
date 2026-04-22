/*
 * compat_str.c — portable strlcpy() and strlcat() implementations
 *
 * Compiled on Linux only via $(COMPAT_SRC) in the Makefile.
 * Not compiled on OpenBSD — libc provides these.
 * See TECH_STACK.md §4.3.
 */

#include <stddef.h>
#include <string.h>

#include "compat_str.h"

/*
 * strlcpy — copy at most dstsize-1 bytes from src to dst, always
 * NUL-terminating dst. Returns strlen(src) (the full source length),
 * which allows the caller to detect truncation: truncation occurred if
 * the return value >= dstsize.
 *
 * Behaviour on edge cases:
 *   dstsize == 0 : no write, returns strlen(src)
 *   src == ""    : dst[0] = '\0', returns 0
 */
size_t
strlcpy(char *dst, const char *src, size_t dstsize)
{
	const char	*s = src;
	size_t		 n = dstsize;

	if (n != 0) {
		while (--n != 0) {
			if ((*dst++ = *s++) == '\0')
				break;
		}
	}

	if (n == 0) {
		if (dstsize != 0)
			*dst = '\0';
		while (*s++ != '\0')
			;
	}

	return (size_t)(s - src - 1);
}

/*
 * strlcat — append src to dst. Appends at most dstsize - strlen(dst) - 1
 * bytes, always NUL-terminating the result. Returns the total length
 * that would have been created (strlen(dst_initial) + strlen(src)),
 * which allows the caller to detect truncation.
 *
 * If strlen(dst) >= dstsize, the existing content is not modified;
 * returns dstsize + strlen(src).
 */
size_t
strlcat(char *dst, const char *src, size_t dstsize)
{
	char		*d    = dst;
	const char	*s    = src;
	size_t		 n    = dstsize;
	size_t		 dlen;

	/* Find the end of dst within the buffer boundary. */
	while (n-- != 0 && *d != '\0')
		d++;
	dlen = (size_t)(d - dst);
	n    = dstsize - dlen;

	if (n == 0)
		return dlen + strlen(s);

	while (*s != '\0') {
		if (n != 1) {
			*d++ = *s;
			n--;
		}
		s++;
	}
	*d = '\0';

	return dlen + (size_t)(s - src);
}

