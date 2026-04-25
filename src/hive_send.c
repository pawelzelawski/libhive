/*
 * hive_send.c — send queue helpers
 *
 * Implements frame_hdr_write(), send_queue_append_ctrl(), and the
 * Phase 5 flow-control-gated send_queue_flush_data().
 *
 * frame_hdr_write() is a session-coupled thin wrapper around the
 * standalone frame_hdr_write_at() from hive_frame_bare.c.
 *
 * send_queue_append_ctrl() serialises one control frame (header + payload)
 * into send_buf and records one iovec entry.  See ARCHITECTURE.md §6.3.
 *
 * send_queue_flush_data() in Phase 5 enforces send-window gating and
 * outbound max frame-size capping before calling per-stream read callbacks.
 * Full DATA frame queuing lands in Phase 6.  See ARCHITECTURE.md §6.5.
 *
 * Requires the full hive_session_t definition (send_buf, send_buf_used,
 * send_iov, send_iov_count).
 *
 * See ARCHITECTURE.md §6.1 for the send buffer layout.
 * See ARCHITECTURE.md §6.2 for the canonical frame_hdr_write() pseudocode.
 */

#include <string.h>

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
frame_hdr_write(hive_session_t *s,
                uint32_t length,
                uint8_t type,
                uint8_t flags,
                uint32_t stream_id)
{
	uint8_t *p = s->send_buf + s->send_buf_used;
	frame_hdr_write_at(p, length, type, flags, stream_id);
	s->send_buf_used += 9;
	return p;
}

/*
 * Serialise one control frame into send_buf and record one iovec entry.
 *
 * Layout (ARCHITECTURE.md §6.3):
 *   frame_start        = send_buf_used          (before header write)
 *   frame_hdr_write()  → advances send_buf_used by 9
 *   payload bytes      → copied at send_buf + send_buf_used
 *   iov entry          → {send_buf + frame_start, 9 + payload_len}
 *   send_buf_used      += payload_len
 *
 * One iovec entry per control frame.  Header and payload are contiguous.
 */
void
send_queue_append_ctrl(hive_session_t *s,
                       uint8_t type,
                       uint8_t flags,
                       uint32_t stream_id,
                       const uint8_t *payload,
                       uint32_t payload_len)
{
	size_t frame_start = s->send_buf_used;

	frame_hdr_write(s, payload_len, type, flags, stream_id);

	if (payload_len > 0 && payload != NULL)
		memcpy(s->send_buf + s->send_buf_used, payload, payload_len);

	s->send_iov[s->send_iov_count].iov_base = s->send_buf + frame_start;
	s->send_iov[s->send_iov_count].iov_len =
	    (size_t)9u + (size_t)payload_len;
	s->send_iov_count++;

	s->send_buf_used += (size_t)payload_len;
}

/*
 * Phase 5.3 — flow-control-gated DATA source scan.
 *
 * For each open stream with a pending data_source callback:
 * - require both connection and stream send windows to be positive,
 * - cap callback request length to min(remote max frame size,
 *   connection send window, stream send window),
 * - if callback returns 0 bytes, skip to the next stream.
 *
 * Phase 6 adds actual DATA frame queueing and window decrement on emitted
 * frames; this Phase 5 step is gating-only enforcement.
 */
void
send_queue_flush_data(hive_session_t *s)
{
	uint32_t i;

	if (s == NULL)
		return;

	for (i = 0; i < s->opt_max_concurrent_streams; i++) {
		hive_stream_t *st;
		hive_read_callback_t cb;
		uint32_t max_len;
		uint8_t *body_ptr;
		ssize_t nread;

		st = &s->stream_slots[i];
		if (st->stream_id == 0)
			continue;

		cb = st->data_source.read_callback;
		if (cb == NULL)
			continue;

		if (s->send_window <= 0 || st->send_window <= 0)
			continue;

		max_len = s->remote_settings.max_frame_size;
		if ((uint32_t)s->send_window < max_len)
			max_len = (uint32_t)s->send_window;
		if ((uint32_t)st->send_window < max_len)
			max_len = (uint32_t)st->send_window;
		if (max_len == 0)
			continue;

		body_ptr = s->send_buf + s->send_buf_used;
		nread = cb(s,
		           st->stream_id,
		           &body_ptr,
		           max_len,
		           st->data_source.user_data);
		if (nread <= 0)
			continue;
	}
}
