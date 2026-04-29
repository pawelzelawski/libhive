/*
 * test_compat.c - platform compat layer tests (strlcpy, strlcat)
 *
 * On Linux: tests the compat_str.c implementations linked from libhive.a.
 * On OpenBSD: tests the libc implementations (same behaviour expected).
 */

#include <stddef.h>
#include <string.h>

#ifdef LINUX
/*
 * On Linux: use our compat_str.h which declares the implementations
 * linked in from libhive.a via src/compat_str.c.
 */
#include "../src/compat_str.h"
#else
/*
 * On OpenBSD: strlcpy/strlcat are in libc but _XOPEN_SOURCE=700 hides
 * them from <string.h> (they are BSD extensions, not POSIX). Declare
 * them explicitly so the test file compiles without warnings under
 * strict feature-test-macro mode.
 */
size_t	strlcpy(char *dst, const char *src, size_t dstsize);
size_t	strlcat(char *dst, const char *src, size_t dstsize);
#endif

#include "test_harness.h"

/* Forward declarations */
int	test_strlcpy_basic(void);
int	test_strlcpy_truncation(void);
int	test_strlcpy_empty_src(void);
int	test_strlcat_basic(void);
int	test_strlcat_full_dst(void);

/*
 * test_strlcpy_basic - copies up to dstsize-1 bytes, always NUL-terminates.
 */
int
test_strlcpy_basic(void)
{
	char	dst[8];
	size_t	ret;

	memset(dst, 0xFF, sizeof(dst));
	ret = strlcpy(dst, "hello", sizeof(dst));

	ASSERT(ret == 5);              /* returns strlen(src) */
	ASSERT(dst[5] == '\0');        /* NUL-terminated */
	ASSERT(memcmp(dst, "hello", 5) == 0);
	return 1;
}

/*
 * test_strlcpy_truncation - source longer than dst → truncated,
 * NUL-terminated, returns full source length.
 */
int
test_strlcpy_truncation(void)
{
	char	dst[4];
	size_t	ret;

	memset(dst, 0xFF, sizeof(dst));
	ret = strlcpy(dst, "hello", sizeof(dst)); /* "hello" is 5 bytes */

	ASSERT(ret == 5);              /* full source length returned */
	ASSERT(dst[3] == '\0');        /* NUL at last position */
	ASSERT(memcmp(dst, "hel", 3) == 0); /* truncated to dstsize-1 */
	return 1;
}

/*
 * test_strlcpy_empty_src - empty source → dst = "", returns 0.
 */
int
test_strlcpy_empty_src(void)
{
	char	dst[8];
	size_t	ret;

	memset(dst, 0xFF, sizeof(dst));
	ret = strlcpy(dst, "", sizeof(dst));

	ASSERT(ret == 0);
	ASSERT(dst[0] == '\0');
	return 1;
}

/*
 * test_strlcat_basic - appends src to dst up to remaining space.
 */
int
test_strlcat_basic(void)
{
	char	dst[16];
	size_t	ret;

	strlcpy(dst, "hello", sizeof(dst));
	ret = strlcat(dst, " world", sizeof(dst));

	ASSERT(ret == 11);             /* strlen("hello") + strlen(" world") */
	ASSERT(memcmp(dst, "hello world", 12) == 0); /* includes NUL */
	return 1;
}

/*
 * test_strlcat_full_dst - dst already full → no write,
 * returns combined length (strlen(dst) + strlen(src)).
 */
int
test_strlcat_full_dst(void)
{
	char	dst[4];
	size_t	ret;

	strlcpy(dst, "abc", sizeof(dst)); /* fills dst exactly: "abc\0" */
	ret = strlcat(dst, "xyz", sizeof(dst));

	/* dst already full: no characters appended */
	ASSERT(ret == 6);              /* strlen("abc") + strlen("xyz") */
	ASSERT(memcmp(dst, "abc", 4) == 0); /* dst unchanged, NUL still there */
	return 1;
}

