/*
 * hive_send.c - send queue helpers
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

static int
hpack_table_clone(const hpack_table_t *src, const hive_mem_t *mem,
                  hpack_table_t *dst)
{
	uint32_t i;
	int ret;

	ret = hpack_table_init(dst, mem, src->max_size);
	if (ret != HIVE_OK)
		return ret;
	for (i = src->count; i > 0u; i--) {
		const hpack_entry_t *entry;

		entry = hpack_table_get(src, i - 1u);
		ret = hpack_table_insert(dst,
		                         mem,
		                         HPACK_ENTRY_NAME(entry),
		                         entry->name_len,
		                         HPACK_ENTRY_VALUE(entry),
		                         entry->value_len);
		if (ret != HIVE_OK) {
			hpack_table_free(dst, mem);
			return ret;
		}
	}
	dst->pending_max = src->pending_max;
	dst->pending_min = src->pending_min;
	dst->has_pending = src->has_pending;
	return HIVE_OK;
}

static void
hpack_table_commit(hpack_table_t *live, hpack_table_t *trial,
                   const hive_mem_t *mem)
{
	hpack_table_t old;

	old = *live;
	*live = *trial;
	memset(trial, 0, sizeof(*trial));
	hpack_table_free(&old, mem);
}

static int
send_queue_reserve_iov(hive_session_t *s, int needed)
{
	if (needed <= 0)
		return HIVE_ERR_INVALID_ARG;
	if ((uint32_t)needed > s->opt_max_send_iov)
		return HIVE_ERR_NOMEM;
	if (s->send_iov_count + needed <= (int)s->opt_max_send_iov)
		return HIVE_OK;

	/*
	 * Queue pressure is transport backpressure, not an invitation to perform
	 * I/O here.  In particular, this helper is used while parsing received
	 * frames; calling hive_session_send() from that path made recv() invoke
	 * the transport callback (and potentially a DATA source callback).
	 */
	return HIVE_ERR_WOULDBLOCK;
}

