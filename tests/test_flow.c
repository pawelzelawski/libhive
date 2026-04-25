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
int test_data_recv_zero_copy(void);
int test_data_recv_partial(void);
int test_data_recv_exceeds_stream_window(void);
int test_data_recv_exceeds_connection_window(void);
int test_window_update_coalescing(void);
int test_send_window_blocks_data(void);
int test_send_max_len_respects_remote_max_frame_size(void);
int test_want_write_pending_data_source(void);
int test_want_write_blocked_by_connection_window(void);

typedef struct {
	const uint8_t *buf_start;
	const uint8_t *buf_end;
	const uint8_t *last_ptr;
	size_t last_len;
	size_t total;
	int calls;
	int out_of_range;
} data_cap_t;

static int
on_data_chunk_capture(hive_session_t *session,
                      uint32_t stream_id,
                      const uint8_t *data,
                      size_t len,
                      uint8_t flags,
                      void *user_data)
{
	data_cap_t *cap;

	(void)session;
	(void)stream_id;
	(void)flags;

	cap = user_data;
	cap->calls++;
	cap->total += len;
	cap->last_ptr = data;
	cap->last_len = len;
	if (len > 0 && (data < cap->buf_start || data + len > cap->buf_end))
		cap->out_of_range = 1;

	return HIVE_OK;
}

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

static size_t
build_data_frame(uint8_t *dst,
                 uint32_t stream_id,
                 uint8_t flags,
                 const uint8_t *payload,
                 uint32_t payload_len)
{
	frame_hdr_write_at(dst, payload_len, HIVE_FRAME_DATA, flags, stream_id);
	if (payload_len > 0)
		memcpy(dst + 9, payload, payload_len);
	return 9u + payload_len;
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

static hive_session_t *
new_server_recv_session_with_data_cb(data_cap_t *cap)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	cb.on_data_chunk = on_data_chunk_capture;

	s = hive_session_server_new(NULL, NULL, &cb, cap);
	ASSERT(s != NULL);

	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;
	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	return s;
}

typedef struct {
	int calls;
	uint32_t last_max_len;
} send_read_cap_t;

static ssize_t
send_read_capture_cb(hive_session_t *session,
                     uint32_t stream_id,
                     uint8_t **buf,
                     uint32_t flags,
                     void *user_data)
{
	send_read_cap_t *cap;

	(void)session;
	(void)stream_id;
	(void)buf;

	cap = user_data;
	cap->calls++;
	cap->last_max_len = flags;

	/* Phase 5.3: callback returning 0 means skip this stream. */
	return 0;
}

static hive_session_t *
new_server_send_session(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	/* Ignore queued server preface while send-path testing. */
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

int
test_data_recv_zero_copy(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	data_cap_t cap;
	uint8_t frame[64];
	const uint8_t payload[] = "hello";
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session_with_data_cb(&cap);
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	n = build_data_frame(frame, 1u, 0u, payload, 5u);
	cap.buf_start = frame;
	cap.buf_end = frame + n;

	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.calls == 1);
	ASSERT(cap.total == 5u);
	ASSERT(cap.last_ptr == frame + 9);
	ASSERT(cap.last_len == 5u);
	ASSERT(cap.out_of_range == 0);
	ASSERT(st->recv_window ==
	       (int32_t)s->local_settings.initial_window_size - 5);

	hive_session_free(s);
	return 1;
}

int
test_data_recv_partial(void)
{
	hive_session_t *s;
	data_cap_t cap;
	uint8_t frame[64];
	const uint8_t payload[] = "abcdef";
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session_with_data_cb(&cap);
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);

	n = build_data_frame(frame, 1u, 0u, payload, 6u);
	cap.buf_start = frame;
	cap.buf_end = frame + n;

	ASSERT(hive_session_recv(s, frame, 11u) == 11);
	ASSERT(hive_session_recv(s, frame + 11u, n - 11u) ==
	       (ssize_t)(n - 11u));
	ASSERT(cap.calls == 2);
	ASSERT(cap.total == 6u);
	ASSERT(cap.out_of_range == 0);

	hive_session_free(s);
	return 1;
}

