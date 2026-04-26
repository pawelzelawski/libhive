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
new_server_security_session(uint32_t max_settings_pending)
{
	hive_callbacks_t cb;
	hive_options_t *opt;
	hive_session_t *s;

	opt = hive_options_new();
	ASSERT(opt != NULL);
	ASSERT(hive_options_set_max_settings_pending(opt, max_settings_pending) ==
	    HIVE_OK);

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, opt, &cb, NULL);
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

	s = new_server_security_session(1u);

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

	s = new_server_security_session(1u);

	n = build_settings_frame(frame, HIVE_FLAG_ACK);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

