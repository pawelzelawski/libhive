/*
 * test_session.c — Phase 4 session tests
 *
 * Task 4.0: minimal send queue verification.
 *   test_send_control_frame_queued — queue a control frame; want_write == 1;
 *     full send clears the queue.
 *   test_send_partial_write — partial send retains unsent tail; second send
 *     drains completely.
 *   test_send_fatal_error — callback returns -1; session marked CLOSED.
 *
 * Task 4.2: options API.
 *   test_options_defaults — hive_options_new() applies all defaults from
 *     ARCHITECTURE.md §9.5.
 *   test_options_set_valid — each setter accepts its boundary values.
 *   test_options_set_invalid — each setter rejects out-of-range values.
 *
 * NOTE: test_options_set_max_concurrent (verifying stream_slots size after
 * session creation) is deferred to Task 4.3 because it requires a fully
 * working hive_session_server_new().
 *
 * The session struct for send-queue tests is initialised manually on the
 * stack, following the same pattern as test_frame.c.  Full session lifecycle
 * (create / free) is tested in tasks 4.1–4.3.
 *
 * See DEVELOPMENT.md — Phase 4, Tasks 4.0 and 4.2.
 * See ARCHITECTURE.md §6.1, §6.3, §6.6, §9.5.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>

#include "../include/hive.h"
#include "../src/hive_frame.h"
#include "../src/hive_internal.h"
#include "../src/hive_send.h"
#include "test_harness.h"

int test_send_control_frame_queued(void);
int test_send_partial_write(void);
int test_send_fatal_error(void);
int test_options_defaults(void);
int test_options_set_valid(void);
int test_options_set_invalid(void);

/* ------------------------------------------------------------------ */
/* Shared test infrastructure                                          */
/* ------------------------------------------------------------------ */

#define TEST_SEND_BUF_CAP 4096u
#define TEST_SEND_IOV_CAP 32u

/*
 * send callback that sums the effective iov and returns the total —
 * simulating a full successful write.
 */
static ssize_t
send_cb_full(hive_session_t *session,
             const struct iovec *iov,
             int iovcnt,
             void *user_data)
{
	ssize_t total = 0;
	int i;

	(void)session;
	(void)user_data;

	for (i = 0; i < iovcnt; i++)
		total += (ssize_t)iov[i].iov_len;

	return total;
}

/*
 * State for a controlled partial-then-full send.
 * call_count tracks invocations; first call returns partial_bytes,
 * subsequent calls return the full remaining amount.
 */
typedef struct {
	int    call_count;
	ssize_t partial_bytes; /* bytes to return on first call */
} partial_send_state_t;

static ssize_t
send_cb_partial(hive_session_t *session,
                const struct iovec *iov,
                int iovcnt,
                void *user_data)
{
	partial_send_state_t *st = user_data;
	ssize_t total = 0;
	int i;

	(void)session;

	for (i = 0; i < iovcnt; i++)
		total += (ssize_t)iov[i].iov_len;

	st->call_count++;

	if (st->call_count == 1)
		return st->partial_bytes; /* simulated partial write */

	return total; /* full write on subsequent calls */
}

/* Send callback that always returns -1 (fatal I/O error). */
static ssize_t
send_cb_fatal(hive_session_t *session,
              const struct iovec *iov,
              int iovcnt,
              void *user_data)
{
	(void)session;
	(void)iov;
	(void)iovcnt;
	(void)user_data;
	return -1;
}

/*
 * Initialise a minimal hive_session_t for send queue tests.
 * Wires up the provided static buffers and zeroes all send state fields.
 */
static void
test_send_session_init(hive_session_t *s,
                       uint8_t *send_buf,
                       size_t send_buf_cap,
                       struct iovec *send_iov)
{
	memset(s, 0, sizeof(*s));
	s->send_buf             = send_buf;
	s->send_buf_cap         = send_buf_cap;
	s->send_iov             = send_iov;
	s->send_iov_count       = 0;
	s->send_buf_used        = 0;
	s->send_partial         = 0;
	s->send_partial_offset  = 0;
	s->session_state        = HIVE_SESSION_OPEN;
}

/* ------------------------------------------------------------------ */
/* test_send_control_frame_queued                                      */
/* ------------------------------------------------------------------ */

/*
 * After appending a SETTINGS ACK control frame (9 bytes, no payload),
 * want_write() must return 1.  After hive_session_send() with a full-write
 * callback, want_write() must return 0 and the send queue must be empty.
 */
