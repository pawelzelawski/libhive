/*
 * test_harness.h — minimal test assertion framework
 *
 * Test functions must return 1 on pass, 0 on fail.
 * Use the RUN() macro in a test binary's main() to register and run tests.
 * See TECH_STACK.md §6.1.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name)                                    \
	do {                                         \
		tests_run++;                         \
		if (test_##name()) {                 \
			tests_passed++;              \
			printf("PASS: " #name "\n"); \
		} else {                             \
			printf("FAIL: " #name "\n"); \
		}                                    \
	} while (0)

#define ASSERT(expr)                                          \
	do {                                                  \
		if (!(expr)) {                                \
			printf("  assertion failed: %s\n"     \
			       "  at %s:%d\n",                \
			       #expr, __FILE__, __LINE__);    \
			return 0;                             \
		}                                             \
	} while (0)

#endif /* TEST_HARNESS_H */

