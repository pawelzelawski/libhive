/* test_frame.c -- Phase 2 frame tests (2.2, 2.3, 2.4) */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../include/hive.h"
#include "../src/hive_frame.h"
#include "../src/hive_frame_bare.h"
#include "../src/hive_internal.h"
#include "test_harness.h"

int test_frame_hdr_write_data(void);
int test_frame_hdr_write_settings(void);
int test_frame_hdr_write_headers(void);
int test_recv_settings_ack(void);
int test_recv_ping(void);
int test_recv_window_update(void);
int test_recv_rst_stream(void);
int test_recv_data_full(void);
int test_recv_data_padded(void);
int test_recv_headers_end_headers(void);
int test_recv_headers_priority_prefix(void);
int test_recv_priority_ignored(void);
int test_recv_unknown_type(void);
int test_recv_split_frame_header(void);
int test_recv_split_data_payload(void);
int test_recv_split_settings_param(void);
int test_recv_headers_plus_continuation(void);
int test_recv_continuation_lockout(void);
int test_recv_continuation_flood_headers_is_connection_error(void);
int test_recv_continuation_flood_continuation_is_connection_error(void);
int test_recv_frame_too_large(void);
int test_recv_data_on_stream_zero(void);
int test_recv_settings_nonzero_stream(void);
int test_recv_ping_wrong_length(void);
int test_recv_rst_stream_wrong_length(void);
int test_recv_settings_bad_length_nonzero_ack(void);
int test_recv_unknown_frame_mid_stream(void);
int test_recv_split_ping_payload(void);
int test_recv_split_window_update_payload(void);
int test_recv_split_rst_stream_payload(void);
int test_recv_split_priority_payload(void);
int test_recv_split_goaway_payload(void);
int test_recv_split_push_promise_payload(void);
int test_recv_split_continuation_payload(void);
int test_recv_settings_bad_length_non_ack(void);
int test_recv_window_update_wrong_length(void);
int test_recv_priority_wrong_length(void);
int test_recv_goaway_too_short(void);
int test_recv_push_promise_wrong_length_unpadded(void);
int test_recv_push_promise_wrong_length_padded(void);

typedef struct {
	const uint8_t *base;
	const uint8_t *last_ptr;
	size_t last_len;
	size_t total;
	uint32_t calls;
} data_capture_t;

static int
on_data_chunk_capture(hive_session_t *session, uint32_t stream_id,
    const uint8_t *data, size_t len, uint8_t flags, void *user_data)
{
	data_capture_t *cap = user_data;
	(void)session;
	(void)stream_id;
	(void)flags;
	cap->last_ptr = data;
	cap->last_len = len;
	cap->total += len;
	cap->calls++;
	ASSERT(data >= cap->base);
	return HIVE_OK;
}

static void
test_session_init(hive_session_t *s, uint8_t *reassembly_buf, size_t cap)
{
	static uint8_t send_buf[2048];
	static struct iovec send_iov[16];
	static hive_settings_t pending_settings[4];

	frame_recv_init(s, HIVE_ROLE_SERVER);
	s->reassembly_buf = reassembly_buf;
	s->opt_max_continuation_size = (uint32_t)cap;
	s->local_settings.max_frame_size = 16384;
	s->remote_settings.initial_window_size = 65535u;
	s->opt_max_concurrent_streams = 1u;
	s->opt_enable_push = 1u;
	s->opt_max_settings_pending = 3u;
	s->send_buf = send_buf;
	s->send_buf_cap = sizeof(send_buf);
	s->send_iov = send_iov;
	s->opt_max_send_iov = 16u;
	s->pending_settings = pending_settings;
	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;
}

static size_t
build_frame(uint8_t *dst, uint32_t length, uint8_t type, uint8_t flags,
    uint32_t stream_id, const uint8_t *payload)
{
	frame_hdr_write_at(dst, length, type, flags, stream_id);
	if (payload != NULL && length > 0) {
		memcpy(dst + 9, payload, length);
	}
	return 9u + length;
}

static uint8_t
queued_frame_type(const hive_session_t *s, int iov_index)
{
	const uint8_t *p;

	p = (const uint8_t *)s->send_iov[iov_index].iov_base;
	return p[3];
}

