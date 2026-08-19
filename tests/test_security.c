/*
 * test_security.c -- Phase 7 security tests
 *
 * SETTINGS flood protection (ARCHITECTURE.md §8.4)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>

#include "../include/hive.h"
#include "../src/hive_frame.h"
#include "../src/hive_frame_bare.h"
#include "../src/hive_internal.h"
#include "test_harness.h"

int test_settings_flood_uses_inbound_counter(void);
int test_settings_unsolicited_ack(void);
int test_continuation_flood_is_connection_error(void);
int test_rst_stream_flood_callback(void);
int test_rst_stream_flood_window_reset(void);
int test_stream_id_exhaustion_triggers_prepare(void);
int test_http_messaging_pseudo_after_regular(void);
int test_http_messaging_unknown_pseudo_header(void);
int test_http_messaging_duplicate_pseudo_header(void);
int test_http_messaging_pseudo_header_in_trailers(void);
int test_http_messaging_trailers_require_end_stream(void);
int test_http_messaging_uppercase_field_name(void);
int test_http_messaging_forbidden_connection_header(void);
int test_http_messaging_te_invalid_value(void);
int test_http_messaging_content_length_mismatch(void);
int test_hpack_negative_index_zero(void);
int test_hive_buf_asan_poisoning(void);

#if defined(HIVE_TEST_CLOCK) && HIVE_TEST_CLOCK == 1
uint64_t hive_test_clock_secs;

typedef struct {
	uint32_t calls;
	uint32_t last_rate;
} rst_flood_capture_t;

static size_t
build_rst_stream_frame(uint8_t *dst, uint32_t stream_id, uint32_t error_code)
{
	frame_hdr_write_at(dst, 4u, HIVE_FRAME_RST_STREAM, 0u, stream_id);
	dst[9] = (uint8_t)((error_code >> 24) & 0xffu);
	dst[10] = (uint8_t)((error_code >> 16) & 0xffu);
	dst[11] = (uint8_t)((error_code >> 8) & 0xffu);
	dst[12] = (uint8_t)(error_code & 0xffu);
	return 13u;
}

static size_t build_headers_frame_block(uint8_t *, uint32_t, uint8_t,
	const uint8_t *, size_t);

static int
on_rst_stream_flood_capture(hive_session_t *session,
	uint32_t rate,
	void *user_data)
{
	rst_flood_capture_t *cap;

	(void)session;

	cap = (rst_flood_capture_t *)user_data;
	if (cap == NULL)
		return 0;
	cap->calls++;
	cap->last_rate = rate;
	return 0;
}

static int
recv_opened_rst_stream(hive_session_t *session, uint32_t stream_id)
{
	uint8_t headers[12];
	uint8_t rst[13];
	uint8_t request[] = {0x82u, 0x84u, 0x86u};
	size_t n;

	n = build_headers_frame_block(headers,
	                              stream_id,
	                              HIVE_FLAG_END_HEADERS,
	                              request,
	                              sizeof(request));
	if (hive_session_recv(session, headers, n) != (ssize_t)n)
		return 0;
	n = build_rst_stream_frame(rst, stream_id, HIVE_H2_CANCEL);
	return hive_session_recv(session, rst, n) == (ssize_t)n;
}
#endif

static ssize_t
send_cb_full(hive_session_t *session,
    const struct iovec *iov,
    int iovcnt,
    void *user_data)
{
	ssize_t total;
	int i;

	(void)session;
	(void)user_data;

	total = 0;
	for (i = 0; i < iovcnt; i++)
		total += (ssize_t)iov[i].iov_len;

	return total;
}

static int
on_header_noop(hive_session_t *session,
	uint32_t stream_id,
	hive_buf_t *name,
	hive_buf_t *value,
	uint8_t flags,
	void *user_data)
{
	(void)session;
	(void)stream_id;
	(void)name;
	(void)value;
	(void)flags;
	(void)user_data;
	return HIVE_OK;
}

static size_t
build_settings_frame(uint8_t *dst, uint8_t flags)
{
	frame_hdr_write_at(dst, 0u, HIVE_FRAME_SETTINGS, flags, 0u);
	return 9u;
}

static size_t
build_headers_frame(uint8_t *dst, uint32_t stream_id, uint8_t flags)
{
	frame_hdr_write_at(dst, 0u, HIVE_FRAME_HEADERS, flags, stream_id);
	return 9u;
}

static size_t
build_headers_frame_block(uint8_t *dst,
	    uint32_t stream_id,
	    uint8_t flags,
	    const uint8_t *payload,
	    size_t payload_len)
{
	frame_hdr_write_at(
	    dst, (uint32_t)payload_len, HIVE_FRAME_HEADERS, flags, stream_id);
	if (payload_len > 0)
		memcpy(dst + 9, payload, payload_len);
	return 9u + payload_len;
}

static size_t
build_continuation_frame(uint8_t *dst,
	    uint32_t stream_id,
	    uint8_t flags,
	    const uint8_t *payload,
	    size_t payload_len)
{
	frame_hdr_write_at(
	    dst, (uint32_t)payload_len, HIVE_FRAME_CONTINUATION, flags, stream_id);
	if (payload_len > 0)
		memcpy(dst + 9, payload, payload_len);
	return 9u + payload_len;
}

static uint32_t
u32be_at(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static size_t
build_data_frame(uint8_t *dst,
	    uint32_t stream_id,
	    uint8_t flags,
	    const uint8_t *payload,
	    size_t payload_len)
{
	frame_hdr_write_at(
	    dst, (uint32_t)payload_len, HIVE_FRAME_DATA, flags, stream_id);
	if (payload_len > 0)
		memcpy(dst + 9, payload, payload_len);
	return 9u + payload_len;
}

static size_t
build_headers_frame_hpack(hive_session_t *s,
	    uint8_t *dst,
	    size_t dst_cap,
	    uint32_t stream_id,
	    uint8_t flags,
	    const hive_nv_t *nva,
	    size_t nvlen)
{
	uint8_t block[1024];
	size_t block_len;

	block_len = sizeof(block);
	ASSERT(hpack_encode_block(&s->enc_table,
	                          &s->mem,
	                          nva,
	                          nvlen,
	                          block,
	                          sizeof(block),
	                          &block_len) == HIVE_OK);
	ASSERT(dst_cap >= 9u + block_len);

	frame_hdr_write_at(dst,
	                  (uint32_t)block_len,
	                  HIVE_FRAME_HEADERS,
	                  (uint8_t)(flags | HIVE_FLAG_END_HEADERS),
	                  stream_id);
	if (block_len > 0)
		memcpy(dst + 9, block, block_len);
	return 9u + block_len;
}

static int
assert_rst_protocol(hive_session_t *s, uint32_t stream_id)
{
	frame_hdr_t hdr;

	ASSERT(s->closed == 0u);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_buf, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_RST_STREAM);
	ASSERT(hdr.stream_id == stream_id);
	ASSERT(hdr.length == 4u);
	ASSERT(u32be_at(s->send_buf + 9) == HIVE_H2_PROTOCOL_ERROR);
	return 1;
}

static int
assert_goaway(hive_session_t *s, uint32_t h2_err)
{
	frame_hdr_t hdr;

	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_buf, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_GOAWAY);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(hdr.length >= 8u);
	ASSERT(u32be_at(s->send_buf + 13) == h2_err);
	return 1;
}

static hive_session_t *
new_server_security_session(uint32_t max_settings_pending,
	uint32_t rst_threshold,
	uint32_t rst_window_secs,
	int (*on_rst_stream_flood)(hive_session_t *, uint32_t, void *),
	void *user_data)
{
	hive_callbacks_t cb;
	hive_options_t *opt;
	hive_session_t *s;

	opt = hive_options_new();
	ASSERT(opt != NULL);
	ASSERT(hive_options_set_max_settings_pending(opt, max_settings_pending) ==
	    HIVE_OK);
	ASSERT(hive_options_set_rst_stream_flood_threshold(opt, rst_threshold) ==
	    HIVE_OK);
	ASSERT(hive_options_set_rst_stream_flood_window_secs(opt, rst_window_secs) ==
	    HIVE_OK);

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	cb.on_header = on_header_noop;
	cb.on_rst_stream_flood = on_rst_stream_flood;

	s = hive_session_server_new(NULL, opt, &cb, user_data);
	hive_options_free(opt);
	ASSERT(s != NULL);

	/* Ignore queued server preface in receive-path tests. */
	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;
	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	return s;
}

