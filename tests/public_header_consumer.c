/* Verify that public stream-state declarations need no private header. */
#include "hive.h"

#if HIVE_VERSION_MAJOR != 1 || HIVE_VERSION_MINOR != 1 || \
    HIVE_VERSION_PATCH != 0
#error "unexpected libhive release version"
#endif

int
main(void)
{
	hive_stream_state_t state = HIVE_STREAM_IDLE;
	const char *version = HIVE_VERSION_STRING;

	return state == HIVE_STREAM_IDLE && version[0] == '1' ? 0 : 1;
}