static int
feed_one_by_one(hive_session_t *s, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (hive_session_recv(s, buf + i, 1) != 1) {
			return 0;
		}
	}
	return 1;
}

/*
 * DATA frame: stream_id=1, length=100 (0x000064), flags=0x01 (END_STREAM).
 *
 * Expected wire bytes (RFC 9113 §4.1):
 *   [0x00, 0x00, 0x64]  — length 100, big-endian 24-bit
 *   [0x00]              — type DATA (0x0)
 *   [0x01]              — flags END_STREAM
 *   [0x00, 0x00, 0x00, 0x01] — stream_id 1, 31-bit big-endian (R=0)
 */
int
test_frame_hdr_write_data(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x64, /* length = 100 */
		0x00,             /* type   = DATA */
		0x01,             /* flags  = END_STREAM */
		0x00, 0x00, 0x00, 0x01 /* stream_id = 1 */
	};

	frame_hdr_write_at(buf, 100, HIVE_FRAME_DATA, HIVE_FLAG_END_STREAM, 1);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

/*
 * SETTINGS ACK frame: stream_id=0, length=0, flags=0x01 (ACK).
 *
 * Expected wire bytes:
 *   [0x00, 0x00, 0x00]  — length 0
 *   [0x04]              — type SETTINGS (0x4)
 *   [0x01]              — flags ACK
 *   [0x00, 0x00, 0x00, 0x00] — stream_id 0 (connection-level)
 */