int
test_settings_flood_uses_inbound_counter(void)
{
	hive_session_t *s;
	uint8_t frame[9];
	size_t n;

	s = new_server_security_session(1u, 100u, 10u, NULL, NULL);

	n = build_settings_frame(frame, 0u);
	/* Leave the first ACK queued, then exceed the configured limit on wire. */
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(s->inbound_settings_count == 1u);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_settings_unsolicited_ack(void)
{
	hive_session_t *s;
	uint8_t frame[9];
	size_t n;

	s = new_server_security_session(1u, 100u, 10u, NULL, NULL);
	s->pending_head = 0u;
	s->pending_tail = 0u;
	s->pending_count = 0u;

	n = build_settings_frame(frame, HIVE_FLAG_ACK);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_continuation_flood_is_connection_error(void)
{
	hive_session_t *s;
	uint8_t headers[13];
	uint8_t cont[13];
	uint8_t payload1[4] = {0x82u, 0x84u, 0x86u, 0x41u};
	uint8_t payload2[4] = {0x8cu, 0xf1u, 0xe3u, 0xc2u};
	size_t n;

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	s->opt_max_continuation_size = 6u;

	n = build_headers_frame_block(headers, 1u, 0u, payload1, sizeof(payload1));
	ASSERT(hive_session_recv(s, headers, n) == (ssize_t)n);
	ASSERT(s->reassembly_active == 1u);

	n = build_continuation_frame(
	    cont, 1u, HIVE_FLAG_END_HEADERS, payload2, sizeof(payload2));
	ASSERT(hive_session_recv(s, cont, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s->closed == 1u);
	ASSERT(assert_goaway(s, HIVE_H2_PROTOCOL_ERROR));

	hive_session_free(s);
	return 1;
}

int
test_rst_stream_flood_callback(void)
{
#if defined(HIVE_TEST_CLOCK) && HIVE_TEST_CLOCK == 1
	hive_session_t *s;
	rst_flood_capture_t cap;

	memset(&cap, 0, sizeof(cap));
	hive_test_clock_secs = 100u;

	s = new_server_security_session(3u,
	                                3u,
	                                10u,
	                                on_rst_stream_flood_capture,
	                                &cap);

	ASSERT(recv_opened_rst_stream(s, 1u));
	ASSERT(recv_opened_rst_stream(s, 3u));
	ASSERT(recv_opened_rst_stream(s, 5u));
	ASSERT(cap.calls == 0u);

	ASSERT(recv_opened_rst_stream(s, 7u));
	ASSERT(cap.calls == 1u);
	ASSERT(cap.last_rate == 4u);

	hive_session_free(s);
	return 1;
#else
	return 1;
#endif
}

int
test_rst_stream_flood_window_reset(void)
{
#if defined(HIVE_TEST_CLOCK) && HIVE_TEST_CLOCK == 1
	hive_session_t *s;
	rst_flood_capture_t cap;

	memset(&cap, 0, sizeof(cap));
	hive_test_clock_secs = 200u;

	s = new_server_security_session(3u,
	                                3u,
	                                10u,
	                                on_rst_stream_flood_capture,
	                                &cap);

	ASSERT(recv_opened_rst_stream(s, 1u));
	ASSERT(recv_opened_rst_stream(s, 3u));
	ASSERT(recv_opened_rst_stream(s, 5u));
	ASSERT(cap.calls == 0u);

	hive_test_clock_secs = 211u;
	ASSERT(recv_opened_rst_stream(s, 7u));
	ASSERT(recv_opened_rst_stream(s, 9u));
	ASSERT(recv_opened_rst_stream(s, 11u));
	ASSERT(cap.calls == 0u);

	hive_session_free(s);
	return 1;
#else
	return 1;
#endif
}

int
test_stream_id_exhaustion_triggers_prepare(void)
{
	hive_session_t *s;
	uint8_t frame[9];
	size_t n;
	uint32_t stream_id;
	frame_hdr_t hdr;

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);

	stream_id = 0x7ffffc19u; /* 2^31 - 999, odd peer stream for server role */
	n = build_headers_frame(frame, stream_id, HIVE_FLAG_END_HEADERS);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(stream_lookup(s, stream_id) != NULL);
	ASSERT(s->goaway_prepare_sent == 1u);
	ASSERT(s->goaway_sent == 1u);
	ASSERT(s->session_state == HIVE_SESSION_OPEN);
	ASSERT(s->goaway_last_stream_id_sent == 0x7fffffffu);
	ASSERT(s->send_iov_count == 1u);

	frame_hdr_parse(s->send_buf, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_GOAWAY);
	ASSERT(hdr.length == 8u);
	ASSERT(hdr.flags == 0u);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(u32be_at(s->send_buf + 9) == 0x7fffffffu);
	ASSERT(u32be_at(s->send_buf + 13) == HIVE_H2_NO_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_pseudo_after_regular(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)"content-type", (const uint8_t *)"text/plain",
		    12u, 10u, 0u},
		{(const uint8_t *)":method", (const uint8_t *)"GET", 7u, 3u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 2u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_unknown_pseudo_header(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)":bogus", (const uint8_t *)"x", 6u, 1u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 1u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_duplicate_pseudo_header(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)":method", (const uint8_t *)"GET", 7u, 3u, 0u},
		{(const uint8_t *)":method", (const uint8_t *)"POST", 7u, 4u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 2u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_pseudo_header_in_trailers(void)
{
	hive_session_t *s;
	uint8_t frame1[256];
	uint8_t frame2[256];
	size_t n1;
	size_t n2;
	static const hive_nv_t headers1[] = {
		{(const uint8_t *)"content-type", (const uint8_t *)"text/plain",
		    12u, 10u, 0u},
	};
	static const hive_nv_t headers2[] = {
		{(const uint8_t *)":method", (const uint8_t *)"GET", 7u, 3u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n1 = build_headers_frame_hpack(
	    s, frame1, sizeof(frame1), 1u, 0u, headers1, 1u);
	ASSERT(hive_session_recv(s, frame1, n1) == (ssize_t)n1);
	ASSERT(s->send_iov_count == 0);

	n2 = build_headers_frame_hpack(
	    s, frame2, sizeof(frame2), 1u, HIVE_FLAG_END_STREAM, headers2, 1u);
	ASSERT(hive_session_recv(s, frame2, n2) == (ssize_t)n2);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_trailers_require_end_stream(void)
{
	hive_session_t *s;
	uint8_t frame1[256];
	uint8_t frame2[256];
	size_t n1;
	size_t n2;
	static const hive_nv_t headers1[] = {
		{(const uint8_t *)"content-type", (const uint8_t *)"text/plain",
		    12u, 10u, 0u},
	};
	static const hive_nv_t headers2[] = {
		{(const uint8_t *)"x-extra", (const uint8_t *)"1", 7u, 1u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n1 = build_headers_frame_hpack(
	    s, frame1, sizeof(frame1), 1u, 0u, headers1, 1u);
	ASSERT(hive_session_recv(s, frame1, n1) == (ssize_t)n1);
	ASSERT(s->send_iov_count == 0);

	/* Trailing HEADERS must terminate the stream (END_STREAM required). */
	n2 = build_headers_frame_hpack(
	    s, frame2, sizeof(frame2), 1u, 0u, headers2, 1u);
	ASSERT(hive_session_recv(s, frame2, n2) == (ssize_t)n2);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_uppercase_field_name(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)"Content-Type", (const uint8_t *)"text/plain",
		    12u, 10u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 1u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_forbidden_connection_header(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)"connection", (const uint8_t *)"keep-alive",
		    10u, 10u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 1u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_te_invalid_value(void)
{
	hive_session_t *s;
	uint8_t frame[256];
	size_t n;
	static const hive_nv_t headers[] = {
		{(const uint8_t *)"te", (const uint8_t *)"gzip", 2u, 4u, 0u},
	};

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);
	n = build_headers_frame_hpack(
	    s, frame, sizeof(frame), 1u, 0u, headers, 1u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_http_messaging_content_length_mismatch(void)
{
	hive_session_t *s;
	uint8_t headers[256];
	uint8_t data_frame[80];
	uint8_t body[50];
	size_t n;
	static const hive_nv_t req_headers[] = {
		{(const uint8_t *)"content-length", (const uint8_t *)"100", 14u,
		    3u, 0u},
	};

	memset(body, 'x', sizeof(body));
	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);

	n = build_headers_frame_hpack(
	    s, headers, sizeof(headers), 1u, 0u, req_headers, 1u);
	ASSERT(hive_session_recv(s, headers, n) == (ssize_t)n);
	ASSERT(s->send_iov_count == 0);

	n = build_data_frame(
	    data_frame, 1u, HIVE_FLAG_END_STREAM, body, sizeof(body));
	ASSERT(hive_session_recv(s, data_frame, n) == (ssize_t)n);
	ASSERT(assert_rst_protocol(s, 1u));

	hive_session_free(s);
	return 1;
}

int
test_hpack_negative_index_zero(void)
{
	hive_session_t *s;
	uint8_t frame[10];

	s = new_server_security_session(3u, 100u, 10u, NULL, NULL);

	frame_hdr_write_at(frame, 1u, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS, 1u);
	frame[9] = 0x80u; /* indexed representation with index=0 (invalid). */

	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == -1);
	ASSERT(s->last_err == HIVE_ERR_COMPRESSION);
	ASSERT(s->last_h2_err == HIVE_H2_COMPRESSION_ERROR);
	ASSERT(s->closed == 1u);
	ASSERT(assert_goaway(s, HIVE_H2_COMPRESSION_ERROR));

	hive_session_free(s);
	return 1;
}

int
test_hive_buf_asan_poisoning(void)
{
	/*
	 * Manual verification only (expected ASan abort in HIVE_DEBUG builds):
	 * 1) Capture name->data/value->data in on_header without hive_buf_retain().
	 * 2) Access captured pointer after callback returns.
	 * 3) Expect heap-use-after-poison for reassembly/scratch-backed buffers.
	 *
	 * This case is intentionally not executed in automated RUN() flow because
	 * success condition is process termination under ASan.
	 */
	return 1;
}
