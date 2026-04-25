/*
 * hive_frame.c -- Phase 2 receive state machine
 *
 * Implements DEVELOPMENT.md Tasks 2.3/2.4 with the minimal Phase 2
 * internal session layout from Task 2.5.
 */

#include "hive_frame.h"
#include "hive_frame_bare.h"
#include "hive_hpack.h"
#include "hive_internal.h"
#include "hive_send.h"

static void
copy_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		dst[i] = src[i];
	}
}

static void
zero_bytes(uint8_t *dst, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		dst[i] = 0;
	}
}

static int
bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return 0;
		}
	}
	return 1;
}

static int
session_error(hive_session_t *s, int hive_err, uint32_t h2_err)
{
	s->last_err = hive_err;
	s->last_h2_err = h2_err;
	s->closed = 1;
	if (s->callbacks.on_connection_error != NULL) {
		(void)s->callbacks.on_connection_error(
		    s, hive_err, h2_err, s->user_data);
	}
	return -1;
}

static uint32_t
u32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int
frame_size_error(hive_session_t *s)
{
	return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_FRAME_SIZE_ERROR);
}

static int
protocol_error(hive_session_t *s)
{
	return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR);
}

static int
flow_control_error(hive_session_t *s)
{
	return session_error(
	    s, HIVE_ERR_FLOW_CONTROL, HIVE_H2_FLOW_CONTROL_ERROR);
}

static int
stream_is_locally_initiated(const hive_session_t *s, uint32_t stream_id)
{
	if (s->role == HIVE_ROLE_SERVER)
		return ((stream_id & 1u) == 0u);
	return ((stream_id & 1u) != 0u);
}

static int
stream_was_idle(const hive_session_t *s, uint32_t stream_id)
{
	if (stream_is_locally_initiated(s, stream_id))
		return (stream_id > s->last_stream_id_local);
	return (stream_id > s->last_stream_id_remote);
}

static void
u32be_write(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)((v >> 24) & 0xffu);
	p[1] = (uint8_t)((v >> 16) & 0xffu);
	p[2] = (uint8_t)((v >> 8) & 0xffu);
	p[3] = (uint8_t)(v & 0xffu);
}

static int
stream_error(hive_session_t *s,
             uint32_t stream_id,
             int hive_err,
             uint32_t h2_err)
{
	uint8_t payload[4];

	u32be_write(payload, h2_err);
	send_queue_append_ctrl(
	    s, HIVE_FRAME_RST_STREAM, 0u, stream_id, payload, 4u);
	s->last_err = hive_err;
	s->last_h2_err = h2_err;
	return 0;
}

static int
headers_callbacks_enabled(const hive_session_t *s)
{
	return (s->callbacks.on_begin_headers != NULL ||
	        s->callbacks.on_header != NULL ||
	        s->callbacks.on_headers_complete != NULL);
}

static int
headers_decode_complete(hive_session_t *s,
                        uint32_t stream_id,
                        uint8_t end_stream)
{
	int ret;

	if (!headers_callbacks_enabled(s))
		return 0;

	s->reassembly_stream_id = stream_id;
	s->reassembly_end_stream = end_stream;
	ret = hpack_decode_block(
	    s, s->reassembly_buf, s->reassembly_len, 0, stream_id);
	if (ret == HIVE_OK) {
		s->reassembly_len = 0;
		return 0;
	}
	if (ret == HIVE_ERR_COMPRESSION)
		return session_error(
		    s, HIVE_ERR_COMPRESSION, HIVE_H2_COMPRESSION_ERROR);
	return protocol_error(s);
}

static int
settings_apply_initial_window(hive_session_t *s, uint32_t val)
{
	int64_t delta;
	uint32_t i;

	if (val > 0x7fffffffU)
		return flow_control_error(s);

	delta = (int64_t)(int32_t)val -
	        (int64_t)(int32_t)s->remote_settings.initial_window_size;
	s->remote_settings.initial_window_size = val;

	if (s->stream_slots == NULL)
		return 0;

	for (i = 0; i < s->opt_max_concurrent_streams; i++) {
		hive_stream_t *st;
		int64_t new_window;

		st = &s->stream_slots[i];
		if (st->stream_id == 0)
			continue;
		new_window = (int64_t)st->send_window + delta;
		if (new_window > 0x7fffffffLL || new_window < -2147483648LL)
			return flow_control_error(s);
		st->send_window = (int32_t)new_window;
	}

	return 0;
}

