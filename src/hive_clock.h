#ifndef HIVE_CLOCK_H
#define HIVE_CLOCK_H

#include <stdint.h>

#if defined(HIVE_TEST_CLOCK) && HIVE_TEST_CLOCK == 1

extern uint64_t hive_test_clock_secs;

static inline uint64_t
hive_monotonic_secs(void)
{
	return hive_test_clock_secs;
}

#else

#include <time.h>

static inline uint64_t
hive_monotonic_secs(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0u;
	return (uint64_t)ts.tv_sec;
}

#endif

#endif /* HIVE_CLOCK_H */