int
test_send_control_frame_queued(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	hive_session_t s;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);

	/* SETTINGS ACK: type=SETTINGS (4), flags=ACK (1), stream=0, no payload */
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);

	/* One frame queued — want_write must be true */
	ASSERT(hive_session_want_write(&s) == 1);
	ASSERT(s.send_iov_count == 1);
	ASSERT(s.send_buf_used == 9u); /* header only */

	/* Wire up a full-write send callback */
	s.callbacks.send = send_cb_full;
	s.user_data      = NULL;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);

	/* Queue drained — want_write must be false */
	ASSERT(hive_session_want_write(&s) == 0);
	ASSERT(s.send_iov_count == 0);
	ASSERT(s.send_buf_used == 0u);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_partial_offset == 0u);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_send_partial_write                                             */
/* ------------------------------------------------------------------ */

/*
 * Queue a PING frame (9-byte header + 8-byte opaque payload = 17 bytes total).
 * First hive_session_send() call: callback returns 5 bytes.
 *   → want_write() still 1; send_partial == 1; send_partial_offset == 5.
 * Second hive_session_send() call: callback returns the remaining 12 bytes.
 *   → want_write() == 0; send queue fully reset.
 */
int
test_send_partial_write(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	static const uint8_t ping_payload[8] = {
	    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
	};
	partial_send_state_t st;
	hive_session_t s;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);

	/* PING: type=6, no special flags, stream_id=0, 8-byte payload */
	send_queue_append_ctrl(&s, HIVE_FRAME_PING, 0u, 0u,
	    ping_payload, 8u);

	/* 9-byte header + 8-byte payload = 17 bytes, 1 iov entry */
	ASSERT(s.send_iov_count == 1);
	ASSERT(s.send_buf_used == 17u);
	ASSERT(hive_session_want_write(&s) == 1);

	/* First send: partial — return only 5 of 17 bytes */
	st.call_count    = 0;
	st.partial_bytes = 5;
	s.callbacks.send = send_cb_partial;
	s.user_data      = &st;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 1);

	/* Still more to send */
	ASSERT(hive_session_want_write(&s) == 1);
	ASSERT(s.send_partial == 1);
	ASSERT(s.send_partial_offset == 5u);
	ASSERT(s.send_iov_count == 1); /* queue not reset yet */

	/* Second send: effective iov covers remaining 12 bytes; callback
	 * returns the full amount on calls > 1. */
	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);

	/* Queue fully drained */
	ASSERT(hive_session_want_write(&s) == 0);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_partial_offset == 0u);
	ASSERT(s.send_iov_count == 0);
	ASSERT(s.send_buf_used == 0u);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_send_fatal_error                                               */
/* ------------------------------------------------------------------ */

/*
 * Queue a SETTINGS ACK.  The send callback returns -1 (fatal I/O error).
 * hive_session_send() must return HIVE_ERR_PROTOCOL and mark the session
 * as HIVE_SESSION_CLOSED.
 */
int
test_send_fatal_error(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	hive_session_t s;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);

	/* Queue a SETTINGS ACK (9 bytes, no payload) */
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);

	ASSERT(hive_session_want_write(&s) == 1);

	s.callbacks.send = send_cb_fatal;
	s.user_data      = NULL;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_ERR_PROTOCOL);
	ASSERT(s.session_state == (uint8_t)HIVE_SESSION_CLOSED);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_options_defaults                                               */
/* ------------------------------------------------------------------ */

/*
 * hive_options_new() must apply every default listed in ARCHITECTURE.md §9.5.
 * Verified by reading each field directly from the returned struct.
 * hive_options_free() must not crash and must release the allocation.
 */
int
test_options_defaults(void)
{
	hive_options_t *opt;

	opt = hive_options_new();
	ASSERT(opt != NULL);

	ASSERT(opt->opt_header_table_size      == 4096u);
	ASSERT(opt->opt_enable_push            == 1u);
	ASSERT(opt->opt_max_concurrent_streams == 100u);
	ASSERT(opt->opt_initial_window_size    == 65535u);
	ASSERT(opt->opt_max_frame_size         == 16384u);
	ASSERT(opt->opt_max_header_list_size   == 65536u);
	ASSERT(opt->opt_max_header_count       == 100u);
	ASSERT(opt->opt_max_continuation_size  == 65536u);
	ASSERT(opt->opt_max_settings_pending   == 3u);
	ASSERT(opt->opt_rst_flood_threshold    == 100u);
	ASSERT(opt->opt_rst_flood_window_secs  == 10u);
	ASSERT(opt->opt_max_send_iov           == 512u);
	ASSERT(opt->opt_max_header_string_size == 8192u);
	ASSERT(opt->opt_no_http_messaging      == 0u);
	ASSERT(opt->opt_no_auto_ping_ack       == 0u);

	hive_options_free(opt);
	return 1;
}

