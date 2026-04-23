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
 * The session struct is initialised manually on the stack, following the same
 * pattern as test_frame.c.  Full session lifecycle (create / free) is tested
 * in tasks 4.1–4.3.
 *
 * See DEVELOPMENT.md — Phase 4, Task 4.0.
 * See ARCHITECTURE.md §6.1, §6.3, §6.6.
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
	ASSERT(s.send_iov_count == 1u);
	ASSERT(s.send_buf_used == 9u); /* header only */

	/* Wire up a full-write send callback */
	s.callbacks.send = send_cb_full;
	s.user_data      = NULL;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);

	/* Queue drained — want_write must be false */
	ASSERT(hive_session_want_write(&s) == 0);
	ASSERT(s.send_iov_count == 0u);
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
	ASSERT(s.send_iov_count == 1u);
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
	ASSERT(s.send_iov_count == 1u); /* queue not reset yet */

	/* Second send: effective iov covers remaining 12 bytes; callback
	 * returns the full amount on calls > 1. */
	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);

	/* Queue fully drained */
	ASSERT(hive_session_want_write(&s) == 0);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_partial_offset == 0u);
	ASSERT(s.send_iov_count == 0u);
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