int
test_frame_hdr_write_settings(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x00, /* length = 0 */
		0x04,             /* type   = SETTINGS */
		0x01,             /* flags  = ACK */
		0x00, 0x00, 0x00, 0x00 /* stream_id = 0 */
	};

	frame_hdr_write_at(buf, 0, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

/*
 * HEADERS frame: stream_id=3, length=20 (0x000014), flags=END_HEADERS(0x04).
 *
 * Expected wire bytes:
 *   [0x00, 0x00, 0x14]  — length 20
 *   [0x01]              — type HEADERS (0x1)
 *   [0x04]              — flags END_HEADERS
 *   [0x00, 0x00, 0x00, 0x03] — stream_id 3
 */
int
test_frame_hdr_write_headers(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x14, /* length = 20 */
		0x01,             /* type   = HEADERS */
		0x04,             /* flags  = END_HEADERS */
		0x00, 0x00, 0x00, 0x03 /* stream_id = 3 */
	};

	frame_hdr_write_at(buf, 20, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS,
	    3);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

int
test_recv_settings_ack(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	s.pending_count = 1;
	frame_hdr_write_at(frame, 0, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_ping(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	uint8_t frame[17];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 8, HIVE_FRAME_PING, 0, 0, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_window_update(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[4] = {0, 0, 0, 1};
	uint8_t frame[13];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 4, HIVE_FRAME_WINDOW_UPDATE, 0, 0, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	n = build_frame(frame, 4, HIVE_FRAME_WINDOW_UPDATE, 0, 1, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_rst_stream(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[4] = {0, 0, 0, 0};
	uint8_t frame[13];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 4, HIVE_FRAME_RST_STREAM, 0, 1, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_data_full(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[5] = {'h', 'e', 'l', 'l', 'o'};
	uint8_t frame[14];
	data_capture_t cap;
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	memset(&cap, 0, sizeof(cap));
	cap.base = frame;
	s.callbacks.on_data_chunk = on_data_chunk_capture;
	s.user_data = &cap;
	n = build_frame(frame, 5, HIVE_FRAME_DATA, 0, 1, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 1);
	ASSERT(cap.last_len == 5);
	ASSERT(cap.last_ptr == frame + 9);
	return 1;
}

/*
 * Padded DATA frame: stream=1, 3 payload bytes + 2 padding bytes + 1
 * pad-length byte = 6 bytes total payload, PADDED flag set.
 *
 * Wire layout (9-byte header + payload):
 *   [0x02]              — pad_length = 2
 *   [0x61, 0x62, 0x63]  — data bytes "abc"
 *   [0x00, 0x00]        — 2 padding bytes
 *
 * on_data_chunk must fire with pointer to the first data byte (frame+10,
 * i.e. one byte past the pad_length field) and length 3.
 */
int
test_recv_data_padded(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	/* payload: pad_length=2, data "abc", 2 pad bytes */
	uint8_t payload[6] = {0x02, 'a', 'b', 'c', 0x00, 0x00};
	uint8_t frame[15];
	data_capture_t cap;
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	memset(&cap, 0, sizeof(cap));
	cap.base = frame;
	s.callbacks.on_data_chunk = on_data_chunk_capture;
	s.user_data = &cap;
	n = build_frame(frame, 6, HIVE_FRAME_DATA, HIVE_FLAG_PADDED, 1,
	    payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 1);
	ASSERT(cap.last_len == 3);
	/* Pointer must point past the 9-byte header AND the 1-byte
	 * pad_length field to the first real data byte. */
	ASSERT(cap.last_ptr == frame + 10);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_headers_end_headers(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[4] = {0x82, 0x86, 0x84, 0x41};
	uint8_t frame[13];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 4, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS, 1,
	    payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.reassembly_len == 4);
	ASSERT(memcmp(reassembly, payload, 4) == 0);
	ASSERT(s.reassembly_active == 0);
	return 1;
}

/*
 * HEADERS frame with PRIORITY flag (5-byte prefix) + END_HEADERS.
 *
 * Payload layout (9 bytes total):
 *   [0x00, 0x00, 0x00, 0x05, 0x00] — 5-byte PRIORITY prefix
 *                                     (exclusive=0, dep=5, weight=0)
 *   [0x82, 0x86, 0x84, 0x41]       — 4-byte HPACK-encoded header block
 *
 * The state machine must skip the 5-byte PRIORITY prefix via
 * priority_payload_len and copy only the 4 header-block bytes into
 * reassembly_buf.
 */
int
test_recv_headers_priority_prefix(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	/* 5-byte PRIORITY prefix + 4-byte header block = 9 bytes */
	uint8_t payload[9] = {
		0x00, 0x00, 0x00, 0x05, 0x00, /* PRIORITY: dep=5, weight=0 */
		0x82, 0x86, 0x84, 0x41        /* header block */
	};
	uint8_t frame[18];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 9, HIVE_FRAME_HEADERS,
	    HIVE_FLAG_PRIORITY | HIVE_FLAG_END_HEADERS, 1, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	/* Only the 4 header-block bytes (after the PRIORITY prefix) must
	 * land in reassembly_buf. */
	ASSERT(s.reassembly_len == 4);
	ASSERT(memcmp(reassembly, payload + 5, 4) == 0);
	ASSERT(s.reassembly_active == 0);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_priority_ignored(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[5] = {0, 0, 0, 1, 255};
	uint8_t frame[14];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 5, HIVE_FRAME_PRIORITY, 0, 1, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_unknown_type(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[3] = {1, 2, 3};
	uint8_t frame[12];
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 3, 0xff, 0, 0, payload);
	ASSERT(hive_session_recv(&s, frame, n) == (ssize_t)n);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_frame_header(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];
	size_t i;

	test_session_init(&s, reassembly, sizeof(reassembly));
	s.pending_count = 1;
	frame_hdr_write_at(frame, 0, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0);
	for (i = 0; i < sizeof(frame); i++) {
		ASSERT(hive_session_recv(&s, frame + i, 1) == 1);
	}
	ASSERT(s.frame_hdr_count == 0);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_data_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[4] = {'t', 'e', 's', 't'};
	uint8_t frame[13];
	data_capture_t cap;
	size_t n;
	size_t i;

	test_session_init(&s, reassembly, sizeof(reassembly));
	memset(&cap, 0, sizeof(cap));
	cap.base = frame;
	s.callbacks.on_data_chunk = on_data_chunk_capture;
	s.user_data = &cap;
	n = build_frame(frame, 4, HIVE_FRAME_DATA, 0, 1, payload);
	for (i = 0; i < n; i++) {
		ASSERT(hive_session_recv(&s, frame + i, 1) == 1);
	}
	ASSERT(cap.total == 4);
	ASSERT(cap.calls >= 1);
	return 1;
}

int
test_recv_split_settings_param(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t payload[6] = {
		0x00, 0x01, 0x00, 0x00, 0x20, 0x00
	};
	uint8_t frame[15];
	size_t n;
	size_t i;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 6, HIVE_FRAME_SETTINGS, 0, 0, payload);
	for (i = 0; i < n; i++) {
		ASSERT(hive_session_recv(&s, frame + i, 1) == 1);
	}
	ASSERT(s.ctrl_staging_count == 0);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_headers_plus_continuation(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t f1[11];
	uint8_t f2[11];
	const uint8_t p1[2] = {0xaa, 0xbb};
	const uint8_t p2[2] = {0xcc, 0xdd};
	size_t n1;
	size_t n2;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n1 = build_frame(f1, 2, HIVE_FRAME_HEADERS, 0, 1, p1);
	n2 = build_frame(f2, 2, HIVE_FRAME_CONTINUATION, HIVE_FLAG_END_HEADERS, 1,
	    p2);
	ASSERT(hive_session_recv(&s, f1, n1) == (ssize_t)n1);
	ASSERT(s.reassembly_active == 1);
	ASSERT(hive_session_recv(&s, f2, n2) == (ssize_t)n2);
	ASSERT(s.reassembly_active == 0);
	ASSERT(s.reassembly_len == 4);
	ASSERT(memcmp(reassembly, "\xaa\xbb\xcc\xdd", 4) == 0);
	return 1;
}

int
test_recv_continuation_lockout(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t headers[11];
	uint8_t dataf[10];
	const uint8_t p[2] = {0x01, 0x02};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(headers, 2, HIVE_FRAME_HEADERS, 0, 1, p);
	ASSERT(hive_session_recv(&s, headers, n) == (ssize_t)n);
	n = build_frame(dataf, 1, HIVE_FRAME_DATA, 0, 1, p);
	ASSERT(hive_session_recv(&s, dataf, n) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	return 1;
}

int
test_recv_continuation_flood_headers_is_connection_error(void)
{
	hive_session_t s;
	uint8_t reassembly[8];
	uint8_t headers[13];
	const uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
	size_t n;

	test_session_init(&s, reassembly, 3u);
	n = build_frame(headers, 4, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS, 1,
	    payload);
	ASSERT(hive_session_recv(&s, headers, n) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s.closed == 1);
	ASSERT(s.send_iov_count == 1);
	ASSERT(queued_frame_type(&s, 0) == HIVE_FRAME_GOAWAY);
	ASSERT(queued_frame_type(&s, 0) != HIVE_FRAME_RST_STREAM);
	return 1;
}

int
test_recv_continuation_flood_continuation_is_connection_error(void)
{
	hive_session_t s;
	uint8_t reassembly[8];
	uint8_t headers[11];
	uint8_t cont[11];
	const uint8_t p1[2] = {0xaa, 0xbb};
	const uint8_t p2[2] = {0xcc, 0xdd};
	size_t n;

	test_session_init(&s, reassembly, 3u);
	n = build_frame(headers, 2, HIVE_FRAME_HEADERS, 0, 1, p1);
	ASSERT(hive_session_recv(&s, headers, n) == (ssize_t)n);
	ASSERT(s.reassembly_len == 2);
	ASSERT(s.reassembly_active == 1);

	n = build_frame(cont, 2, HIVE_FRAME_CONTINUATION, HIVE_FLAG_END_HEADERS, 1,
	    p2);
	ASSERT(hive_session_recv(&s, cont, n) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s.closed == 1);
	ASSERT(s.send_iov_count == 1);
	ASSERT(queued_frame_type(&s, 0) == HIVE_FRAME_GOAWAY);
	ASSERT(queued_frame_type(&s, 0) != HIVE_FRAME_RST_STREAM);
	return 1;
}

int
test_recv_frame_too_large(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	s.local_settings.max_frame_size = 16;
	frame_hdr_write_at(frame, 17, HIVE_FRAME_DATA, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_data_on_stream_zero(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 0, HIVE_FRAME_DATA, 0, 0);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	return 1;
}

int
test_recv_settings_nonzero_stream(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 0, HIVE_FRAME_SETTINGS, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	return 1;
}

int
test_recv_ping_wrong_length(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 4, HIVE_FRAME_PING, 0, 0);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_rst_stream_wrong_length(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 5, HIVE_FRAME_RST_STREAM, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_settings_bad_length_nonzero_ack(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[15];
	uint8_t payload[6] = {0, 1, 0, 0, 0, 1};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 6, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0, payload);
	ASSERT(hive_session_recv(&s, frame, n) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_unknown_frame_mid_stream(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame1[11];
	uint8_t frame2[17];
	uint8_t p1[2] = {0xaa, 0xbb};
	uint8_t p2[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	size_t n1;
	size_t n2;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n1 = build_frame(frame1, 2, 0xff, 0, 0, p1);
	ASSERT(hive_session_recv(&s, frame1, n1) == (ssize_t)n1);
	n2 = build_frame(frame2, 8, HIVE_FRAME_PING, 0, 0, p2);
	ASSERT(hive_session_recv(&s, frame2, n2) == (ssize_t)n2);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	ASSERT(s.last_err == 0);
	return 1;
}

int
test_recv_split_ping_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[17];
	uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 8, HIVE_FRAME_PING, 0, 0, payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_window_update_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[13];
	uint8_t payload[4] = {0, 0, 0, 1};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 4, HIVE_FRAME_WINDOW_UPDATE, 0, 1, payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_rst_stream_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[13];
	uint8_t payload[4] = {0, 0, 0, 0};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 4, HIVE_FRAME_RST_STREAM, 0, 1, payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_priority_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[14];
	uint8_t payload[5] = {0, 0, 0, 1, 42};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 5, HIVE_FRAME_PRIORITY, 0, 1, payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_goaway_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[17];
	uint8_t payload[8] = {0, 0, 0, 1, 0, 0, 0, 0};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 8, HIVE_FRAME_GOAWAY, 0, 0, payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_push_promise_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[14];
	uint8_t payload[5] = {0, 0, 0, 2, 0xaa};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	s.role = HIVE_ROLE_CLIENT;
	n = build_frame(frame, 5, HIVE_FRAME_PUSH_PROMISE, HIVE_FLAG_END_HEADERS, 1,
	    payload);
	ASSERT(feed_one_by_one(&s, frame, n) == 1);
	ASSERT(s.reassembly_promised_stream_id == 2);
	ASSERT(s.recv_state == RECV_FRAME_HEADER);
	return 1;
}

int
test_recv_split_continuation_payload(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t h[10];
	uint8_t c[12];
	uint8_t hp[1] = {0xaa};
	uint8_t cp[3] = {0xbb, 0xcc, 0xdd};
	size_t nh;
	size_t nc;

	test_session_init(&s, reassembly, sizeof(reassembly));
	nh = build_frame(h, 1, HIVE_FRAME_HEADERS, 0, 1, hp);
	ASSERT(feed_one_by_one(&s, h, nh) == 1);
	ASSERT(s.reassembly_active == 1);
	nc = build_frame(c, 3, HIVE_FRAME_CONTINUATION, HIVE_FLAG_END_HEADERS, 1,
	    cp);
	ASSERT(feed_one_by_one(&s, c, nc) == 1);
	ASSERT(s.reassembly_active == 0);
	ASSERT(s.reassembly_len == 4);
	return 1;
}

int
test_recv_settings_bad_length_non_ack(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[14];
	uint8_t payload[5] = {0, 1, 0, 0, 0};
	size_t n;

	test_session_init(&s, reassembly, sizeof(reassembly));
	n = build_frame(frame, 5, HIVE_FRAME_SETTINGS, 0, 0, payload);
	ASSERT(hive_session_recv(&s, frame, n) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_window_update_wrong_length(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 5, HIVE_FRAME_WINDOW_UPDATE, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_priority_wrong_length(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 4, HIVE_FRAME_PRIORITY, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	ASSERT(s.send_iov_count == 1);
	ASSERT(queued_frame_type(&s, 0) == HIVE_FRAME_RST_STREAM);
	return 1;
}

int
test_recv_goaway_too_short(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 7, HIVE_FRAME_GOAWAY, 0, 0);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_push_promise_wrong_length_unpadded(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 3, HIVE_FRAME_PUSH_PROMISE, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

int
test_recv_push_promise_wrong_length_padded(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	frame_hdr_write_at(frame, 4, HIVE_FRAME_PUSH_PROMISE, HIVE_FLAG_PADDED, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
	ASSERT(s.last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