/* ------------------------------------------------------------------ */
/* test_options_set_valid                                              */
/* ------------------------------------------------------------------ */

/*
 * Each setter must accept valid boundary values and update the field.
 */
int
test_options_set_valid(void)
{
	hive_options_t *opt;
	int ret;

	opt = hive_options_new();
	ASSERT(opt != NULL);

	/* header_table_size: 0–65536 */
	ret = hive_options_set_header_table_size(opt, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_header_table_size == 0u);
	ret = hive_options_set_header_table_size(opt, 65536u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_header_table_size == 65536u);

	/* enable_push: 0–1 */
	ret = hive_options_set_enable_push(opt, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_enable_push == 0u);
	ret = hive_options_set_enable_push(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_enable_push == 1u);

	/* max_concurrent_streams: 1–65535 */
	ret = hive_options_set_max_concurrent_streams(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_concurrent_streams == 1u);
	ret = hive_options_set_max_concurrent_streams(opt, 65535u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_concurrent_streams == 65535u);

	/* initial_window_size: 1–2147483647 */
	ret = hive_options_set_initial_window_size(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_initial_window_size == 1u);
	ret = hive_options_set_initial_window_size(opt, 2147483647u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_initial_window_size == 2147483647u);

	/* max_frame_size: 16384–16777215 */
	ret = hive_options_set_max_frame_size(opt, 16384u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_frame_size == 16384u);
	ret = hive_options_set_max_frame_size(opt, 16777215u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_frame_size == 16777215u);

	/* max_header_list_size: 1–16777215 */
	ret = hive_options_set_max_header_list_size(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_list_size == 1u);
	ret = hive_options_set_max_header_list_size(opt, 16777215u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_list_size == 16777215u);

	/* max_header_count: 1–65535 */
	ret = hive_options_set_max_header_count(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_count == 1u);
	ret = hive_options_set_max_header_count(opt, 65535u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_count == 65535u);

	/* max_continuation_size: 16384–16777215 */
	ret = hive_options_set_max_continuation_size(opt, 16384u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_continuation_size == 16384u);
	ret = hive_options_set_max_continuation_size(opt, 16777215u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_continuation_size == 16777215u);

	/* max_settings_pending: 1–255 */
	ret = hive_options_set_max_settings_pending(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_settings_pending == 1u);
	ret = hive_options_set_max_settings_pending(opt, 255u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_settings_pending == 255u);

	/* rst_stream_flood_threshold: 1–65535 */
	ret = hive_options_set_rst_stream_flood_threshold(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_rst_flood_threshold == 1u);
	ret = hive_options_set_rst_stream_flood_threshold(opt, 65535u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_rst_flood_threshold == 65535u);

	/* rst_stream_flood_window_secs: 1–3600 */
	ret = hive_options_set_rst_stream_flood_window_secs(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_rst_flood_window_secs == 1u);
	ret = hive_options_set_rst_stream_flood_window_secs(opt, 3600u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_rst_flood_window_secs == 3600u);

	/* max_send_iov: 64–HIVE_SEND_IOV_MAX (1024) */
	ret = hive_options_set_max_send_iov(opt, 64u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_send_iov == 64u);
	ret = hive_options_set_max_send_iov(opt, HIVE_SEND_IOV_MAX);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_send_iov == HIVE_SEND_IOV_MAX);

	/* max_header_string_size: 256–65536 */
	ret = hive_options_set_max_header_string_size(opt, 256u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_string_size == 256u);
	ret = hive_options_set_max_header_string_size(opt, 65536u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_max_header_string_size == 65536u);

	/* no_http_messaging: 0–1 */
	ret = hive_options_set_no_http_messaging(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_no_http_messaging == 1u);
	ret = hive_options_set_no_http_messaging(opt, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_no_http_messaging == 0u);

	/* no_auto_ping_ack: 0–1 */
	ret = hive_options_set_no_auto_ping_ack(opt, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_no_auto_ping_ack == 1u);
	ret = hive_options_set_no_auto_ping_ack(opt, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(opt->opt_no_auto_ping_ack == 0u);

	hive_options_free(opt);
	return 1;
}

