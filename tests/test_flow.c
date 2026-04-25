/* test_flow.c -- Phase 5 flow-control tests (Task 5.1: WINDOW_UPDATE recv) */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/uio.h>

#include "../include/hive.h"
#include "../src/hive_frame_bare.h"
#include "../src/hive_internal.h"
#include "test_harness.h"

int test_window_update_connection(void);
int test_window_update_stream(void);
int test_window_update_zero_increment_connection(void);
int test_window_update_zero_increment_stream(void);
int test_window_update_overflow(void);

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
build_window_update_frame(uint8_t *dst, uint32_t stream_id, uint32_t increment)
{
	dst[9] = (uint8_t)((increment >> 24) & 0xffu);
	dst[10] = (uint8_t)((increment >> 16) & 0xffu);
	dst[11] = (uint8_t)((increment >> 8) & 0xffu);
	dst[12] = (uint8_t)(increment & 0xffu);
	frame_hdr_write_at(dst, 4u, HIVE_FRAME_WINDOW_UPDATE, 0u, stream_id);
	return 13u;
}

static hive_session_t *
new_server_recv_session(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	/* Ignore queued server preface while receive-path testing. */
	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;
	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	return s;
}

int
test_window_update_connection(void)
{
	hive_session_t *s;
	uint8_t frame[13];
	size_t n;
	int32_t before;

	s = new_server_recv_session();
	before = s->send_window;

	n = build_window_update_frame(frame, 0u, 1024u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(s->send_window == before + 1024);
	ASSERT(s->send_iov_count == 0);

	hive_session_free(s);
	return 1;
}

int
test_window_update_stream(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	uint8_t frame[13];
	size_t n;
	int32_t before;

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	before = st->send_window;

	n = build_window_update_frame(frame, 1u, 2048u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(st->send_window == before + 2048);
	ASSERT(s->send_iov_count == 0);

	hive_session_free(s);
	return 1;
}

int
test_window_update_zero_increment_connection(void)
{
	hive_session_t *s;
	uint8_t frame[13];
	size_t n;

	s = new_server_recv_session();

	n = build_window_update_frame(frame, 0u, 0u);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s->closed == 1);

	hive_session_free(s);
	return 1;
}

int
test_window_update_zero_increment_stream(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	const uint8_t *p;
	uint8_t frame[13];
	size_t n;

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	n = build_window_update_frame(frame, 1u, 0u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(s->closed == 0);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(s->send_iov_count == 1);

	p = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 13u);
	ASSERT(p[3] == HIVE_FRAME_RST_STREAM);
	ASSERT(p[5] == 0u);
	ASSERT(p[6] == 0u);
	ASSERT(p[7] == 0u);
	ASSERT(p[8] == 1u);
	ASSERT(p[9] == 0u);
	ASSERT(p[10] == 0u);
	ASSERT(p[11] == 0u);
	ASSERT(p[12] == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(st->send_window == (int32_t)s->remote_settings.initial_window_size);

	hive_session_free(s);
	return 1;
}

int
test_window_update_overflow(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	const uint8_t *p;
	uint8_t frame[13];
	size_t n;

	s = new_server_recv_session();
	s->send_window = 0x7fffffff;
	n = build_window_update_frame(frame, 0u, 1u);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(s->closed == 1);
	hive_session_free(s);

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	st->send_window = 0x7fffffff;

	n = build_window_update_frame(frame, 1u, 1u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(s->closed == 0);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(s->send_iov_count == 1);

	p = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 13u);
	ASSERT(p[3] == HIVE_FRAME_RST_STREAM);
	ASSERT(p[5] == 0u);
	ASSERT(p[6] == 0u);
	ASSERT(p[7] == 0u);
	ASSERT(p[8] == 1u);
	ASSERT(p[9] == 0u);
	ASSERT(p[10] == 0u);
	ASSERT(p[11] == 0u);
	ASSERT(p[12] == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(st->send_window == 0x7fffffff);

	hive_session_free(s);
	return 1;
}

