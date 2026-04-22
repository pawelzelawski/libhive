/*
 * run_tests.c — test binary entry point
 *
 * Calls RUN() for every test case across all test files.
 * Returns 0 if all pass, 1 if any fail.
 * See TECH_STACK.md §6.1.
 */

#include <stdio.h>

#include "test_harness.h"

/* --- test_compat.c -------------------------------------------------------- */
int test_strlcpy_basic(void);
int test_strlcpy_truncation(void);
int test_strlcpy_empty_src(void);
int test_strlcat_basic(void);
int test_strlcat_full_dst(void);

int
main(void)
{
	/* Phase 1.3 — platform compat layer */
	RUN(strlcpy_basic);
	RUN(strlcpy_truncation);
	RUN(strlcpy_empty_src);
	RUN(strlcat_basic);
	RUN(strlcat_full_dst);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
