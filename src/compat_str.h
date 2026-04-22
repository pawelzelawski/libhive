/*
 * compat_str.h — portable strlcpy and strlcat for Linux
 *
 * On OpenBSD: not compiled; libc provides these functions.
 * On Linux:   compiled in via $(COMPAT_SRC) in the Makefile.
 * See TECH_STACK.md §4.3.
 */

#ifndef COMPAT_STR_H
#define COMPAT_STR_H

#include <stddef.h>

/*
 * strlcpy — copy at most dstsize-1 bytes from src to dst, always
 * NUL-terminating. Returns strlen(src) (the full source length).
 */
size_t	strlcpy(char *dst, const char *src, size_t dstsize);

/*
 * strlcat — append src to dst up to dstsize-1 total bytes, always
 * NUL-terminating. Returns strlen(dst_initial) + strlen(src).
 */
size_t	strlcat(char *dst, const char *src, size_t dstsize);

#endif /* COMPAT_STR_H */