int
test_data_recv_exceeds_stream_window(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	const uint8_t *p;
	uint8_t frame[64];
	const uint8_t payload[] = "12345";
	size_t n;

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	st->recv_window = 4;

	n = build_data_frame(frame, 1u, 0u, payload, 5u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(s->closed == 0);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(s->send_iov_count == 1);

	p = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 13u);
	ASSERT(p[3] == HIVE_FRAME_RST_STREAM);
	ASSERT(p[8] == 1u);
	ASSERT(p[12] == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(st->recv_window == 4);

	hive_session_free(s);
	return 1;
}

int
test_data_recv_exceeds_connection_window(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	uint8_t frame[64];
	const uint8_t payload[] = "12345";
	size_t n;

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	st->recv_window = 100;
	s->recv_window = 4;

	n = build_data_frame(frame, 1u, 0u, payload, 5u);
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->closed == 1);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);
	ASSERT(s->send_iov_count == 0);

	hive_session_free(s);
	return 1;
}

int
test_window_update_coalescing(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	const uint8_t *p;
	uint8_t frame[64];
	const uint8_t payload[] = "ABCDEF";
	uint32_t increment;
	size_t n;

	s = new_server_recv_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	st->recv_window = 10;
	st->recv_consumed = 0;
	s->recv_window = 20;
	s->recv_consumed = 0;

	n = build_data_frame(frame, 1u, 0u, payload, 6u);
	ASSERT(hive_session_recv(s, frame, 12u) == 12);
	ASSERT(hive_session_recv(s, frame + 12u, n - 12u) ==
	       (ssize_t)(n - 12u));

	ASSERT(s->send_iov_count == 1);
	p = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 13u);
	ASSERT(p[3] == HIVE_FRAME_WINDOW_UPDATE);
	ASSERT(p[8] == 1u);
	increment = ((uint32_t)p[9] << 24) | ((uint32_t)p[10] << 16) |
	            ((uint32_t)p[11] << 8) | (uint32_t)p[12];
	ASSERT(increment == 6u);
	ASSERT(st->recv_window == 10);
	ASSERT(st->recv_consumed == 0u);
	ASSERT(s->recv_window == 14);
	ASSERT(s->recv_consumed == 6u);

	hive_session_free(s);
	return 1;
}

int
test_send_window_blocks_data(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	send_read_cap_t cap;
	uint8_t frame[13];
	size_t n;

	memset(&cap, 0, sizeof(cap));

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	st->data_source.read_callback = send_read_capture_cb;
	st->data_source.user_data = &cap;
	st->send_window = 0;

	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 0);

	n = build_window_update_frame(frame, 1u, 128u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(st->send_window == 128);

	/* Flush queued RST/ACK/WINDOW_UPDATE test artifacts if any. */
	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;

	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 1);
	ASSERT(cap.last_max_len == 128u);

	hive_session_free(s);
	return 1;
}

int
test_send_max_len_respects_remote_max_frame_size(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	send_read_cap_t cap;

	memset(&cap, 0, sizeof(cap));

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	s->remote_settings.max_frame_size = 100u;
	s->send_window = 1000;
	st->send_window = 500;
	st->data_source.read_callback = send_read_capture_cb;
	st->data_source.user_data = &cap;

	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 1);
	ASSERT(cap.last_max_len == 100u);

	hive_session_free(s);
	return 1;
}

int
test_want_write_pending_data_source(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	send_read_cap_t cap;

	memset(&cap, 0, sizeof(cap));

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	st->data_source.read_callback = send_read_capture_cb;
	st->data_source.user_data = &cap;
	ASSERT(s->send_iov_count == 0);
	ASSERT(s->send_partial == 0);
	ASSERT(s->send_window > 0);

	ASSERT(hive_session_want_write(s) == 1);

	hive_session_free(s);
	return 1;
}

int
test_want_write_blocked_by_connection_window(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	send_read_cap_t cap;

	memset(&cap, 0, sizeof(cap));

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);

	st->data_source.read_callback = send_read_capture_cb;
	st->data_source.user_data = &cap;
	s->send_window = 0;
	ASSERT(s->send_iov_count == 0);
	ASSERT(s->send_partial == 0);

	ASSERT(hive_session_want_write(s) == 0);

	hive_session_free(s);
	return 1;
}

