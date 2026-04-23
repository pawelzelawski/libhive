/*
 * hive_frame.c -- Phase 2 receive state machine
 *
 * Implements DEVELOPMENT.md Tasks 2.3/2.4 with the minimal Phase 2
 * internal session layout from Task 2.5.
 */

#include "hive_frame.h"
#include "hive_frame_bare.h"
#include "hive_internal.h"

static const uint8_t client_preface_magic[24] = {
    'P', 'R', 'I',  ' ',  '*',  ' ',  'H', 'T', 'T',  'P',  '/',  '2',
    '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};

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
frame_header_validate(hive_session_t *s)
{
	const frame_hdr_t *f = &s->cur_frame;

	if (f->length > s->local_settings.max_frame_size) {
		return frame_size_error(s);
	}

	switch (f->type) {
	case HIVE_FRAME_SETTINGS:
		if ((f->flags & HIVE_FLAG_ACK) != 0) {
			if (f->length != 0) {
				return frame_size_error(s);
			}
		} else if ((f->length % 6u) != 0) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PING:
		if (f->length != 8) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_RST_STREAM:
	case HIVE_FRAME_WINDOW_UPDATE:
		if (f->length != 4) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PRIORITY:
		if (f->length != 5) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_GOAWAY:
		if (f->length < 8) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_PUSH_PROMISE:
		if ((f->flags & HIVE_FLAG_PADDED) != 0) {
			if (f->length < 5) {
				return frame_size_error(s);
			}
		} else if (f->length < 4) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_DATA:
		if ((f->flags & HIVE_FLAG_PADDED) != 0 && f->length < 1) {
			return frame_size_error(s);
		}
		break;
	case HIVE_FRAME_HEADERS:
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
		if (f->stream_id == 0) {
			return protocol_error(s);
		}
		break;
	case HIVE_FRAME_SETTINGS:
	case HIVE_FRAME_PING:
	case HIVE_FRAME_GOAWAY:
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
	s->role = role;
	s->opt_max_frame_size = 16384;
	s->opt_max_continuation_size = 65536;
	s->local_settings.max_frame_size = 16384;
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
				if (s->recv_state == RECV_GOAWAY_PAYLOAD) {
					s->goaway_last_stream_id_recv = 0;
					s->goaway_error_code_recv = 0;
					if (s->callbacks.on_goaway != NULL) {
						(void)s->callbacks.on_goaway(
						    s,
						    s->goaway_last_stream_id_recv,
						    s->goaway_error_code_recv,
						    NULL,
						    0,
						    s->user_data);
					}
				}
				s->recv_state = RECV_FRAME_HEADER;
			}
			break;

		case RECV_DATA_PAYLOAD:
			if ((s->cur_frame.flags & HIVE_FLAG_PADDED) != 0 &&
			    s->pad_length_received == 0) {
				if (avail == 0) {
					break;
				}
				s->pad_remaining = data[consumed];
				s->pad_length_received = 1;
				consumed++;
				s->payload_remaining--;
				if (s->pad_remaining > s->payload_remaining) {
					return protocol_error(s);
				}
			}
			n = s->payload_remaining - s->pad_remaining;
			if (n > avail) {
				n = avail;
			}
			if (n > 0 && s->callbacks.on_data_chunk != NULL) {
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
						    RECV_HEADERS_PAD;
					} else {
						s->recv_state =
						    RECV_FRAME_HEADER;
					}
				} else {
					s->reassembly_active = 1;
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
			while (s->ctrl_staging_count < 4 &&
			       s->payload_remaining > 0 && consumed < len) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
			}
			if (s->ctrl_staging_count < 4) {
				break;
			}
			if (s->pad_validated == 0) {
				s->reassembly_promised_stream_id =
				    u32be(s->ctrl_staging) & 0x7fffffffU;
				s->ctrl_staging_count = 0;
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
			if ((s->cur_frame.flags & HIVE_FLAG_ACK) != 0) {
				s->payload_remaining = 0;
				s->recv_state = RECV_FRAME_HEADER;
				break;
			}
			while (s->payload_remaining > 0 && consumed < len) {
				s->ctrl_staging[s->ctrl_staging_count++] =
				    data[consumed++];
				s->payload_remaining--;
				if (s->ctrl_staging_count == 6) {
					s->ctrl_staging_count = 0;
				}
			}
			if (s->payload_remaining == 0) {
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
		case RECV_WINDOW_UPDATE_PAYLOAD:
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