/* ------------------------------------------------------------------ */
/* test_options_set_invalid                                            */
/* ------------------------------------------------------------------ */

/*
 * Each setter must return HIVE_ERR_INVALID_ARG for out-of-range values
 * and must NOT modify the option field on failure.
 */
int
test_options_set_invalid(void)
{
	hive_options_t *opt;
	int ret;

	opt = hive_options_new();
	ASSERT(opt != NULL);

	/* header_table_size: > 65536 rejected */
	ret = hive_options_set_header_table_size(opt, 65537u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);
	ASSERT(opt->opt_header_table_size == 4096u); /* default unchanged */

	/* enable_push: > 1 rejected */
	ret = hive_options_set_enable_push(opt, 2u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);
	ASSERT(opt->opt_enable_push == 1u);

	/* max_concurrent_streams: 0 rejected */
	ret = hive_options_set_max_concurrent_streams(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);
	ASSERT(opt->opt_max_concurrent_streams == 100u);

	/* max_concurrent_streams: > 65535 rejected */
	ret = hive_options_set_max_concurrent_streams(opt, 65536u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* initial_window_size: 0 rejected */
	ret = hive_options_set_initial_window_size(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);
	ASSERT(opt->opt_initial_window_size == 65535u);

	/* initial_window_size: > 2^31-1 rejected */
	ret = hive_options_set_initial_window_size(opt, 2147483648u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_frame_size: < 16384 rejected */
	ret = hive_options_set_max_frame_size(opt, 16383u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);
	ASSERT(opt->opt_max_frame_size == 16384u);

	/* max_frame_size: > 16777215 rejected */
	ret = hive_options_set_max_frame_size(opt, 16777216u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_header_list_size: 0 rejected */
	ret = hive_options_set_max_header_list_size(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_header_count: 0 rejected */
	ret = hive_options_set_max_header_count(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_continuation_size: < 16384 rejected */
	ret = hive_options_set_max_continuation_size(opt, 16383u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_settings_pending: 0 rejected */
	ret = hive_options_set_max_settings_pending(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_settings_pending: > 255 rejected */
	ret = hive_options_set_max_settings_pending(opt, 256u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* rst_stream_flood_threshold: 0 rejected */
	ret = hive_options_set_rst_stream_flood_threshold(opt, 0u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* rst_stream_flood_window_secs: > 3600 rejected */
	ret = hive_options_set_rst_stream_flood_window_secs(opt, 3601u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_send_iov: < 64 rejected */
	ret = hive_options_set_max_send_iov(opt, 63u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_send_iov: > HIVE_SEND_IOV_MAX rejected */
	ret = hive_options_set_max_send_iov(opt, HIVE_SEND_IOV_MAX + 1u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_header_string_size: < 256 rejected */
	ret = hive_options_set_max_header_string_size(opt, 255u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* max_header_string_size: > 65536 rejected */
	ret = hive_options_set_max_header_string_size(opt, 65537u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* no_http_messaging: > 1 rejected */
	ret = hive_options_set_no_http_messaging(opt, 2u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* no_auto_ping_ack: > 1 rejected */
	ret = hive_options_set_no_auto_ping_ack(opt, 2u);
	ASSERT(ret == HIVE_ERR_INVALID_ARG);

	/* NULL opt pointer: all setters reject it */
	ASSERT(hive_options_set_header_table_size(NULL, 4096u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_enable_push(NULL, 0u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_concurrent_streams(NULL, 1u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_initial_window_size(NULL, 65535u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_frame_size(NULL, 16384u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_header_list_size(NULL, 65536u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_header_count(NULL, 100u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_continuation_size(NULL, 65536u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_settings_pending(NULL, 3u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_rst_stream_flood_threshold(NULL, 100u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_rst_stream_flood_window_secs(NULL, 10u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_send_iov(NULL, 512u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_max_header_string_size(NULL, 8192u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_no_http_messaging(NULL, 0u)
	    == HIVE_ERR_INVALID_ARG);
	ASSERT(hive_options_set_no_auto_ping_ack(NULL, 0u)
	    == HIVE_ERR_INVALID_ARG);

	hive_options_free(opt);
	return 1;
}

