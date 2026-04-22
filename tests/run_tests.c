/*
 * run_tests.c — test binary entry point
 *
 * Calls RUN() for every test case across all test files.
 * Returns 0 if all pass, 1 if any fail.
 * See TECH_STACK.md §6.1.
 */

#include <stdio.h>

#include "test_harness.h"

int
main(void)
{
	/* Phase 1 tests are registered as they are added in tasks 1.3 and 1.4. */

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}

