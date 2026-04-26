/*
 * test_security.c -- Phase 7 security tests
 *
 * Task 7.2 -- SETTINGS flood protection (§8.4)
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
int test_rst_stream_flood_callback(void);
int test_rst_stream_flood_window_reset(void);

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

static size_t
build_settings_frame(uint8_t *dst, uint8_t flags)
{
	frame_hdr_write_at(dst, 0u, HIVE_FRAME_SETTINGS, flags, 0u);
	return 9u;
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

	/*
	 * SECURITY: Flood tracking must use inbound_settings_count, not the
	 * outbound pending_settings ring. Seed inbound counter at the limit, then
	 * deliver one more non-ACK SETTINGS frame.
	 */
	s->inbound_settings_count = 1u;
	s->pending_count = 0u;

	n = build_settings_frame(frame, 0u);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s->pending_count == 0u);

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

	n = build_settings_frame(frame, HIVE_FLAG_ACK);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_rst_stream_flood_callback(void)
{
#if defined(HIVE_TEST_CLOCK) && HIVE_TEST_CLOCK == 1
	hive_session_t *s;
	rst_flood_capture_t cap;
	uint8_t frame[13];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	hive_test_clock_secs = 100u;

	s = new_server_security_session(3u,
	                                3u,
	                                10u,
	                                on_rst_stream_flood_capture,
	                                &cap);

	n = build_rst_stream_frame(frame, 1u, HIVE_H2_CANCEL);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 0u);

	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
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
	uint8_t frame[13];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	hive_test_clock_secs = 200u;

	s = new_server_security_session(3u,
	                                3u,
	                                10u,
	                                on_rst_stream_flood_capture,
	                                &cap);

	n = build_rst_stream_frame(frame, 1u, HIVE_H2_CANCEL);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 0u);

	hive_test_clock_secs = 211u;
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 0u);

	hive_session_free(s);
	return 1;
#else
	return 1;
#endif
}