static int
settings_apply_param(hive_session_t *s, uint16_t param_id, uint32_t param_val)
{
	switch (param_id) {
	case HIVE_SETTINGS_HEADER_TABLE_SIZE:
		s->remote_settings.header_table_size = param_val;
		if (s->enc_table.has_pending == 0 ||
		    param_val < s->enc_table.pending_min) {
			s->enc_table.pending_min = param_val;
		}
		s->enc_table.pending_max = param_val;
		s->enc_table.has_pending = 1;
		break;
	case HIVE_SETTINGS_ENABLE_PUSH:
		if (param_val > 1)
			return protocol_error(s);
		if (s->role == HIVE_ROLE_CLIENT && param_val == 1)
			return protocol_error(s);
		s->remote_settings.enable_push = param_val;
		break;
	case HIVE_SETTINGS_MAX_CONCURRENT_STREAMS:
		s->remote_settings.max_concurrent_streams = param_val;
		break;
	case HIVE_SETTINGS_INITIAL_WINDOW_SIZE:
		return settings_apply_initial_window(s, param_val);
	case HIVE_SETTINGS_MAX_FRAME_SIZE:
		if (param_val < 16384u || param_val > 16777215u)
			return protocol_error(s);
		s->remote_settings.max_frame_size = param_val;
		break;
	case HIVE_SETTINGS_MAX_HEADER_LIST_SIZE:
		s->remote_settings.max_header_list_size = param_val;
		break;
	default:
		/* Unknown SETTINGS parameter IDs are ignored per RFC 9113 §6.5.
		 */
		break;
	}

	return 0;
}