void
u32_write_be(uint8_t out[4], uint32_t v)
{
	out[0] = (uint8_t)((v >> 24) & 0xffu);
	out[1] = (uint8_t)((v >> 16) & 0xffu);
	out[2] = (uint8_t)((v >> 8) & 0xffu);
	out[3] = (uint8_t)(v & 0xffu);
}

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
int
send_queue_append_ctrl(hive_session_t *s,
                       uint8_t type,
                       uint8_t flags,
                       uint32_t stream_id,
                       const uint8_t *payload,
                       uint32_t payload_len)
{
	size_t needed;
	size_t frame_start;
	int ret;

	if (s == NULL || s->send_buf == NULL || s->send_iov == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (payload_len > 0 && payload == NULL)
		return HIVE_ERR_INVALID_ARG;

	ret = send_queue_reserve_iov(s, 1);
	if (ret != HIVE_OK)
		return ret;
	frame_start = s->send_buf_used;

	needed = 9u + (size_t)payload_len;
	if (frame_start + needed > s->send_buf_cap)
		return HIVE_ERR_NOMEM;

	frame_hdr_write(s, payload_len, type, flags, stream_id);

	if (payload_len > 0 && payload != NULL)
		memcpy(s->send_buf + s->send_buf_used, payload, payload_len);

	s->send_iov[s->send_iov_count].iov_base = s->send_buf + frame_start;
	s->send_iov[s->send_iov_count].iov_len =
	    (size_t)9u + (size_t)payload_len;
	s->send_iov_count++;

	s->send_buf_used += (size_t)payload_len;
	return HIVE_OK;
}

/*
 * Phase 6.1 - HEADERS queueing with CONTINUATION splitting.
 *
 * Encodes the header block contiguously, then emits either:
 *   - single iov (HEADERS header + full payload), or
 *   - alternating iovs (frame header, payload chunk, ...)
 *     for HEADERS + CONTINUATION sequence.
 *
 * See ARCHITECTURE.md §6.4.
 */
int
send_queue_append_headers(hive_session_t *s,
                          uint32_t stream_id,
                          const hive_nv_t *nva,
                          size_t nvlen,
                          uint8_t end_stream)
{
	uint32_t max_frame;
	size_t first_hdr_offset;
	size_t encode_start;
	size_t out_cap;
	size_t encoded_len;
	hpack_table_t trial;
	int rc;
	int ret;

	if (s == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (nvlen > 0 && nva == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (s->send_buf == NULL || s->send_iov == NULL)
		return HIVE_ERR_INVALID_ARG;

	max_frame = s->remote_settings.max_frame_size;
	if (max_frame == 0)
		return HIVE_ERR_INVALID_ARG;

	/* Ensure at least one free iov slot before encoding into send_buf. */
	rc = send_queue_reserve_iov(s, 1);
	if (rc != HIVE_OK)
		return rc;

	first_hdr_offset = s->send_buf_used;
	if (first_hdr_offset + 9u > s->send_buf_cap)
		return HIVE_ERR_NOMEM;

	encode_start = first_hdr_offset + 9u;
	out_cap = s->send_buf_cap - encode_start;
	if (out_cap > (size_t)s->opt_max_continuation_size)
		out_cap = (size_t)s->opt_max_continuation_size;

	memset(&trial, 0, sizeof(trial));
	ret = hpack_table_clone(&s->enc_table, &s->mem, &trial);
	if (ret != HIVE_OK)
		return ret;
	ret = hpack_encode_block(&trial,
	                         &s->mem,
	                         nva,
	                         nvlen,
	                         s->send_buf + encode_start,
	                         out_cap,
	                         &encoded_len);
	if (ret != HIVE_OK) {
		hpack_table_free(&trial, &s->mem);
		return ret;
	}

	if (encoded_len <= (size_t)max_frame) {
		uint8_t flags;

		flags = HIVE_FLAG_END_HEADERS;
		if (end_stream)
			flags |= HIVE_FLAG_END_STREAM;

		frame_hdr_write_at(s->send_buf + first_hdr_offset,
		                   (uint32_t)encoded_len,
		                   HIVE_FRAME_HEADERS,
		                   flags,
		                   stream_id);
		s->send_iov[s->send_iov_count].iov_base =
		    s->send_buf + first_hdr_offset;
		s->send_iov[s->send_iov_count].iov_len = 9u + encoded_len;
		s->send_iov_count++;
		s->send_buf_used = encode_start + encoded_len;
		hpack_table_commit(&s->enc_table, &trial, &s->mem);
		return HIVE_OK;
	} else {
		size_t n_frames;
		size_t cont_hdr_area;
		size_t cont_hdr_bytes;
		size_t needed_iov;
		size_t pos;
		size_t frame_idx;

		n_frames =
		    (encoded_len + (size_t)max_frame - 1u) / (size_t)max_frame;
		cont_hdr_area = encode_start + encoded_len;
		cont_hdr_bytes = (n_frames - 1u) * 9u;
		if (cont_hdr_area + cont_hdr_bytes > s->send_buf_cap) {
			hpack_table_free(&trial, &s->mem);
			return HIVE_ERR_NOMEM;
		}

		needed_iov = n_frames * 2u;
		if ((size_t)s->send_iov_count + needed_iov >
		    (size_t)s->opt_max_send_iov) {
			hpack_table_free(&trial, &s->mem);
			return HIVE_ERR_NOMEM;
		}

		pos = 0u;
		frame_idx = 0u;
		while (pos < encoded_len) {
			size_t chunk_len;
			uint8_t hdr_flags;
			int is_last;

			chunk_len = (size_t)max_frame;
			if (chunk_len > encoded_len - pos)
				chunk_len = encoded_len - pos;

			is_last = (pos + chunk_len >= encoded_len) ? 1 : 0;
			hdr_flags = is_last ? HIVE_FLAG_END_HEADERS : 0u;

			if (frame_idx == 0u) {
				if (end_stream)
					hdr_flags |= HIVE_FLAG_END_STREAM;
				frame_hdr_write_at(s->send_buf +
				                       first_hdr_offset,
				                   (uint32_t)chunk_len,
				                   HIVE_FRAME_HEADERS,
				                   hdr_flags,
				                   stream_id);
				s->send_iov[s->send_iov_count].iov_base =
				    s->send_buf + first_hdr_offset;
				s->send_iov[s->send_iov_count].iov_len = 9u;
				s->send_iov_count++;
			} else {
				size_t cont_offset;

				cont_offset =
				    cont_hdr_area + (frame_idx - 1u) * 9u;
				frame_hdr_write_at(s->send_buf + cont_offset,
				                   (uint32_t)chunk_len,
				                   HIVE_FRAME_CONTINUATION,
				                   hdr_flags,
				                   stream_id);
				s->send_iov[s->send_iov_count].iov_base =
				    s->send_buf + cont_offset;
				s->send_iov[s->send_iov_count].iov_len = 9u;
				s->send_iov_count++;
			}

			s->send_iov[s->send_iov_count].iov_base =
			    s->send_buf + encode_start + pos;
			s->send_iov[s->send_iov_count].iov_len = chunk_len;
			s->send_iov_count++;

			pos += chunk_len;
			frame_idx++;
		}

		s->send_buf_used = cont_hdr_area + cont_hdr_bytes;
	}
	hpack_table_commit(&s->enc_table, &trial, &s->mem);

	return HIVE_OK;
}

int
send_queue_append_push_promise(hive_session_t *s,
                               uint32_t stream_id,
                               uint32_t promised_stream_id,
                               const hive_nv_t *nva,
                               size_t nvlen)
{
	uint32_t max_frame;
	size_t frame_offset;
	size_t block_start;
	size_t out_cap;
	size_t block_len;
	size_t payload_len;
	hpack_table_t trial;
	int rc;

	if (s == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (nvlen > 0 && nva == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (s->send_buf == NULL || s->send_iov == NULL)
		return HIVE_ERR_INVALID_ARG;

	max_frame = s->remote_settings.max_frame_size;
	if (max_frame < 4u)
		return HIVE_ERR_INVALID_ARG;

	rc = send_queue_reserve_iov(s, 1);
	if (rc != HIVE_OK)
		return rc;

	frame_offset = s->send_buf_used;
	if (frame_offset + 13u > s->send_buf_cap)
		return HIVE_ERR_NOMEM;

	block_start = frame_offset + 9u + 4u;
	out_cap = s->send_buf_cap - block_start;
	if (out_cap > (size_t)s->opt_max_continuation_size)
		out_cap = (size_t)s->opt_max_continuation_size;

	memset(&trial, 0, sizeof(trial));
	rc = hpack_table_clone(&s->enc_table, &s->mem, &trial);
	if (rc != HIVE_OK)
		return rc;
	rc = hpack_encode_block(&trial,
	                        &s->mem,
	                        nva,
	                        nvlen,
	                        s->send_buf + block_start,
	                        out_cap,
	                        &block_len);
	if (rc != HIVE_OK) {
		hpack_table_free(&trial, &s->mem);
		return rc;
	}

	payload_len = 4u + block_len;
	if (payload_len > (size_t)max_frame) {
		hpack_table_free(&trial, &s->mem);
		return HIVE_ERR_NOMEM;
	}

	u32_write_be(s->send_buf + frame_offset + 9u,
	             promised_stream_id & 0x7fffffffu);
	frame_hdr_write_at(s->send_buf + frame_offset,
	                   (uint32_t)payload_len,
	                   HIVE_FRAME_PUSH_PROMISE,
	                   HIVE_FLAG_END_HEADERS,
	                   stream_id);

	s->send_iov[s->send_iov_count].iov_base = s->send_buf + frame_offset;
	s->send_iov[s->send_iov_count].iov_len = 9u + payload_len;
	s->send_iov_count++;
	s->send_buf_used = block_start + block_len;
	hpack_table_commit(&s->enc_table, &trial, &s->mem);

	return HIVE_OK;
}

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
		uint32_t stream_id;
		uint32_t data_flags;
		uint8_t hdr_flags;
		size_t hdr_offset;
		size_t avail;
		size_t nbytes;
		uint8_t *body_ptr;
		ssize_t nread;

		st = &s->stream_slots[i];
		if (st->stream_id == 0)
			continue;

		cb = st->data_source.read_callback;
		if (cb == NULL)
			continue;
		stream_id = st->stream_id;

		/*
		 * SECURITY: send-window gating - skip this stream if either
		 * the connection-level or stream-level send window is zero
		 * or negative.  This enforces flow control limits advertised
		 * by the peer.  See ARCHITECTURE.md §6.5.
		 */
		if (s->send_window <= 0 || st->send_window <= 0)
			continue;
		if (s->send_iov_count + 2 > (int)s->opt_max_send_iov)
			continue;

		/*
		 * SECURITY: outbound DATA frame sizing must respect the
		 * peer's advertised max_frame_size (remote_settings) and
		 * both send windows.  Using local_settings here would be
		 * a protocol violation.  See ARCHITECTURE.md §2.5.
		 */
		max_len = s->remote_settings.max_frame_size;
		if ((uint32_t)s->send_window < max_len)
			max_len = (uint32_t)s->send_window;
		if ((uint32_t)st->send_window < max_len)
			max_len = (uint32_t)st->send_window;

		avail = s->send_buf_cap - s->send_buf_used;
		if (avail <= 9u)
			continue;
		if ((size_t)max_len > avail - 9u)
			max_len = (uint32_t)(avail - 9u);
		if (max_len == 0)
			continue;

		hdr_offset = s->send_buf_used;
		s->send_buf_used += 9u;
		s->send_iov[s->send_iov_count].iov_base =
		    s->send_buf + hdr_offset;
		s->send_iov[s->send_iov_count].iov_len = 9u;
		s->send_iov_count++;

		body_ptr = s->send_buf + s->send_buf_used;
		data_flags = 0u;
		nread = cb(s,
		           stream_id,
		           &body_ptr,
		           max_len,
		           &data_flags,
		           &st->data_source,
		           s->user_data);
		if (nread < 0) {
			s->send_iov_count--;
			s->send_buf_used -= 9u;
			continue;
		}

		nbytes = (size_t)nread;
		if (nbytes > (size_t)max_len) {
			s->send_iov_count--;
			s->send_buf_used -= 9u;
			continue;
		}

		if (nbytes == 0u && (data_flags & HIVE_DATA_FLAG_EOF) == 0u) {
			s->send_iov_count--;
			s->send_buf_used -= 9u;
			st->data_source.read_callback = NULL;
			continue;
		}

		if ((data_flags & HIVE_DATA_FLAG_NO_COPY) != 0u) {
			/* SECURITY: NO_COPY iovec points at caller-owned
			 * memory. */
			s->send_iov[s->send_iov_count].iov_base = body_ptr;
			s->send_iov[s->send_iov_count].iov_len = nbytes;
			s->send_iov_count++;
		} else {
			s->send_iov[s->send_iov_count].iov_base =
			    s->send_buf + s->send_buf_used;
			s->send_iov[s->send_iov_count].iov_len = nbytes;
			s->send_iov_count++;
			s->send_buf_used += nbytes;
		}

		hdr_flags = 0u;
		if ((data_flags & HIVE_DATA_FLAG_EOF) != 0u) {
			hdr_flags |= HIVE_FLAG_END_STREAM;
			st->data_source.read_callback = NULL;
			if (st->state == HIVE_STREAM_OPEN) {
				st->state = HIVE_STREAM_HALF_CLOSED_LOCAL;
			} else if (st->state ==
			           HIVE_STREAM_HALF_CLOSED_REMOTE) {
				st->state = HIVE_STREAM_CLOSED;
				s->send_window -= (int32_t)nbytes;
				st->send_window -= (int32_t)nbytes;
				if (s->callbacks.on_stream_close != NULL)
					(void)s->callbacks.on_stream_close(
					    s,
					    stream_id,
					    HIVE_H2_NO_ERROR,
					    s->user_data);
				stream_close(s, st);
				st = NULL;
			}
		}

		frame_hdr_write_at(s->send_buf + hdr_offset,
		                   (uint32_t)nbytes,
		                   HIVE_FRAME_DATA,
		                   hdr_flags,
		                   stream_id);

		if (st != NULL) {
			s->send_window -= (int32_t)nbytes;
			st->send_window -= (int32_t)nbytes;
		}
	}
}
