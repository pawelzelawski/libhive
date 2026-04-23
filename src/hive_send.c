/*
 * hive_send.c — send queue helpers
 *
 * Implements frame_hdr_write(), a session-coupled thin wrapper around the
 * standalone frame_hdr_write_at() from hive_frame_bare.c.
 *
 * Requires the full hive_session_t definition (send_buf, send_buf_used).
 * Wired into the Makefile build in Task 2.5 once hive_session_t is defined
 * in src/hive_internal.h.
 *
 * See ARCHITECTURE.md §6.2 for the canonical pseudocode.
 * See ARCHITECTURE.md §6.1 for the send buffer layout.
 */

#include "hive_internal.h"
#include "hive_send.h"

/*
 * Write a 9-byte HTTP/2 frame header at send_buf + send_buf_used,
 * advance send_buf_used by 9, and return a pointer to the written header.
 *
 * The returned pointer allows callers to back-patch length or flags after
 * encoding the payload (used in the HEADERS split path and DATA path).
 * See ARCHITECTURE.md §6.2.
 */
uint8_t *
frame_hdr_write(hive_session_t *s, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id)
{
	uint8_t *p = s->send_buf + s->send_buf_used;
	frame_hdr_write_at(p, length, type, flags, stream_id);
	s->send_buf_used += 9;
	return p;
}