static int
settings_payload_complete(hive_session_t *s)
{
	if ((s->cur_frame.flags & HIVE_FLAG_ACK) != 0) {
		if (s->pending_count == 0)
			return protocol_error(s);
		if (s->pending_settings != NULL) {
			zero_bytes(
			    (uint8_t *)&s->pending_settings[s->pending_head],
			    sizeof(s->pending_settings[s->pending_head]));
		}
		s->pending_head = (uint8_t)((s->pending_head + 1u) %
		                            s->opt_max_settings_pending);
		s->pending_count--;
		if (s->callbacks.on_settings_ack != NULL) {
			(void)s->callbacks.on_settings_ack(s, s->user_data);
		}
		return 0;
	}

	s->inbound_settings_count++;
	if (s->inbound_settings_count > s->opt_max_settings_pending)
		return protocol_error(s);

	send_queue_append_ctrl(
	    s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
	s->inbound_settings_count--;

	if (s->callbacks.on_settings != NULL)
		(void)s->callbacks.on_settings(s, s->user_data);

	return 0;
}

static int
headers_open_new_stream(hive_session_t *s, uint32_t stream_id)
{
	int expect_peer_odd;
	int ret;

	if (s->stream_hash == NULL || s->stream_slots == NULL ||
	    s->stream_free_stack == NULL)
		return 0;

	if (stream_lookup(s, stream_id) != NULL)
		return 0;

	expect_peer_odd = (s->role == HIVE_ROLE_SERVER) ? 1 : 0;
	/* SECURITY: New peer-initiated streams must match role parity and be
	 * strictly monotonic. See ARCHITECTURE.md §3.4. */
	if (((stream_id & 1u) != 0u) != expect_peer_odd)
		return protocol_error(s);
	if (stream_id <= s->last_stream_id_remote)
		return protocol_error(s);

	if (s->peer_stream_open_count >= s->opt_max_concurrent_streams)
		return protocol_error(s);
	if (s->stream_open_count >= s->opt_max_concurrent_streams)
		return protocol_error(s);

	ret = stream_open(s, stream_id, HIVE_STREAM_OPEN);
	if (ret != HIVE_OK)
		return protocol_error(s);
	s->last_stream_id_remote = stream_id;
	return 0;
}

static int
frame_header_validate(hive_session_t *s)
{
	const frame_hdr_t *f = &s->cur_frame;

	/* SECURITY: Inbound frame length must not exceed what we advertised
	 * in our local SETTINGS (local_settings.max_frame_size). Validating
	 * against local_settings — not remote_settings — is the correct
	 * directionality: local_settings governs frames we are willing to
	 * receive; remote_settings governs frames we are permitted to send.
	 * See ARCHITECTURE.md §2.5 and CODING_STANDARDS.md §3.1. */
	if (f->length > s->local_settings.max_frame_size) {
		return frame_size_error(s);
	}

	switch (f->type) {
	case HIVE_FRAME_SETTINGS:
		/* SECURITY: A SETTINGS frame with the ACK flag must carry
		 * zero payload (RFC 9113 §6.5); a non-ACK SETTINGS payload
		 * must be an exact multiple of 6 bytes — one 6-byte
		 * parameter record per entry. Any other length is a
		 * FRAME_SIZE_ERROR connection error. */
		if ((f->flags & HIVE_FLAG_ACK) != 0) {
			if (f->length != 0) {
				return frame_size_error(s);
			}
		} else if ((f->length % 6u) != 0) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PING:
		/* SECURITY: PING payload must be exactly 8 bytes
		 * (RFC 9113 §6.7). Any other length is a
		 * FRAME_SIZE_ERROR connection error. */
		if (f->length != 8) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_RST_STREAM:
	case HIVE_FRAME_WINDOW_UPDATE:
		/* SECURITY: RST_STREAM and WINDOW_UPDATE payloads must be
		 * exactly 4 bytes (RFC 9113 §6.4, §6.9). Any other length
		 * is a FRAME_SIZE_ERROR connection error. */
		if (f->length != 4) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PRIORITY:
		/* SECURITY: PRIORITY payload must be exactly 5 bytes
		 * (RFC 9113 §6.3). Any other length is a
		 * FRAME_SIZE_ERROR connection error. */
		if (f->length != 5) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_GOAWAY:
		/* SECURITY: GOAWAY must carry at least 8 bytes:
		 * 4-byte last_stream_id + 4-byte error_code
		 * (RFC 9113 §6.8). Fewer bytes is a FRAME_SIZE_ERROR
		 * connection error. */
		if (f->length < 8) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PUSH_PROMISE:
		/* SECURITY: PUSH_PROMISE must carry at least the 4-byte
		 * promised stream identifier. When the PADDED flag is set a
		 * further 1-byte Pad Length field is prepended, so the
		 * minimum rises to 5 bytes (RFC 9113 §6.6). Shorter frames
		 * are a FRAME_SIZE_ERROR connection error. */
		if ((f->flags & HIVE_FLAG_PADDED) != 0) {
			if (f->length < 5) {
				return frame_size_error(s);
			}
		} else if (f->length < 4) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_DATA:
		/* SECURITY: A padded DATA frame must carry at least 1 byte
		 * for the Pad Length field itself (RFC 9113 §6.1). A
		 * PADDED frame with length 0 is a FRAME_SIZE_ERROR. */
		if ((f->flags & HIVE_FLAG_PADDED) != 0 && f->length < 1) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_HEADERS:
		/* SECURITY: A HEADERS frame must be large enough to hold the
		 * optional Pad Length byte (PADDED flag) and the optional
		 * 5-byte PRIORITY prefix (PRIORITY flag), alone or in
		 * combination (RFC 9113 §6.2). Any combination that leaves
		 * insufficient room is a FRAME_SIZE_ERROR connection error:
		 *   PADDED + PRIORITY : minimum 6 bytes
		 *   PRIORITY only     : minimum 5 bytes
		 *   PADDED only       : minimum 1 byte */
		if ((f->flags & HIVE_FLAG_PADDED) != 0 &&
		    (f->flags & HIVE_FLAG_PRIORITY) != 0 && f->length < 6) {
			return frame_size_error(s);
		}
		if ((f->flags & HIVE_FLAG_PADDED) == 0 &&
		    (f->flags & HIVE_FLAG_PRIORITY) != 0 && f->length < 5) {
			return frame_size_error(s);
		}
		if ((f->flags & HIVE_FLAG_PADDED) != 0 &&
		    (f->flags & HIVE_FLAG_PRIORITY) == 0 && f->length < 1) {
			return frame_size_error(s);
		}
		break;
	default:
		break;
	}

	switch (f->type) {
	case HIVE_FRAME_DATA:
	case HIVE_FRAME_HEADERS:
	case HIVE_FRAME_RST_STREAM:
	case HIVE_FRAME_PRIORITY:
	case HIVE_FRAME_CONTINUATION:
	case HIVE_FRAME_PUSH_PROMISE:
		/* SECURITY: These frame types are stream-associated and must
		 * not appear on the connection-control stream (stream_id 0).
		 * Receiving any of them on stream 0 is a connection error
		 * (RFC 9113 §6.1, §6.2, §6.3, §6.4, §6.6, §6.10). */
		if (f->stream_id == 0) {
			return protocol_error(s);
		}
		break;
	case HIVE_FRAME_SETTINGS:
	case HIVE_FRAME_PING:
	case HIVE_FRAME_GOAWAY:
		/* SECURITY: Connection-level control frames must only appear
		 * on stream 0. A non-zero stream_id is a connection error
		 * (RFC 9113 §6.5, §6.7, §6.8). */
		if (f->stream_id != 0) {
			return protocol_error(s);
		}
		break;
	default:
		break;
	}

	return 0;
}

void
frame_recv_init(hive_session_t *s, hive_role_t role)
{
	zero_bytes((uint8_t *)s, sizeof(*s));
	s->role = (uint8_t)role;
	s->opt_max_frame_size = 16384;
	s->opt_max_continuation_size = 65536;
	s->local_settings.initial_window_size = 65535;
	s->local_settings.max_frame_size = 16384;
	s->remote_settings.initial_window_size = 65535;
	s->send_window = 65535;
	s->recv_window = 65535;
	s->recv_state = RECV_FRAME_HEADER;
}

ssize_t
frame_recv_process(hive_session_t *s, const uint8_t *data, size_t len)
{
	size_t consumed;

	if (s == NULL || (len > 0 && data == NULL)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	consumed = 0;
	while (consumed < len) {
		size_t avail;
		size_t n;

		avail = len - consumed;
		switch ((hive_recv_state_t)s->recv_state) {
		case RECV_CLIENT_PREFACE:
			n = 24u - s->preface_count;
			if (n > avail) {
				n = avail;
			}
			if (!bytes_equal(data + consumed,
			                 client_preface_magic +
			                     s->preface_count,
			                 n)) {
				return protocol_error(s);
			}
			s->preface_count += (uint8_t)n;
			consumed += n;
			if (s->preface_count == 24) {
				s->preface_count = 1;
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_SERVER_PREFACE:
			/*
			 * Set the preface_count=1 sentinel so RECV_FRAME_HEADER
			 * enforces that the first received frame must be a
			 * non-ACK SETTINGS (RFC 9113 §3.4).  No bytes are
			 * consumed here — the state transitions immediately so
			 * the incoming bytes are processed by RECV_FRAME_HEADER
			 * on the very next loop iteration.
			 */
			s->preface_count = 1;
			s->recv_state = RECV_FRAME_HEADER;
			break;

		case RECV_FRAME_HEADER:
			n = 9u - s->frame_hdr_count;
			if (n > avail) {
				n = avail;
			}
			copy_bytes(s->frame_hdr_buf + s->frame_hdr_count,
			           data + consumed,
			           n);
			s->frame_hdr_count += (uint8_t)n;
			consumed += n;
			if (s->frame_hdr_count != 9) {
				break;
			}

			frame_hdr_parse(s->frame_hdr_buf, &s->cur_frame);
			s->frame_hdr_count = 0;
			s->payload_remaining = s->cur_frame.length;
			s->pad_remaining = 0;
			s->pad_length_received = 0;
			s->pad_validated = 0;
			s->fc_accounted = 0;
			s->ctrl_staging_count = 0;
			s->priority_payload_len = 0;
			if (s->reassembly_active == 0) {
				s->reassembly_promised_stream_id = 0;
			}

			if (s->preface_count == 1) {
				if (s->cur_frame.type != HIVE_FRAME_SETTINGS ||
				    (s->cur_frame.flags & HIVE_FLAG_ACK) != 0) {
					return protocol_error(s);
				}
				s->preface_count = 0;
			}

			/* SECURITY: CONTINUATION lockout is a connection error.
			 */
			if (s->reassembly_active != 0 &&
			    (s->cur_frame.type != HIVE_FRAME_CONTINUATION ||
			     s->cur_frame.stream_id !=
			         s->reassembly_stream_id)) {
				return protocol_error(s);
			}
			if (s->reassembly_active == 0 &&
			    s->cur_frame.type == HIVE_FRAME_CONTINUATION) {
				return protocol_error(s);
			}

			if (frame_header_validate(s) != 0) {
				return -1;
			}

			if (s->cur_frame.type == HIVE_FRAME_HEADERS) {
				if (headers_open_new_stream(
				        s, s->cur_frame.stream_id) != 0)
					return -1;
			}

			if (s->cur_frame.type == HIVE_FRAME_HEADERS &&
			    (s->cur_frame.flags & HIVE_FLAG_PRIORITY) != 0) {
				s->priority_payload_len = 5;
			}

			switch (s->cur_frame.type) {
			case HIVE_FRAME_DATA:
				s->recv_state = RECV_DATA_PAYLOAD;
				break;
			case HIVE_FRAME_HEADERS:
				s->recv_state = RECV_HEADERS_PAYLOAD;
				if (s->reassembly_active == 0) {
					s->reassembly_len = 0;
				}
				break;
			case HIVE_FRAME_PRIORITY:
				s->recv_state = RECV_PRIORITY_PAYLOAD;
				break;
			case HIVE_FRAME_RST_STREAM:
				s->recv_state = RECV_RST_STREAM_PAYLOAD;
				break;
			case HIVE_FRAME_SETTINGS:
				s->recv_state = RECV_SETTINGS_PAYLOAD;
				break;
			case HIVE_FRAME_PUSH_PROMISE:
				s->recv_state = RECV_PUSH_PROMISE_PAYLOAD;
				if (s->reassembly_active == 0) {
					s->reassembly_len = 0;
				}
				break;
			case HIVE_FRAME_PING:
				s->recv_state = RECV_PING_PAYLOAD;
				break;
			case HIVE_FRAME_GOAWAY:
				s->recv_state = RECV_GOAWAY_PAYLOAD;
				s->reassembly_len = 0;
				break;
			case HIVE_FRAME_WINDOW_UPDATE:
				s->recv_state = RECV_WINDOW_UPDATE_PAYLOAD;
				break;
			case HIVE_FRAME_CONTINUATION:
				s->recv_state = RECV_CONTINUATION_PAYLOAD;
				break;
			default:
				s->recv_state = RECV_SKIP_PAYLOAD;
				break;
			}
			if (s->payload_remaining == 0) {
				if (s->recv_state == RECV_SETTINGS_PAYLOAD) {
					if (settings_payload_complete(s) != 0)
						return -1;
				}
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_DATA_PAYLOAD: {
			hive_stream_t *st;
			int have_stream_state;
			uint32_t increment;
			int64_t restored;
			uint8_t wu_payload[4];

			st = NULL;
			have_stream_state =
			    (s->stream_hash != NULL && s->stream_slots != NULL);
			if (have_stream_state)
				st = stream_lookup(s, s->cur_frame.stream_id);

			if (have_stream_state && st == NULL) {
				if (stream_was_idle(s, s->cur_frame.stream_id))
					return protocol_error(s);
				if (s->fc_accounted == 0) {
					if ((int64_t)s->cur_frame.length >
					    (int64_t)s->recv_window)
						return flow_control_error(s);
					s->recv_window -=
					    (int32_t)s->cur_frame.length;
					s->recv_consumed += s->cur_frame.length;
					s->fc_accounted = 1;
					if (s->recv_consumed >
					    ((uint32_t)s->recv_window / 2u)) {
						increment = s->recv_consumed;
						restored =
						    (int64_t)s->recv_window +
						    (int64_t)increment;
						if (restored > 0x7fffffffLL)
							return flow_control_error(
							    s);
						u32be_write(wu_payload,
						            increment);
						send_queue_append_ctrl(
						    s,
						    HIVE_FRAME_WINDOW_UPDATE,
						    0u,
						    0u,
						    wu_payload,
						    4u);
						s->recv_window =
						    (int32_t)restored;
						s->recv_consumed = 0;
					}
					(void)stream_error(
					    s,
					    s->cur_frame.stream_id,
					    HIVE_ERR_PROTOCOL,
					    HIVE_H2_STREAM_CLOSED);
				}
				s->recv_state = RECV_SKIP_PAYLOAD;
				break;
			}

			if (have_stream_state &&
			    st->state != HIVE_STREAM_OPEN &&
			    st->state != HIVE_STREAM_HALF_CLOSED_LOCAL) {
				uint32_t h2_err;

				h2_err =
				    (st->state == HIVE_STREAM_RESERVED_LOCAL ||
				     st->state == HIVE_STREAM_RESERVED_REMOTE)
				        ? HIVE_H2_PROTOCOL_ERROR
				        : HIVE_H2_STREAM_CLOSED;
				if (s->fc_accounted == 0) {
					if ((int64_t)s->cur_frame.length >
					    (int64_t)s->recv_window)
						return flow_control_error(s);
					s->recv_window -=
					    (int32_t)s->cur_frame.length;
					s->recv_consumed += s->cur_frame.length;
					s->fc_accounted = 1;
					if (s->recv_consumed >
					    ((uint32_t)s->recv_window / 2u)) {
						increment = s->recv_consumed;
						restored =
						    (int64_t)s->recv_window +
						    (int64_t)increment;
						if (restored > 0x7fffffffLL)
							return flow_control_error(
							    s);
						u32be_write(wu_payload,
						            increment);
						send_queue_append_ctrl(
						    s,
						    HIVE_FRAME_WINDOW_UPDATE,
						    0u,
						    0u,
						    wu_payload,
						    4u);
						s->recv_window =
						    (int32_t)restored;
						s->recv_consumed = 0;
					}
					(void)stream_error(
					    s,
					    s->cur_frame.stream_id,
					    HIVE_ERR_PROTOCOL,
					    h2_err);
				}
				s->recv_state = RECV_SKIP_PAYLOAD;
				break;
			}

			if (s->fc_accounted == 0) {
				/* SECURITY: receive-side flow control
				 * enforcement is accounted once per DATA frame
				 * against the full frame payload length
				 * (including padding), not per recv() chunk;
				 * see ARCHITECTURE.md §3.3 and §8.7. */
				if (have_stream_state &&
				    (int64_t)s->cur_frame.length >
				        (int64_t)st->recv_window) {
					(void)stream_error(
					    s,
					    s->cur_frame.stream_id,
					    HIVE_ERR_FLOW_CONTROL,
					    HIVE_H2_FLOW_CONTROL_ERROR);
					s->fc_accounted = 1;
					s->recv_state = RECV_SKIP_PAYLOAD;
					break;
				}
				if ((int64_t)s->cur_frame.length >
				    (int64_t)s->recv_window) {
					return flow_control_error(s);
				}

				if (have_stream_state) {
					st->recv_window -=
					    (int32_t)s->cur_frame.length;
					st->recv_consumed +=
					    s->cur_frame.length;
				}
				s->recv_window -= (int32_t)s->cur_frame.length;
				s->recv_consumed += s->cur_frame.length;
				s->fc_accounted = 1;

				if (have_stream_state &&
				    st->recv_consumed >
				        ((uint32_t)st->recv_window / 2u)) {
					increment = st->recv_consumed;
					restored = (int64_t)st->recv_window +
					           (int64_t)increment;
					if (restored > 0x7fffffffLL)
						return flow_control_error(s);
					u32be_write(wu_payload, increment);
					send_queue_append_ctrl(
					    s,
					    HIVE_FRAME_WINDOW_UPDATE,
					    0u,
					    s->cur_frame.stream_id,
					    wu_payload,
					    4u);
					st->recv_window = (int32_t)restored;
					st->recv_consumed = 0;
				}

				if (s->recv_consumed >
				    ((uint32_t)s->recv_window / 2u)) {
					increment = s->recv_consumed;
					restored = (int64_t)s->recv_window +
					           (int64_t)increment;
					if (restored > 0x7fffffffLL)
						return flow_control_error(s);
					u32be_write(wu_payload, increment);
					send_queue_append_ctrl(
					    s,
					    HIVE_FRAME_WINDOW_UPDATE,
					    0u,
					    0u,
					    wu_payload,
					    4u);
					s->recv_window = (int32_t)restored;
					s->recv_consumed = 0;
				}
			}

			if ((s->cur_frame.flags & HIVE_FLAG_PADDED) != 0 &&
			    s->pad_length_received == 0) {
				if (avail == 0) {
					break;
				}
				s->pad_remaining = data[consumed];
				s->pad_length_received = 1;
				consumed++;
				s->payload_remaining--;
				/* SECURITY: Pad length must not exceed the
				 * remaining payload bytes after the Pad Length
				 * field itself has been consumed.  An oversized
				 * pad_length is a connection error
				 * (RFC 9113 §6.1). */
				if (s->pad_remaining > s->payload_remaining) {
					return protocol_error(s);
				}
			}
			n = s->payload_remaining - s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0 && s->callbacks.on_data_chunk != NULL) {
				/* SECURITY: on_data_chunk receives a zero-copy
				 * pointer into caller-owned input memory. Its
				 * lifetime is only for the callback duration;
				 * the library cannot poison caller-owned input
				 * after return. */
				(void)s->callbacks.on_data_chunk(
				    s,
				    s->cur_frame.stream_id,
				    data + consumed,
				    n,
				    s->cur_frame.flags,
				    s->user_data);
			}
			consumed += n;
			s->payload_remaining -= (uint32_t)n;
			if (s->payload_remaining == s->pad_remaining) {
				if (s->pad_remaining > 0) {
					s->recv_state = RECV_DATA_PAD;
				} else {
					s->recv_state = RECV_FRAME_HEADER;
				}
			}
			break;
		}

		case RECV_DATA_PAD:
			n = s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			consumed += n;
			s->pad_remaining -= (uint32_t)n;
			s->payload_remaining -= (uint32_t)n;
			if (s->pad_remaining == 0) {
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_HEADERS_PAYLOAD:
			if ((s->cur_frame.flags & HIVE_FLAG_PADDED) != 0 &&
			    s->pad_length_received == 0) {
				if (avail == 0) {
					break;
				}
				s->pad_remaining = data[consumed];
				s->pad_length_received = 1;
				consumed++;
				s->payload_remaining--;
			}
			if (s->priority_payload_len > 0) {
				n = s->priority_payload_len;
				if (n > avail) {
					n = avail;
				}
				if (n > s->payload_remaining) {
					n = s->payload_remaining;
				}
				consumed += n;
				s->priority_payload_len -= (uint8_t)n;
				s->payload_remaining -= (uint32_t)n;
				break;
			}
			if (s->pad_length_received != 0 &&
			    s->pad_validated == 0) {
				/* SECURITY: Pad length must not exceed the
				 * remaining payload bytes after the Pad Length
				 * field (and any PRIORITY prefix) have been
				 * consumed.  Oversized padding is a connection
				 * error (RFC 9113 §6.2). */
				if (s->pad_remaining > s->payload_remaining) {
					return protocol_error(s);
				}
				s->pad_validated = 1;
			}
			n = s->payload_remaining - s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0) {
				/* SECURITY: Total accumulated header block must
				 * not exceed opt_max_continuation_size.  This
				 * prevents header-block bomb attacks where an
				 * adversary chains many CONTINUATION frames to
				 * exhaust reassembly memory
				 * (ARCHITECTURE.md §8.3). */
				if ((s->reassembly_len + n) >
				    s->opt_max_continuation_size) {
					return protocol_error(s);
				}
				if (s->reassembly_buf != NULL) {
					copy_bytes(s->reassembly_buf +
					               s->reassembly_len,
					           data + consumed,
					           n);
				}
				s->reassembly_len += (uint32_t)n;
				consumed += n;
				s->payload_remaining -= (uint32_t)n;
			}
			if (s->payload_remaining == s->pad_remaining) {
				if ((s->cur_frame.flags &
				     HIVE_FLAG_END_HEADERS) != 0) {
					s->reassembly_active = 0;
					if (headers_decode_complete(
					        s,
					        s->cur_frame.stream_id,
					        (uint8_t)((s->cur_frame.flags &
					                   HIVE_FLAG_END_STREAM) !=
					                  0)) != 0)
						return -1;
					if (s->pad_remaining > 0) {
						s->recv_state =
						    RECV_HEADERS_PAD;
					} else {
						s->recv_state =
						    RECV_FRAME_HEADER;
					}
				} else {
					s->reassembly_active = 1;
					s->reassembly_type = 0;
					s->reassembly_end_stream =
					    (s->cur_frame.flags &
					     HIVE_FLAG_END_STREAM) != 0;
					s->reassembly_stream_id =
					    s->cur_frame.stream_id;
					s->recv_state = RECV_FRAME_HEADER;
				}
			}
			break;

		case RECV_HEADERS_PAD:
			n = s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			consumed += n;
			s->pad_remaining -= (uint32_t)n;
			s->payload_remaining -= (uint32_t)n;
			if (s->pad_remaining == 0) {
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_CONTINUATION_PAYLOAD:
			n = s->payload_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0) {
				/* SECURITY: Accumulated header block across
				 * HEADERS + CONTINUATION frames must not exceed
				 * opt_max_continuation_size.  Prevents
				 * CONTINUATION flood / header-block bomb
				 * attacks (ARCHITECTURE.md §8.3). */
				if ((s->reassembly_len + n) >
				    s->opt_max_continuation_size) {
					return protocol_error(s);
				}
				if (s->reassembly_buf != NULL) {
					copy_bytes(s->reassembly_buf +
					               s->reassembly_len,
					           data + consumed,
					           n);
				}
				s->reassembly_len += (uint32_t)n;
				consumed += n;
				s->payload_remaining -= (uint32_t)n;
			}
			if (s->payload_remaining == 0) {
				if ((s->cur_frame.flags &
				     HIVE_FLAG_END_HEADERS) != 0) {
					s->reassembly_active = 0;
					if (s->reassembly_type == 0 &&
					    headers_decode_complete(
					        s,
					        s->reassembly_stream_id,
					        s->reassembly_end_stream) != 0)
						return -1;
				}
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_PUSH_PROMISE_PAYLOAD:
			if ((s->cur_frame.flags & HIVE_FLAG_PADDED) != 0 &&
			    s->pad_length_received == 0) {
				if (avail == 0) {
					break;
				}
				s->pad_remaining = data[consumed];
				s->pad_length_received = 1;
				consumed++;
				s->payload_remaining--;
			}
			if (s->pad_validated == 0) {
				while (s->ctrl_staging_count < 4 &&
				       s->payload_remaining > 0 &&
				       consumed < len) {
					s->ctrl_staging
					    [s->ctrl_staging_count++] =
					    data[consumed++];
					s->payload_remaining--;
				}
				if (s->ctrl_staging_count < 4) {
					break;
				}
				s->reassembly_promised_stream_id =
				    u32be(s->ctrl_staging) & 0x7fffffffU;
				s->ctrl_staging_count = 0;
				/* SECURITY: Pad length must not exceed the
				 * remaining payload bytes after the
				 * promised_stream_id field has been consumed.
				 * Oversized padding is a connection error
				 * (RFC 9113 §6.6). */
				if (s->pad_remaining > s->payload_remaining) {
					return protocol_error(s);
				}
				s->pad_validated = 1;
			}
			avail = len - consumed;
			n = s->payload_remaining - s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0) {
				/* SECURITY: Total header block accumulated via
				 * PUSH_PROMISE + CONTINUATION must not exceed
				 * opt_max_continuation_size.  Prevents
				 * header-block bomb attacks
				 * (ARCHITECTURE.md §8.2). */
				if ((s->reassembly_len + n) >
				    s->opt_max_continuation_size) {
					return protocol_error(s);
				}
				if (s->reassembly_buf != NULL) {
					copy_bytes(s->reassembly_buf +
					               s->reassembly_len,
					           data + consumed,
					           n);
				}
				s->reassembly_len += (uint32_t)n;
				consumed += n;
				s->payload_remaining -= (uint32_t)n;
			}
			if (s->payload_remaining == s->pad_remaining) {
				if ((s->cur_frame.flags &
				     HIVE_FLAG_END_HEADERS) != 0) {
					s->reassembly_active = 0;
					if (s->pad_remaining > 0) {
						s->recv_state =
						    RECV_PUSH_PROMISE_PAD;
					} else {
						s->recv_state =
						    RECV_FRAME_HEADER;
					}
				} else {
					s->reassembly_active = 1;
					s->reassembly_type = 1;
					s->reassembly_stream_id =
					    s->cur_frame.stream_id;
					s->recv_state = RECV_FRAME_HEADER;
				}
			}
			break;

		case RECV_PUSH_PROMISE_PAD:
			n = s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			consumed += n;
			s->pad_remaining -= (uint32_t)n;
			s->payload_remaining -= (uint32_t)n;
			if (s->pad_remaining == 0) {
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_SETTINGS_PAYLOAD:
			n = 6u - s->ctrl_staging_count;
			if (n > s->payload_remaining)
				n = s->payload_remaining;
			if (n > avail)
				n = avail;
			if (n > 0) {
				copy_bytes(s->ctrl_staging +
				               s->ctrl_staging_count,
				           data + consumed,
				           n);
				s->ctrl_staging_count += (uint8_t)n;
				consumed += n;
				s->payload_remaining -= (uint32_t)n;
			}
			if (s->ctrl_staging_count == 6) {
				uint16_t param_id;
				uint32_t param_val;

				param_id =
				    (uint16_t)(((uint16_t)s->ctrl_staging[0]
				                << 8) |
				               (uint16_t)s->ctrl_staging[1]);
				param_val =
				    ((uint32_t)s->ctrl_staging[2] << 24) |
				    ((uint32_t)s->ctrl_staging[3] << 16) |
				    ((uint32_t)s->ctrl_staging[4] << 8) |
				    (uint32_t)s->ctrl_staging[5];
				if (settings_apply_param(
				        s, param_id, param_val) != 0)
					return -1;
				s->ctrl_staging_count = 0;
			}
			if (s->payload_remaining == 0) {
				if (settings_payload_complete(s) != 0)
					return -1;
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_PING_PAYLOAD:
			while (s->payload_remaining > 0 && consumed < len &&
			       s->ctrl_staging_count < 8) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->payload_remaining == 0) {
				s->ctrl_staging_count = 0;
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_RST_STREAM_PAYLOAD:
			while (s->payload_remaining > 0 && consumed < len &&
			       s->ctrl_staging_count < 4) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->payload_remaining == 0) {
				s->ctrl_staging_count = 0;
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_WINDOW_UPDATE_PAYLOAD:
			while (s->payload_remaining > 0 && consumed < len &&
			       s->ctrl_staging_count < 4) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->payload_remaining == 0) {
				hive_stream_t *st;
				uint32_t increment;
				int64_t new_window;

				increment =
				    u32be(s->ctrl_staging) & 0x7fffffffU;
				s->ctrl_staging_count = 0;

				if (increment == 0) {
					if (s->cur_frame.stream_id == 0)
						return protocol_error(s);
					(void)stream_error(
					    s,
					    s->cur_frame.stream_id,
					    HIVE_ERR_PROTOCOL,
					    HIVE_H2_PROTOCOL_ERROR);
					s->recv_state = RECV_FRAME_HEADER;
					return (ssize_t)consumed;
				}

				if (s->cur_frame.stream_id == 0) {
					/* SECURITY: Connection-level send
					 * window is bounded to signed 31-bit
					 * range. Overflow is
					 * FLOW_CONTROL_ERROR. */
					new_window = (int64_t)s->send_window +
					             (int64_t)increment;
					if (new_window > 0x7fffffffLL)
						return flow_control_error(s);
					s->send_window = (int32_t)new_window;
					s->recv_state = RECV_FRAME_HEADER;
					break;
				}

				st = NULL;
				if (s->stream_hash != NULL &&
				    s->stream_slots != NULL)
					st = stream_lookup(
					    s, s->cur_frame.stream_id);
				if (s->stream_hash == NULL ||
				    s->stream_slots == NULL) {
					s->recv_state = RECV_FRAME_HEADER;
					break;
				}
				if (st == NULL) {
					if (stream_was_idle(
					        s, s->cur_frame.stream_id))
						return protocol_error(s);
					s->recv_state = RECV_FRAME_HEADER;
					break;
				}
				/* SECURITY: Stream-level send window is
				 * bounded to signed 31-bit range.
				 * Overflow is FLOW_CONTROL_ERROR and
				 * must stay stream-scoped (RST_STREAM).
				 */
				new_window = (int64_t)st->send_window +
				             (int64_t)increment;
				if (new_window > 0x7fffffffLL) {
					(void)stream_error(
					    s,
					    s->cur_frame.stream_id,
					    HIVE_ERR_FLOW_CONTROL,
					    HIVE_H2_FLOW_CONTROL_ERROR);
					s->recv_state = RECV_FRAME_HEADER;
					break;
				}
				st->send_window = (int32_t)new_window;

				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_GOAWAY_PAYLOAD:
			while (s->payload_remaining > 0 && consumed < len &&
			       s->ctrl_staging_count < 8) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->ctrl_staging_count == 8) {
				s->goaway_last_stream_id_recv =
				    u32be(s->ctrl_staging) & 0x7fffffffU;
				s->goaway_error_code_recv =
				    u32be(s->ctrl_staging + 4);
				s->ctrl_staging_count = 0;
				if (s->payload_remaining == 0) {
					if (s->callbacks.on_goaway != NULL) {
						(void)s->callbacks.on_goaway(
						    s,
						    s->goaway_last_stream_id_recv,
						    s->goaway_error_code_recv,
						    NULL,
						    0,
						    s->user_data);
					}
					s->recv_state = RECV_FRAME_HEADER;
				} else {
					s->reassembly_len = 0;
					s->recv_state = RECV_GOAWAY_DEBUG;
				}
			}
			break;

		case RECV_GOAWAY_DEBUG:
			n = s->payload_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0) {
				if (s->reassembly_buf != NULL) {
					copy_bytes(s->reassembly_buf +
					               s->reassembly_len,
					           data + consumed,
					           n);
				}
				s->reassembly_len += (uint32_t)n;
				s->payload_remaining -= (uint32_t)n;
				consumed += n;
			}
			if (s->payload_remaining == 0) {
				if (s->callbacks.on_goaway != NULL) {
					(void)s->callbacks.on_goaway(
					    s,
					    s->goaway_last_stream_id_recv,
					    s->goaway_error_code_recv,
					    s->reassembly_buf,
					    s->reassembly_len,
					    s->user_data);
				}
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_PRIORITY_PAYLOAD:
			while (s->payload_remaining > 0 && consumed < len &&
			       s->ctrl_staging_count < 5) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->payload_remaining == 0) {
				s->ctrl_staging_count = 0;
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_SKIP_PAYLOAD:
			n = s->payload_remaining;
			if (n > avail) {
				n = avail;
			}
			consumed += n;
			s->payload_remaining -= (uint32_t)n;
			if (s->payload_remaining == 0) {
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;
		}
	}

	return (ssize_t)consumed;
}
