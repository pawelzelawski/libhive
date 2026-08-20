/* Verify that public stream-state declarations need no private header. */
#include "hive.h"

int
main(void)
{
	hive_stream_state_t state = HIVE_STREAM_IDLE;

	return state == HIVE_STREAM_IDLE ? 0 : 1;
}
