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
int test_recv_headers_end_headers(void);
int test_recv_priority_ignored(void);
int test_recv_unknown_type(void);
int test_recv_split_frame_header(void);
int test_recv_split_data_payload(void);
int test_recv_split_settings_param(void);
int test_recv_headers_plus_continuation(void);
int test_recv_continuation_lockout(void);
int test_recv_frame_too_large(void);
int test_recv_data_on_stream_zero(void);
int test_recv_settings_nonzero_stream(void);
int test_recv_ping_wrong_length(void);
int test_recv_rst_stream_wrong_length(void);

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
	frame_recv_init(s, HIVE_ROLE_SERVER);
	s->reassembly_buf = reassembly_buf;
	s->opt_max_continuation_size = (uint32_t)cap;
	s->local_settings.max_frame_size = 16384;
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
test_recv_frame_too_large(void)
{
	hive_session_t s;
	uint8_t reassembly[1024];
	uint8_t frame[9];

	test_session_init(&s, reassembly, sizeof(reassembly));
	s.local_settings.max_frame_size = 16;
	frame_hdr_write_at(frame, 17, HIVE_FRAME_DATA, 0, 1);
	ASSERT(hive_session_recv(&s, frame, sizeof(frame)) == -1);
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
	ASSERT(s.last_h2_err == HIVE_H2_FRAME_SIZE_ERROR);
	return 1;
}

