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
#include <stdlib.h>
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
int test_send_partial_resume(void);
int test_send_fires_once_per_call(void);
int test_iovec_overflow(void);
int test_iovec_overflow_wouldblock(void);
int test_send_fatal_error(void);
int test_send_headers_single_frame_layout(void);
int test_send_headers_split_layout(void);
int test_send_headers_split_end_stream_flag(void);
int test_submit_response_headers_only(void);
int test_submit_response_with_data_copy(void);
int test_submit_response_no_copy(void);
int test_submit_response_eof_flag(void);
int test_full_get_request_response(void);
int test_submit_trailers(void);
int test_submit_interim_response(void);
int test_submit_rst_stream(void);
int test_submit_goaway_prepare(void);
int test_submit_goaway_final(void);
int test_submit_ping(void);
int test_submit_ping_ack(void);
int test_submit_request_assigns_stream_id(void);
int test_submit_request_max_concurrent_honored(void);
int test_submit_request_with_body(void);
int test_stream_get_state(void);
int test_stream_user_data(void);
int test_options_defaults(void);
int test_options_set_valid(void);
int test_options_set_invalid(void);
int test_session_server_new_null_alloc(void);
int test_session_client_new_null_alloc(void);
int test_session_new_custom_alloc(void);
int test_session_free_all_allocations(void);
int test_session_new_alloc_failure(void);
int test_options_set_max_concurrent(void);
int test_stream_open_lookup_close(void);
int test_stream_hash_collision(void);
int test_stream_free_stack(void);
int test_stream_compaction(void);
int test_recv_headers_opens_new_stream(void);
int test_recv_get_request_headers(void);
int test_stream_id_monotonicity(void);
int test_settings_recv_and_ack(void);
int test_settings_recv_ack(void);
int test_settings_invalid_window_size(void);
int test_settings_invalid_frame_size(void);
int test_settings_header_table_size_updates_encoder(void);
int test_settings_header_table_size_pending_min(void);
int test_settings_initial_window_retroactive_adjust(void);
int test_settings_initial_window_retroactive_overflow(void);
int test_server_preface_valid(void);
int test_server_preface_invalid(void);
int test_client_preface_first_frame_not_settings(void);
int test_client_preface_settings_with_ack(void);
int test_on_settings_ack_fires(void);
int test_on_goaway_fires(void);
int test_on_ping_fires_when_no_auto_ack(void);
int test_on_ping_ack_fires(void);
int test_on_connection_error_fires_before_goaway(void);
int test_h2c_upgrade_settings_applied(void);
int test_h2c_upgrade_stream1_open(void);
int test_h2c_feed_upgrade_headers_fires_callbacks(void);

/* ------------------------------------------------------------------ */
/* Shared test infrastructure                                          */
/* ------------------------------------------------------------------ */

#define TEST_SEND_BUF_CAP 4096u
#define TEST_SEND_IOV_CAP 32u

static const uint8_t test_client_preface_magic[24] = {
	'P', 'R', 'I',  ' ',  '*',  ' ',  'H', 'T', 'T',  'P',  '/',  '2',
	'.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};

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

typedef struct {
	int call_count;
	int last_iovcnt;
} send_count_state_t;

typedef struct {
	int calls;
	int iovcnt;
	struct iovec iov[8];
} send_capture_t;

typedef struct {
	const uint8_t *data;
	size_t len;
} no_copy_src_t;

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

static ssize_t
send_cb_counting(hive_session_t *session,
                 const struct iovec *iov,
                 int iovcnt,
                 void *user_data)
{
	send_count_state_t *st;
	ssize_t total;
	int i;

	(void)session;

	st = user_data;
	total = 0;
	for (i = 0; i < iovcnt; i++)
		total += (ssize_t)iov[i].iov_len;

	st->call_count++;
	st->last_iovcnt = iovcnt;
	return total;
}

static ssize_t
send_cb_capture(hive_session_t *session,
                const struct iovec *iov,
                int iovcnt,
                void *user_data)
{
	send_capture_t *cap;
	ssize_t total;
	int i;

	(void)session;

	cap = user_data;
	total = 0;
	cap->calls++;
	cap->iovcnt = iovcnt;
	ASSERT(iovcnt <= (int)(sizeof(cap->iov) / sizeof(cap->iov[0])));
	for (i = 0; i < iovcnt; i++) {
		cap->iov[i] = iov[i];
		total += (ssize_t)iov[i].iov_len;
	}

	return total;
}

static ssize_t
resp_read_copy_eof_cb(hive_session_t *session,
                      uint32_t stream_id,
                      uint8_t **buf,
                      size_t length,
                      uint32_t *data_flags,
                      hive_data_source_t *source,
                      void *user_data)
{
	static const uint8_t body[] = "hello";

	(void)session;
	(void)stream_id;
	(void)source;
	(void)user_data;

	ASSERT(buf != NULL);
	ASSERT(*buf != NULL);
	ASSERT(length >= sizeof(body) - 1u);
	ASSERT(data_flags != NULL);

	memcpy(*buf, body, sizeof(body) - 1u);
	*data_flags = HIVE_DATA_FLAG_EOF;
	return (ssize_t)(sizeof(body) - 1u);
}

static ssize_t
resp_read_no_copy_eof_cb(hive_session_t *session,
                         uint32_t stream_id,
                         uint8_t **buf,
                         size_t length,
                         uint32_t *data_flags,
                         hive_data_source_t *source,
                         void *user_data)
{
	no_copy_src_t *src;

	(void)session;
	(void)stream_id;
	(void)user_data;

	ASSERT(buf != NULL);
	ASSERT(data_flags != NULL);
	ASSERT(source != NULL);

	src = source->ptr;
	ASSERT(src != NULL);
	ASSERT(length >= src->len);

	*buf = (uint8_t *)src->data;
	*data_flags = HIVE_DATA_FLAG_NO_COPY | HIVE_DATA_FLAG_EOF;
	return (ssize_t)src->len;
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

typedef struct {
	size_t alloc_calls;
	size_t free_calls;
	size_t outstanding;
	int fail_after;
} alloc_track_t;

typedef struct {
	int settings_count;
	int settings_ack_count;
} settings_capture_t;

typedef struct {
	int begin_count;
	int header_count;
	int complete_count;
	int seq;
	int begin_seq;
	int first_header_seq;
	int last_header_seq;
	int complete_seq;
	uint32_t stream_id;
	uint8_t headers_complete_flags;
	int saw_method;
	int saw_scheme;
	int saw_path;
	int saw_authority;
} headers_capture_t;

typedef struct {
	int settings_ack_count;
	int goaway_count;
	int ping_count;
	int ping_ack_count;
	int connection_error_count;
	int connection_error_saw_goaway_queued;
	int connection_error_hive_err;
	uint32_t connection_error_h2_err;
	uint32_t goaway_last_stream_id;
	uint32_t goaway_error_code;
	const uint8_t *goaway_debug_data;
	size_t goaway_debug_len;
	uint8_t ping_opaque[8];
	uint8_t ping_ack_opaque[8];
} callback_capture_t;

typedef struct {
	int calls;
	size_t len;
	uint8_t bytes[512];
} roundtrip_send_capture_t;

typedef struct {
	roundtrip_send_capture_t wire;
	int headers_complete_count;
	int submit_count;
	int submit_rc;
} roundtrip_server_ctx_t;

typedef struct {
	int begin_count;
	int header_count;
	int complete_count;
	int data_count;
	int saw_status_200;
	uint32_t stream_id;
	uint8_t headers_complete_flags;
	uint8_t data_flags;
	size_t data_len;
	uint8_t data[32];
} roundtrip_client_capture_t;

static int
alloc_should_fail(alloc_track_t *st)
{
	st->alloc_calls++;
	return (st->fail_after > 0 &&
	    st->alloc_calls >= (size_t)st->fail_after) ? 1 : 0;
}

static void *
track_malloc(size_t size, void *ctx)
{
	alloc_track_t *st;
	void *p;

	st = ctx;
	if (alloc_should_fail(st))
		return NULL;
	p = malloc(size);
	if (p != NULL)
		st->outstanding++;
	return p;
}

static void
track_free(void *ptr, void *ctx)
{
	alloc_track_t *st;

	st = ctx;
	if (ptr != NULL) {
		st->free_calls++;
		st->outstanding--;
	}
	free(ptr);
}

static void *
track_calloc(size_t nmemb, size_t size, void *ctx)
{
	alloc_track_t *st;
	void *p;

	st = ctx;
	if (alloc_should_fail(st))
		return NULL;
	p = calloc(nmemb, size);
	if (p != NULL)
		st->outstanding++;
	return p;
}

static void *
track_realloc(void *ptr, size_t size, void *ctx)
{
	alloc_track_t *st;
	void *p;

	st = ctx;
	if (ptr == NULL) {
		if (alloc_should_fail(st))
			return NULL;
		p = realloc(NULL, size);
		if (p != NULL)
			st->outstanding++;
		return p;
	}
	p = realloc(ptr, size);
	return p;
}

static hive_mem_t
track_mem(alloc_track_t *st)
{
	hive_mem_t mem;

	mem.malloc = track_malloc;
	mem.free = track_free;
	mem.calloc = track_calloc;
	mem.realloc = track_realloc;
	mem.ctx = st;
	return mem;
}

static int
on_settings_capture(hive_session_t *session, void *user_data)
{
	settings_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->settings_count++;
	return HIVE_OK;
}

static int
on_settings_ack_capture(hive_session_t *session, void *user_data)
{
	settings_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->settings_ack_count++;
	return HIVE_OK;
}

static int
on_begin_headers_capture(hive_session_t *session,
    uint32_t stream_id,
    void *user_data)
{
	headers_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->begin_count++;
	cap->stream_id = stream_id;
	cap->begin_seq = ++cap->seq;
	return HIVE_OK;
}

static int
on_header_capture(hive_session_t *session,
    uint32_t stream_id,
    hive_buf_t *name,
    hive_buf_t *value,
    uint8_t flags,
    void *user_data)
{
	headers_capture_t *cap;

	(void)session;
	(void)flags;
	cap = user_data;
	ASSERT(name != NULL);
	ASSERT(value != NULL);
	ASSERT((name->flags & HIVE_BUF_VALID) != 0u);
	ASSERT((value->flags & HIVE_BUF_VALID) != 0u);

	if (cap->first_header_seq == 0)
		cap->first_header_seq = cap->seq + 1;
	cap->last_header_seq = ++cap->seq;
	cap->header_count++;
	cap->stream_id = stream_id;

	if (name->len == 7u && memcmp(name->data, ":method", 7u) == 0 &&
	    value->len == 3u && memcmp(value->data, "GET", 3u) == 0)
		cap->saw_method = 1;
	if (name->len == 7u && memcmp(name->data, ":scheme", 7u) == 0 &&
	    value->len == 4u && memcmp(value->data, "http", 4u) == 0)
		cap->saw_scheme = 1;
	if (name->len == 5u && memcmp(name->data, ":path", 5u) == 0 &&
	    value->len == 1u && memcmp(value->data, "/", 1u) == 0)
		cap->saw_path = 1;
	if (name->len == 10u && memcmp(name->data, ":authority", 10u) == 0 &&
	    value->len == 15u &&
	    memcmp(value->data, "www.example.com", 15u) == 0)
		cap->saw_authority = 1;

	return HIVE_OK;
}

static int
on_headers_complete_capture(hive_session_t *session,
    uint32_t stream_id,
    uint8_t flags,
    void *user_data)
{
	headers_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->complete_count++;
	cap->stream_id = stream_id;
	cap->headers_complete_flags = flags;
	cap->complete_seq = ++cap->seq;
	return HIVE_OK;
}

static int
on_settings_ack_cb(hive_session_t *session, void *user_data)
{
	callback_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->settings_ack_count++;
	return HIVE_OK;
}

static int
on_goaway_cb(hive_session_t *session,
    uint32_t last_stream_id,
    uint32_t error_code,
    const uint8_t *debug_data,
    size_t debug_len,
    void *user_data)
{
	callback_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->goaway_count++;
	cap->goaway_last_stream_id = last_stream_id;
	cap->goaway_error_code = error_code;
	cap->goaway_debug_data = debug_data;
	cap->goaway_debug_len = debug_len;
	return HIVE_OK;
}

static int
on_ping_cb(hive_session_t *session,
    const uint8_t opaque[8],
    void *user_data)
{
	callback_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->ping_count++;
	memcpy(cap->ping_opaque, opaque, sizeof(cap->ping_opaque));
	return HIVE_OK;
}

static int
on_ping_ack_cb(hive_session_t *session,
    const uint8_t opaque[8],
    void *user_data)
{
	callback_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->ping_ack_count++;
	memcpy(cap->ping_ack_opaque, opaque, sizeof(cap->ping_ack_opaque));
	return HIVE_OK;
}

static int
on_connection_error_cb(hive_session_t *session,
    int hive_err,
    uint32_t h2_error_code,
    void *user_data)
{
	callback_capture_t *cap;

	cap = user_data;
	cap->connection_error_count++;
	cap->connection_error_hive_err = hive_err;
	cap->connection_error_h2_err = h2_error_code;
	cap->connection_error_saw_goaway_queued =
	    (session->send_iov_count > 0) ? 1 : 0;
	return HIVE_OK;
}

static ssize_t
send_cb_roundtrip_collect(hive_session_t *session,
    const struct iovec *iov,
    int iovcnt,
    void *user_data)
{
	roundtrip_server_ctx_t *ctx;
	ssize_t total;
	size_t i;

	(void)session;

	ctx = user_data;
	total = 0;
	ctx->wire.calls++;
	for (i = 0u; i < (size_t)iovcnt; i++) {
		ASSERT(ctx->wire.len + iov[i].iov_len <= sizeof(ctx->wire.bytes));
		memcpy(ctx->wire.bytes + ctx->wire.len,
		    iov[i].iov_base,
		    iov[i].iov_len);
		ctx->wire.len += iov[i].iov_len;
		total += (ssize_t)iov[i].iov_len;
	}

	return total;
}

static int
on_headers_complete_submit_response(hive_session_t *session,
    uint32_t stream_id,
    uint8_t flags,
    void *user_data)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	hive_nv_t nva[1];
	hive_data_source_t ds;
	roundtrip_server_ctx_t *ctx;

	(void)flags;

	ctx = user_data;
	ctx->headers_complete_count++;
	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;
	ds.read_callback = resp_read_copy_eof_cb;
	ds.ptr = NULL;

	ctx->submit_rc = hive_submit_response(session, stream_id, nva, 1u, &ds);
	if (ctx->submit_rc == HIVE_OK)
		ctx->submit_count++;
	return ctx->submit_rc;
}

static int
on_roundtrip_begin_headers(hive_session_t *session,
    uint32_t stream_id,
    void *user_data)
{
	roundtrip_client_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->begin_count++;
	cap->stream_id = stream_id;
	return HIVE_OK;
}

static int
on_roundtrip_header(hive_session_t *session,
    uint32_t stream_id,
    hive_buf_t *name,
    hive_buf_t *value,
    uint8_t flags,
    void *user_data)
{
	roundtrip_client_capture_t *cap;

	(void)session;
	(void)flags;
	cap = user_data;
	cap->header_count++;
	cap->stream_id = stream_id;
	if (name->len == 7u && memcmp(name->data, ":status", 7u) == 0 &&
	    value->len == 3u && memcmp(value->data, "200", 3u) == 0)
		cap->saw_status_200 = 1;
	return HIVE_OK;
}

static int
on_roundtrip_headers_complete(hive_session_t *session,
    uint32_t stream_id,
    uint8_t flags,
    void *user_data)
{
	roundtrip_client_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->complete_count++;
	cap->stream_id = stream_id;
	cap->headers_complete_flags = flags;
	return HIVE_OK;
}

static int
on_roundtrip_data_chunk(hive_session_t *session,
    uint32_t stream_id,
    const uint8_t *data,
    size_t len,
    uint8_t flags,
    void *user_data)
{
	roundtrip_client_capture_t *cap;

	(void)session;
	cap = user_data;
	cap->data_count++;
	cap->stream_id = stream_id;
	cap->data_flags = flags;
	ASSERT(cap->data_len + len <= sizeof(cap->data));
	memcpy(cap->data + cap->data_len, data, len);
	cap->data_len += len;
	return HIVE_OK;
}

static void
settings_param_write(uint8_t *dst, uint16_t id, uint32_t value)
{
	dst[0] = (uint8_t)((id >> 8) & 0xffu);
	dst[1] = (uint8_t)(id & 0xffu);
	dst[2] = (uint8_t)((value >> 24) & 0xffu);
	dst[3] = (uint8_t)((value >> 16) & 0xffu);
	dst[4] = (uint8_t)((value >> 8) & 0xffu);
	dst[5] = (uint8_t)(value & 0xffu);
}

static size_t
build_settings_frame(uint8_t *dst, uint8_t flags,
    const uint8_t *payload, uint32_t payload_len)
{
	frame_hdr_write_at(dst, payload_len, HIVE_FRAME_SETTINGS, flags, 0u);
	if (payload_len > 0 && payload != NULL)
		memcpy(dst + 9, payload, payload_len);
	return 9u + (size_t)payload_len;
}

static uint32_t
read_u32_be(const uint8_t in[4])
{
	return ((uint32_t)in[0] << 24) |
	    ((uint32_t)in[1] << 16) |
	    ((uint32_t)in[2] << 8) |
	    (uint32_t)in[3];
}

static void
write_u32_be(uint8_t out[4], uint32_t v)
{
	out[0] = (uint8_t)((v >> 24) & 0xffu);
	out[1] = (uint8_t)((v >> 16) & 0xffu);
	out[2] = (uint8_t)((v >> 8) & 0xffu);
	out[3] = (uint8_t)(v & 0xffu);
}

static size_t
build_ping_frame(uint8_t *dst, uint8_t flags, const uint8_t opaque[8])
{
	frame_hdr_write_at(dst, 8u, HIVE_FRAME_PING, flags, 0u);
	memcpy(dst + 9, opaque, 8u);
	return 17u;
}

static size_t
build_goaway_frame(uint8_t *dst,
    uint32_t last_stream_id,
    uint32_t error_code,
    const uint8_t *debug_data,
    size_t debug_len)
{
	uint32_t payload_len;

	payload_len = 8u + (uint32_t)debug_len;
	frame_hdr_write_at(dst, payload_len, HIVE_FRAME_GOAWAY, 0u, 0u);
	write_u32_be(dst + 9, last_stream_id & 0x7fffffffu);
	write_u32_be(dst + 13, error_code);
	if (debug_len > 0)
		memcpy(dst + 17, debug_data, debug_len);
	return 9u + 8u + debug_len;
}

static hive_session_t *
new_server_recv_session(settings_capture_t *cap)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	cb.on_settings = on_settings_capture;
	cb.on_settings_ack = on_settings_ack_capture;

	s = hive_session_server_new(NULL, NULL, &cb, cap);
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

static hive_session_t *
new_server_callback_session(callback_capture_t *cap, uint8_t no_auto_ping_ack)
{
	hive_callbacks_t cb;
	hive_options_t *opt;
	hive_session_t *s;

	opt = NULL;
	if (no_auto_ping_ack != 0) {
		opt = hive_options_new();
		ASSERT(opt != NULL);
		ASSERT(hive_options_set_no_auto_ping_ack(opt, 1u) == HIVE_OK);
	}

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	cb.on_settings_ack = on_settings_ack_cb;
	cb.on_goaway = on_goaway_cb;
	cb.on_ping = on_ping_cb;
	cb.on_ping_ack = on_ping_ack_cb;
	cb.on_connection_error = on_connection_error_cb;

	s = hive_session_server_new(NULL, opt, &cb, cap);
	if (opt != NULL)
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
	s->opt_max_send_iov     = TEST_SEND_IOV_CAP;
}

/*
 * Build a real server session for HEADERS queueing tests (enc table/mem live),
 * then clear startup preface bytes so send queue assertions are isolated.
 */
static hive_session_t *
new_server_send_session(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;

	return s;
}

static hive_session_t *
new_client_send_session(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_client_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	/* Ignore queued client preface bytes so queue assertions are isolated. */
	s->send_iov_count = 0;
	s->send_buf_used = 0;
	s->send_partial = 0;
	s->send_partial_offset = 0;

	return s;
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
/* test_send_partial_resume                                            */
/* ------------------------------------------------------------------ */

int
test_send_partial_resume(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	static const uint8_t ping_payload[8] = {
	    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
	};
	partial_send_state_t st;
	hive_session_t s;
	size_t total;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);

	/* Queue 3 frames to force multi-iov partial-offset skipping. */
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	send_queue_append_ctrl(&s, HIVE_FRAME_PING, 0u, 0u, ping_payload, 8u);
	send_queue_append_ctrl(&s, HIVE_FRAME_WINDOW_UPDATE, 0u, 1u,
	    (const uint8_t[]){0x00, 0x00, 0x01, 0x00}, 4u);

	ASSERT(s.send_iov_count == 3);
	total = s.send_iov[0].iov_len + s.send_iov[1].iov_len +
	    s.send_iov[2].iov_len;
	ASSERT(total == 39u);

	st.call_count = 0;
	st.partial_bytes = (ssize_t)(total / 2u);
	s.callbacks.send = send_cb_partial;
	s.user_data = &st;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 1);
	ASSERT(s.send_partial == 1);
	ASSERT(s.send_partial_offset == (size_t)st.partial_bytes);
	ASSERT(hive_session_want_write(&s) == 1);

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_partial_offset == 0u);
	ASSERT(s.send_iov_count == 0);
	ASSERT(s.send_buf_used == 0u);
	ASSERT(hive_session_want_write(&s) == 0);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_send_fires_once_per_call                                       */
/* ------------------------------------------------------------------ */

int
test_send_fires_once_per_call(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	send_count_state_t st;
	hive_session_t s;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);

	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	ASSERT(s.send_iov_count == 3);

	memset(&st, 0, sizeof(st));
	s.callbacks.send = send_cb_counting;
	s.user_data = &st;

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 1);
	ASSERT(st.last_iovcnt == 3);
	ASSERT(s.send_iov_count == 0);

	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	send_queue_append_ctrl(&s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u,
	    NULL, 0u);
	ASSERT(s.send_iov_count == 2);

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);
	ASSERT(st.last_iovcnt == 2);
	ASSERT(s.send_iov_count == 0);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_iovec_overflow                                                 */
/* ------------------------------------------------------------------ */

int
test_iovec_overflow(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	send_count_state_t st;
	hive_session_t s;
	int i;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);
	s.opt_max_send_iov = 4u;
	memset(&st, 0, sizeof(st));
	s.callbacks.send = send_cb_counting;
	s.user_data = &st;

	for (i = 0; i < 4; i++) {
		ret = send_queue_append_ctrl(
		    &s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
		ASSERT(ret == HIVE_OK);
	}
	ASSERT(st.call_count == 0);
	ASSERT(s.send_iov_count == 4);

	ret = send_queue_append_ctrl(
	    &s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 1);
	ASSERT(st.last_iovcnt == 4);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_iov_count == 1);

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);
	ASSERT(st.last_iovcnt == 1);
	ASSERT(s.send_iov_count == 0);

	return 1;
}

/* ------------------------------------------------------------------ */
/* test_iovec_overflow_wouldblock                                      */
/* ------------------------------------------------------------------ */

int
test_iovec_overflow_wouldblock(void)
{
	static uint8_t buf[TEST_SEND_BUF_CAP];
	static struct iovec iov[TEST_SEND_IOV_CAP];
	partial_send_state_t st;
	hive_session_t s;
	int ret;

	test_send_session_init(&s, buf, sizeof(buf), iov);
	s.opt_max_send_iov = 1u;
	st.call_count = 0;
	st.partial_bytes = 5;
	s.callbacks.send = send_cb_partial;
	s.user_data = &st;

	ret = send_queue_append_ctrl(
	    &s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(s.send_iov_count == 1);

	ret = send_queue_append_ctrl(
	    &s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
	ASSERT(ret == HIVE_ERR_WOULDBLOCK);
	ASSERT(st.call_count == 1);
	ASSERT(s.send_partial == 1);
	ASSERT(s.send_iov_count == 1);

	ret = hive_session_send(&s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.call_count == 2);
	ASSERT(s.send_partial == 0);
	ASSERT(s.send_iov_count == 0);

	ret = send_queue_append_ctrl(
	    &s, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0u, NULL, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(s.send_iov_count == 1);

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
/* test_send_headers_single_frame_layout                              */
/* ------------------------------------------------------------------ */

int
test_send_headers_single_frame_layout(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	hive_nv_t nva[1];
	hive_session_t *s;
	frame_hdr_t hdr;
	uint8_t *p;
	int ret;

	s = new_server_send_session();

	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;

	ret = send_queue_append_headers(s, 1u, nva, 1u, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(s->send_iov_count == 1);

	p = s->send_iov[0].iov_base;
	frame_hdr_parse(p, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 1u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);
	ASSERT(s->send_iov[0].iov_len == 9u + (size_t)hdr.length);
	ASSERT(s->send_buf_used == s->send_iov[0].iov_len);

	hive_session_free(s);
	return 1;
}

/* ------------------------------------------------------------------ */
/* test_send_headers_split_layout                                     */
/* ------------------------------------------------------------------ */

int
test_send_headers_split_layout(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	static const uint8_t n_long[] = "x-long-header";
	static const uint8_t v_long[] =
	    "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz";
	hive_nv_t nva[2];
	hive_session_t *s;
	frame_hdr_t hdr;
	uint32_t max_frame;
	size_t i;
	size_t total_payload;
	size_t n_frames;
	int ret;

	s = new_server_send_session();
	s->remote_settings.max_frame_size = 8u;
	max_frame = s->remote_settings.max_frame_size;

	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;
	nva[1].name = n_long;
	nva[1].value = v_long;
	nva[1].name_len = sizeof(n_long) - 1u;
	nva[1].value_len = sizeof(v_long) - 1u;
	nva[1].flags = HIVE_NV_FLAG_NO_INDEX;

	ret = send_queue_append_headers(s, 3u, nva, 2u, 0u);
	ASSERT(ret == HIVE_OK);
	ASSERT(s->send_iov_count >= 4);
	ASSERT((s->send_iov_count % 2) == 0);

	total_payload = 0u;
	n_frames = (size_t)s->send_iov_count / 2u;
	for (i = 0; i < (size_t)s->send_iov_count; i += 2u) {
		frame_hdr_parse(s->send_iov[i].iov_base, &hdr);
		ASSERT(hdr.stream_id == 3u);
		if (i == 0u)
			ASSERT(hdr.type == HIVE_FRAME_HEADERS);
		else
			ASSERT(hdr.type == HIVE_FRAME_CONTINUATION);

		ASSERT(s->send_iov[i].iov_len == 9u);
		ASSERT(s->send_iov[i + 1u].iov_len == (size_t)hdr.length);
		ASSERT(hdr.length <= max_frame);

		if (i + 2u < (size_t)s->send_iov_count)
			ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) == 0u);
		else
			ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);

		total_payload += s->send_iov[i + 1u].iov_len;
	}

	ASSERT(s->send_buf_used ==
	    9u + total_payload + (n_frames > 0u ? (n_frames - 1u) * 9u : 0u));

	hive_session_free(s);
	return 1;
}

/* ------------------------------------------------------------------ */
/* test_send_headers_split_end_stream_flag                            */
/* ------------------------------------------------------------------ */

int
test_send_headers_split_end_stream_flag(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_204[] = "204";
	static const uint8_t n_long[] = "x-end-stream-check";
	static const uint8_t v_long[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	hive_nv_t nva[2];
	hive_session_t *s;
	frame_hdr_t hdr;
	size_t i;
	int ret;

	s = new_server_send_session();
	s->remote_settings.max_frame_size = 8u;

	nva[0].name = n_status;
	nva[0].value = v_204;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_204) - 1u;
	nva[0].flags = 0u;
	nva[1].name = n_long;
	nva[1].value = v_long;
	nva[1].name_len = sizeof(n_long) - 1u;
	nva[1].value_len = sizeof(v_long) - 1u;
	nva[1].flags = HIVE_NV_FLAG_NO_INDEX;

	ret = send_queue_append_headers(s, 5u, nva, 2u, 1u);
	ASSERT(ret == HIVE_OK);
	ASSERT(s->send_iov_count >= 4);

	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);

	for (i = 2u; i < (size_t)s->send_iov_count; i += 2u) {
		frame_hdr_parse(s->send_iov[i].iov_base, &hdr);
		ASSERT(hdr.type == HIVE_FRAME_CONTINUATION);
		ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);
	}

	frame_hdr_parse(
	    s->send_iov[(size_t)s->send_iov_count - 2u].iov_base, &hdr);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);

	hive_session_free(s);
	return 1;
}

int
test_submit_response_headers_only(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_204[] = "204";
	hive_nv_t nva[1];
	hive_session_t *s;
	frame_hdr_t hdr;
	int ret;

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_status;
	nva[0].value = v_204;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_204) - 1u;
	nva[0].flags = 0u;

	ret = hive_submit_response(s, 1u, nva, 1u, NULL);
	ASSERT(ret == HIVE_OK);
	ASSERT(s->send_iov_count == 1);

	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 1u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(stream_lookup(s, 1u) == NULL);

	hive_session_free(s);
	return 1;
}

int
test_submit_response_with_data_copy(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	hive_nv_t nva[1];
	hive_data_source_t ds;
	hive_session_t *s;
	hive_stream_t *st;
	send_capture_t cap;
	frame_hdr_t hdr;
	int ret;

	memset(&cap, 0, sizeof(cap));
	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;

	ds.read_callback = resp_read_copy_eof_cb;
	ds.ptr = NULL;

	ret = hive_submit_response(s, 1u, nva, 1u, &ds);
	ASSERT(ret == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	ASSERT(st->data_source.read_callback == resp_read_copy_eof_cb);

	s->callbacks.send = send_cb_capture;
	s->user_data = &cap;
	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 1);
	ASSERT(cap.iovcnt == 3);

	frame_hdr_parse(cap.iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 1u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);

	frame_hdr_parse(cap.iov[1].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_DATA);
	ASSERT(hdr.stream_id == 1u);
	ASSERT(hdr.length == 5u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(cap.iov[2].iov_len == 5u);
	ASSERT(memcmp(cap.iov[2].iov_base, "hello", 5u) == 0);
	ASSERT(stream_lookup(s, 1u) == NULL);

	hive_session_free(s);
	return 1;
}

int
test_submit_response_no_copy(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	static const uint8_t body[] = "no-copy-body";
	hive_nv_t nva[1];
	hive_data_source_t ds;
	hive_session_t *s;
	send_capture_t cap;
	no_copy_src_t src;
	frame_hdr_t hdr;

	memset(&cap, 0, sizeof(cap));
	s = new_server_send_session();
	ASSERT(stream_open(s, 3u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;

	src.data = body;
	src.len = sizeof(body) - 1u;
	ds.read_callback = resp_read_no_copy_eof_cb;
	ds.ptr = &src;

	ASSERT(hive_submit_response(s, 3u, nva, 1u, &ds) == HIVE_OK);
	s->callbacks.send = send_cb_capture;
	s->user_data = &cap;
	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 1);
	ASSERT(cap.iovcnt == 3);

	frame_hdr_parse(cap.iov[1].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_DATA);
	ASSERT(hdr.stream_id == 3u);
	ASSERT(hdr.length == src.len);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(cap.iov[2].iov_base == body);
	ASSERT(cap.iov[2].iov_len == src.len);

	hive_session_free(s);
	return 1;
}

int
test_submit_response_eof_flag(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_200[] = "200";
	hive_nv_t nva[1];
	hive_data_source_t ds;
	hive_session_t *s;
	send_capture_t cap;
	frame_hdr_t hdr;

	memset(&cap, 0, sizeof(cap));
	s = new_server_send_session();
	ASSERT(stream_open(s, 5u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_status;
	nva[0].value = v_200;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_200) - 1u;
	nva[0].flags = 0u;

	ds.read_callback = resp_read_copy_eof_cb;
	ds.ptr = NULL;

	ASSERT(hive_submit_response(s, 5u, nva, 1u, &ds) == HIVE_OK);
	s->callbacks.send = send_cb_capture;
	s->user_data = &cap;
	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.iovcnt == 3);

	frame_hdr_parse(cap.iov[1].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_DATA);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);

	hive_session_free(s);
	return 1;
}

int
test_full_get_request_response(void)
{
	hive_callbacks_t server_cb;
	hive_callbacks_t client_cb;
	roundtrip_server_ctx_t srv_ctx;
	roundtrip_client_capture_t cli_cap;
	hive_session_t *server;
	hive_session_t *client;
	uint8_t req_frame[29];
	uint8_t hpack_get[20];
	frame_hdr_t hdr;
	size_t off;

	memset(&server_cb, 0, sizeof(server_cb));
	memset(&client_cb, 0, sizeof(client_cb));
	memset(&srv_ctx, 0, sizeof(srv_ctx));
	memset(&cli_cap, 0, sizeof(cli_cap));

	server_cb.send = send_cb_roundtrip_collect;
	server_cb.on_headers_complete = on_headers_complete_submit_response;
	server = hive_session_server_new(NULL, NULL, &server_cb, &srv_ctx);
	ASSERT(server != NULL);
	server->send_iov_count = 0;
	server->send_buf_used = 0;
	server->send_partial = 0;
	server->send_partial_offset = 0;
	server->recv_state = RECV_FRAME_HEADER;
	server->preface_count = 0;

	hpack_get[0] = 0x82;
	hpack_get[1] = 0x86;
	hpack_get[2] = 0x84;
	hpack_get[3] = 0x41;
	hpack_get[4] = 0x0f;
	memcpy(hpack_get + 5, "www.example.com", 15u);

	frame_hdr_write_at(req_frame,
	    sizeof(hpack_get),
	    HIVE_FRAME_HEADERS,
	    HIVE_FLAG_END_HEADERS | HIVE_FLAG_END_STREAM,
	    1u);
	memcpy(req_frame + 9, hpack_get, sizeof(hpack_get));

	ASSERT(hive_session_recv(server, req_frame, sizeof(req_frame)) ==
	    (ssize_t)sizeof(req_frame));
	ASSERT(srv_ctx.headers_complete_count == 1);
	ASSERT(srv_ctx.submit_count == 1);
	ASSERT(srv_ctx.submit_rc == HIVE_OK);

	ASSERT(hive_session_send(server) == HIVE_OK);
	ASSERT(srv_ctx.wire.calls == 1);
	ASSERT(srv_ctx.wire.len > 18u);

	off = 0u;
	frame_hdr_parse(srv_ctx.wire.bytes + off, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 1u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);
	off += 9u + hdr.length;

	ASSERT(off + 9u <= srv_ctx.wire.len);
	frame_hdr_parse(srv_ctx.wire.bytes + off, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_DATA);
	ASSERT(hdr.stream_id == 1u);
	ASSERT(hdr.length == 5u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(off + 9u + hdr.length == srv_ctx.wire.len);
	ASSERT(memcmp(srv_ctx.wire.bytes + off + 9u, "hello", 5u) == 0);

	client_cb.send = send_cb_full;
	client_cb.on_begin_headers = on_roundtrip_begin_headers;
	client_cb.on_header = on_roundtrip_header;
	client_cb.on_headers_complete = on_roundtrip_headers_complete;
	client_cb.on_data_chunk = on_roundtrip_data_chunk;
	client = hive_session_client_new(NULL, NULL, &client_cb, &cli_cap);
	ASSERT(client != NULL);
	client->send_iov_count = 0;
	client->send_buf_used = 0;
	client->send_partial = 0;
	client->send_partial_offset = 0;
	client->recv_state = RECV_FRAME_HEADER;
	client->preface_count = 0;
	ASSERT(stream_open(client, 1u, HIVE_STREAM_HALF_CLOSED_LOCAL) == HIVE_OK);

	ASSERT(hive_session_recv(client, srv_ctx.wire.bytes, srv_ctx.wire.len) ==
	    (ssize_t)srv_ctx.wire.len);
	ASSERT(cli_cap.begin_count == 1);
	ASSERT(cli_cap.header_count >= 1);
	ASSERT(cli_cap.complete_count == 1);
	ASSERT(cli_cap.data_count == 1);
	ASSERT(cli_cap.stream_id == 1u);
	ASSERT(cli_cap.saw_status_200 == 1);
	ASSERT((cli_cap.headers_complete_flags & HIVE_FLAG_END_STREAM) == 0u);
	ASSERT((cli_cap.data_flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(cli_cap.data_len == 5u);
	ASSERT(memcmp(cli_cap.data, "hello", 5u) == 0);

	hive_session_free(client);
	hive_session_free(server);
	return 1;
}

int
test_submit_trailers(void)
{
	static const uint8_t n_etag[] = "etag";
	static const uint8_t v_etag[] = "abc";
	hive_nv_t nva[1];
	hive_session_t *s;
	frame_hdr_t hdr;

	s = new_server_send_session();
	ASSERT(stream_open(s, 1u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_etag;
	nva[0].value = v_etag;
	nva[0].name_len = sizeof(n_etag) - 1u;
	nva[0].value_len = sizeof(v_etag) - 1u;
	nva[0].flags = 0u;

	ASSERT(hive_submit_trailers(s, 1u, nva, 1u) == HIVE_OK);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 1u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(stream_lookup(s, 1u) == NULL);

	hive_session_free(s);
	return 1;
}

int
test_submit_interim_response(void)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_103[] = "103";
	hive_nv_t nva[1];
	hive_session_t *s;
	hive_stream_t *st;
	frame_hdr_t hdr;

	s = new_server_send_session();
	ASSERT(stream_open(s, 3u, HIVE_STREAM_HALF_CLOSED_REMOTE) == HIVE_OK);

	nva[0].name = n_status;
	nva[0].value = v_103;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_103) - 1u;
	nva[0].flags = 0u;

	ASSERT(hive_submit_interim_response(s, 3u, nva, 1u) == HIVE_OK);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == 3u);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);
	st = stream_lookup(s, 3u);
	ASSERT(st != NULL);
	ASSERT(st->state == HIVE_STREAM_HALF_CLOSED_REMOTE);

	hive_session_free(s);
	return 1;
}

int
test_submit_rst_stream(void)
{
	hive_session_t *s;
	frame_hdr_t hdr;
	const uint8_t *payload;

	s = new_server_send_session();
	ASSERT(stream_open(s, 5u, HIVE_STREAM_OPEN) == HIVE_OK);

	ASSERT(hive_submit_rst_stream(s, 5u, HIVE_H2_CANCEL) == HIVE_OK);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_RST_STREAM);
	ASSERT(hdr.stream_id == 5u);
	ASSERT(hdr.length == 4u);
	payload = (const uint8_t *)s->send_iov[0].iov_base + 9;
	ASSERT(read_u32_be(payload) == HIVE_H2_CANCEL);
	ASSERT(stream_lookup(s, 5u) == NULL);

	hive_session_free(s);
	return 1;
}

int
test_submit_goaway_prepare(void)
{
	hive_session_t *s;
	frame_hdr_t hdr;
	const uint8_t *payload;

	s = new_server_send_session();

	ASSERT(hive_submit_goaway_prepare(s) == HIVE_OK);
	ASSERT(s->goaway_prepare_sent == 1u);
	ASSERT(s->goaway_sent == 1u);
	ASSERT(s->session_state == HIVE_SESSION_OPEN);
	ASSERT(s->send_iov_count == 1);

	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_GOAWAY);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(hdr.length == 8u);
	payload = (const uint8_t *)s->send_iov[0].iov_base + 9;
	ASSERT(read_u32_be(payload) == 0x7fffffffu);
	ASSERT(read_u32_be(payload + 4) == HIVE_H2_NO_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_submit_goaway_final(void)
{
	static const uint8_t debug[] = {0xde, 0xad};
	hive_session_t *s;
	frame_hdr_t hdr;
	const uint8_t *payload;

	s = new_server_send_session();
	s->last_stream_id_remote = 7u;

	ASSERT(hive_submit_goaway_final(
	    s, HIVE_H2_PROTOCOL_ERROR, debug, sizeof(debug)) == HIVE_OK);
	ASSERT(s->goaway_sent == 1u);
	ASSERT(s->session_state == HIVE_SESSION_GOAWAY_SENT);
	ASSERT(s->send_iov_count == 1);

	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_GOAWAY);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(hdr.length == 10u);
	payload = (const uint8_t *)s->send_iov[0].iov_base + 9;
	ASSERT(read_u32_be(payload) == 7u);
	ASSERT(read_u32_be(payload + 4) == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(payload[8] == 0xde);
	ASSERT(payload[9] == 0xad);

	hive_session_free(s);
	return 1;
}

int
test_submit_ping(void)
{
	static const uint8_t opaque[8] =
	    {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
	hive_session_t *s;
	frame_hdr_t hdr;
	const uint8_t *payload;

	s = new_server_send_session();

	ASSERT(hive_submit_ping(s, opaque) == HIVE_OK);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_PING);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(hdr.length == 8u);
	ASSERT((hdr.flags & HIVE_FLAG_ACK) == 0u);
	payload = (const uint8_t *)s->send_iov[0].iov_base + 9;
	ASSERT(memcmp(payload, opaque, 8u) == 0);

	hive_session_free(s);
	return 1;
}

int
test_submit_ping_ack(void)
{
	static const uint8_t opaque[8] =
	    {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
	hive_session_t *s;
	frame_hdr_t hdr;
	const uint8_t *payload;

	s = new_server_send_session();

	ASSERT(hive_submit_ping_ack(s, opaque) == HIVE_OK);
	ASSERT(s->send_iov_count == 1);
	frame_hdr_parse(s->send_iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_PING);
	ASSERT(hdr.stream_id == 0u);
	ASSERT(hdr.length == 8u);
	ASSERT((hdr.flags & HIVE_FLAG_ACK) != 0u);
	payload = (const uint8_t *)s->send_iov[0].iov_base + 9;
	ASSERT(memcmp(payload, opaque, 8u) == 0);

	hive_session_free(s);
	return 1;
}

int
test_submit_request_assigns_stream_id(void)
{
	static const uint8_t n_method[] = ":method";
	static const uint8_t n_path[] = ":path";
	static const uint8_t n_scheme[] = ":scheme";
	static const uint8_t v_get[] = "GET";
	static const uint8_t v_http[] = "http";
	static const uint8_t v_path1[] = "/one";
	static const uint8_t v_path2[] = "/two";
	hive_nv_t nva1[3];
	hive_nv_t nva2[3];
	hive_session_t *s;
	uint32_t sid1;
	uint32_t sid2;

	s = new_client_send_session();

	nva1[0] = (hive_nv_t){n_method, v_get,
	    sizeof(n_method) - 1u, sizeof(v_get) - 1u, 0u};
	nva1[1] = (hive_nv_t){n_path, v_path1,
	    sizeof(n_path) - 1u, sizeof(v_path1) - 1u, 0u};
	nva1[2] = (hive_nv_t){n_scheme, v_http,
	    sizeof(n_scheme) - 1u, sizeof(v_http) - 1u, 0u};

	nva2[0] = (hive_nv_t){n_method, v_get,
	    sizeof(n_method) - 1u, sizeof(v_get) - 1u, 0u};
	nva2[1] = (hive_nv_t){n_path, v_path2,
	    sizeof(n_path) - 1u, sizeof(v_path2) - 1u, 0u};
	nva2[2] = (hive_nv_t){n_scheme, v_http,
	    sizeof(n_scheme) - 1u, sizeof(v_http) - 1u, 0u};

	ASSERT(hive_submit_request(s, nva1, 3u, NULL, &sid1) == HIVE_OK);
	ASSERT(hive_submit_request(s, nva2, 3u, NULL, &sid2) == HIVE_OK);

	ASSERT(sid1 == 1u);
	ASSERT(sid2 == 3u);
	ASSERT(s->next_stream_id == 5u);

	hive_session_free(s);
	return 1;
}

int
test_submit_request_max_concurrent_honored(void)
{
	static const uint8_t n_method[] = ":method";
	static const uint8_t n_path[] = ":path";
	static const uint8_t n_scheme[] = ":scheme";
	static const uint8_t v_get[] = "GET";
	static const uint8_t v_http[] = "http";
	static const uint8_t v_path[] = "/limit";
	hive_nv_t nva[3];
	hive_session_t *s;
	uint32_t sid;

	s = new_client_send_session();
	s->remote_settings.max_concurrent_streams = 2u;

	nva[0] = (hive_nv_t){n_method, v_get,
	    sizeof(n_method) - 1u, sizeof(v_get) - 1u, 0u};
	nva[1] = (hive_nv_t){n_path, v_path,
	    sizeof(n_path) - 1u, sizeof(v_path) - 1u, 0u};
	nva[2] = (hive_nv_t){n_scheme, v_http,
	    sizeof(n_scheme) - 1u, sizeof(v_http) - 1u, 0u};

	ASSERT(hive_submit_request(s, nva, 3u, NULL, &sid) == HIVE_OK);
	ASSERT(hive_submit_request(s, nva, 3u, NULL, &sid) == HIVE_OK);
	ASSERT(hive_submit_request(s, nva, 3u, NULL, &sid) ==
	    HIVE_ERR_REFUSED_STREAM);

	hive_session_free(s);
	return 1;
}

int
test_submit_request_with_body(void)
{
	static const uint8_t n_method[] = ":method";
	static const uint8_t n_path[] = ":path";
	static const uint8_t n_scheme[] = ":scheme";
	static const uint8_t v_post[] = "POST";
	static const uint8_t v_http[] = "http";
	static const uint8_t v_path[] = "/upload";
	hive_nv_t nva[3];
	hive_data_source_t ds;
	hive_session_t *s;
	hive_stream_t *st;
	send_capture_t cap;
	frame_hdr_t hdr;
	uint32_t sid;

	memset(&cap, 0, sizeof(cap));
	s = new_client_send_session();

	nva[0] = (hive_nv_t){n_method, v_post,
	    sizeof(n_method) - 1u, sizeof(v_post) - 1u, 0u};
	nva[1] = (hive_nv_t){n_path, v_path,
	    sizeof(n_path) - 1u, sizeof(v_path) - 1u, 0u};
	nva[2] = (hive_nv_t){n_scheme, v_http,
	    sizeof(n_scheme) - 1u, sizeof(v_http) - 1u, 0u};

	ds.read_callback = resp_read_copy_eof_cb;
	ds.ptr = NULL;

	ASSERT(hive_submit_request(s, nva, 3u, &ds, &sid) == HIVE_OK);
	ASSERT(sid == 1u);

	st = stream_lookup(s, sid);
	ASSERT(st != NULL);
	ASSERT(st->data_source.read_callback == resp_read_copy_eof_cb);

	s->callbacks.send = send_cb_capture;
	s->user_data = &cap;
	ASSERT(hive_session_send(s) == HIVE_OK);
	ASSERT(cap.calls == 1);
	ASSERT(cap.iovcnt == 3);

	frame_hdr_parse(cap.iov[0].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_HEADERS);
	ASSERT(hdr.stream_id == sid);
	ASSERT((hdr.flags & HIVE_FLAG_END_HEADERS) != 0u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) == 0u);

	frame_hdr_parse(cap.iov[1].iov_base, &hdr);
	ASSERT(hdr.type == HIVE_FRAME_DATA);
	ASSERT(hdr.stream_id == sid);
	ASSERT(hdr.length == 5u);
	ASSERT((hdr.flags & HIVE_FLAG_END_STREAM) != 0u);
	ASSERT(cap.iov[2].iov_len == 5u);
	ASSERT(memcmp(cap.iov[2].iov_base, "hello", 5u) == 0);

	hive_session_free(s);
	return 1;
}

int
test_stream_get_state(void)
{
	static const uint8_t n_etag[] = "etag";
	static const uint8_t v_etag[] = "abc";
	hive_nv_t nva[1];
	hive_session_t *s;

	s = new_server_send_session();

	ASSERT(hive_stream_get_state(s, 1u) == HIVE_STREAM_IDLE);
	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(hive_stream_get_state(s, 1u) == HIVE_STREAM_OPEN);

	nva[0].name = n_etag;
	nva[0].value = v_etag;
	nva[0].name_len = sizeof(n_etag) - 1u;
	nva[0].value_len = sizeof(v_etag) - 1u;
	nva[0].flags = 0u;
	ASSERT(hive_submit_trailers(s, 1u, nva, 1u) == HIVE_OK);
	ASSERT(hive_stream_get_state(s, 1u) == HIVE_STREAM_HALF_CLOSED_LOCAL);

	hive_session_free(s);
	return 1;
}

int
test_stream_user_data(void)
{
	hive_session_t *s;
	hive_stream_t *st;
	uint32_t marker;

	marker = 0x12345678u;
	s = new_server_send_session();

	ASSERT(hive_stream_set_user_data(s, 1u, &marker) ==
	    HIVE_ERR_STREAM_CLOSED);
	ASSERT(hive_stream_get_user_data(s, 1u) == NULL);

	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(hive_stream_set_user_data(s, 1u, &marker) == HIVE_OK);
	ASSERT(hive_stream_get_user_data(s, 1u) == &marker);

	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	stream_close(s, st);

	ASSERT(hive_stream_get_user_data(s, 1u) == NULL);
	ASSERT(hive_stream_set_user_data(s, 1u, &marker) ==
	    HIVE_ERR_STREAM_CLOSED);

	hive_session_free(s);
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

int
test_session_server_new_null_alloc(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	const uint8_t *p;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->role == HIVE_ROLE_SERVER);
	ASSERT(s->recv_state == RECV_CLIENT_PREFACE);
	ASSERT(hive_session_want_write(s) == 1);
	ASSERT(s->send_iov_count == 1);
	ASSERT(s->send_iov[0].iov_len >= 9u);

	p = s->send_iov[0].iov_base;
	ASSERT(p[3] == HIVE_FRAME_SETTINGS);
	ASSERT(p[4] == 0u);
	ASSERT((p[5] & 0x7fu) == 0u);
	ASSERT(p[6] == 0u);
	ASSERT(p[7] == 0u);
	ASSERT(p[8] == 0u);

	hive_session_free(s);
	return 1;
}

int
test_session_client_new_null_alloc(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	const uint8_t *p;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_client_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->role == HIVE_ROLE_CLIENT);
	ASSERT(s->recv_state == RECV_SERVER_PREFACE);
	ASSERT(hive_session_want_write(s) == 1);
	ASSERT(s->send_iov_count == 2);
	ASSERT(s->send_iov[0].iov_len == sizeof(test_client_preface_magic));
	ASSERT(memcmp(s->send_iov[0].iov_base, test_client_preface_magic,
	    sizeof(test_client_preface_magic)) == 0);

	p = s->send_iov[1].iov_base;
	ASSERT(s->send_iov[1].iov_len >= 9u);
	ASSERT(p[3] == HIVE_FRAME_SETTINGS);
	ASSERT(p[4] == 0u);
	ASSERT((p[5] & 0x7fu) == 0u);
	ASSERT(p[6] == 0u);
	ASSERT(p[7] == 0u);
	ASSERT(p[8] == 0u);

	hive_session_free(s);
	return 1;
}

int
test_h2c_upgrade_settings_applied(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t settings_payload[12];

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	settings_payload[0] = 0x00u;
	settings_payload[1] = (uint8_t)HIVE_SETTINGS_HEADER_TABLE_SIZE;
	settings_payload[2] = 0x00u;
	settings_payload[3] = 0x00u;
	settings_payload[4] = 0x04u;
	settings_payload[5] = 0x00u; /* 1024 */
	settings_payload[6] = 0x00u;
	settings_payload[7] = (uint8_t)HIVE_SETTINGS_MAX_FRAME_SIZE;
	settings_payload[8] = 0x00u;
	settings_payload[9] = 0x00u;
	settings_payload[10] = 0x80u;
	settings_payload[11] = 0x00u; /* 32768 */

	s = hive_session_server_upgrade(
	    NULL, NULL, &cb, NULL, settings_payload, sizeof(settings_payload));
	ASSERT(s != NULL);
	ASSERT(s->recv_state == RECV_FRAME_HEADER);
	ASSERT(s->remote_settings.header_table_size == 1024u);
	ASSERT(s->remote_settings.max_frame_size == 32768u);
	ASSERT(s->enc_table.has_pending == 1);
	ASSERT(s->enc_table.pending_min == 1024u);
	ASSERT(s->enc_table.pending_max == 1024u);

	hive_session_free(s);
	return 1;
}

int
test_h2c_upgrade_stream1_open(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	static const uint8_t empty_settings = 0u;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_upgrade(
	    NULL, NULL, &cb, NULL, &empty_settings, 0u);
	ASSERT(s != NULL);
	ASSERT(hive_stream_get_state(s, 1u) == HIVE_STREAM_HALF_CLOSED_REMOTE);
	ASSERT(s->last_stream_id_remote == 1u);
	ASSERT(s->send_iov_count == 1);
	ASSERT(s->send_iov[0].iov_len >= 9u);
	ASSERT(((const uint8_t *)s->send_iov[0].iov_base)[3] ==
	    HIVE_FRAME_SETTINGS);

	hive_session_free(s);
	return 1;
}

int
test_h2c_feed_upgrade_headers_fires_callbacks(void)
{
	hive_callbacks_t cb;
	headers_capture_t cap;
	hive_session_t *s;
	hive_nv_t nva[4];
	hive_options_t *opt;
	headers_capture_t cap_limit;
	hive_session_t *s_limit;
	static const uint8_t empty_settings = 0u;
	static const uint8_t n_method[] = ":method";
	static const uint8_t v_get[] = "GET";
	static const uint8_t n_scheme[] = ":scheme";
	static const uint8_t v_http[] = "http";
	static const uint8_t n_path[] = ":path";
	static const uint8_t v_path[] = "/";
	static const uint8_t n_authority[] = ":authority";
	static const uint8_t v_authority[] = "www.example.com";

	memset(&cap, 0, sizeof(cap));
	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	cb.on_begin_headers = on_begin_headers_capture;
	cb.on_header = on_header_capture;
	cb.on_headers_complete = on_headers_complete_capture;

	s = hive_session_server_upgrade(
	    NULL, NULL, &cb, &cap, &empty_settings, 0u);
	ASSERT(s != NULL);

	nva[0].name = n_method;
	nva[0].value = v_get;
	nva[0].name_len = sizeof(n_method) - 1u;
	nva[0].value_len = sizeof(v_get) - 1u;
	nva[0].flags = 0u;
	nva[1].name = n_scheme;
	nva[1].value = v_http;
	nva[1].name_len = sizeof(n_scheme) - 1u;
	nva[1].value_len = sizeof(v_http) - 1u;
	nva[1].flags = 0u;
	nva[2].name = n_path;
	nva[2].value = v_path;
	nva[2].name_len = sizeof(n_path) - 1u;
	nva[2].value_len = sizeof(v_path) - 1u;
	nva[2].flags = 0u;
	nva[3].name = n_authority;
	nva[3].value = v_authority;
	nva[3].name_len = sizeof(n_authority) - 1u;
	nva[3].value_len = sizeof(v_authority) - 1u;
	nva[3].flags = 0u;

	ASSERT(hive_session_feed_upgrade_headers(s, nva, 4u, 1) == HIVE_OK);
	ASSERT(cap.begin_count == 1);
	ASSERT(cap.header_count == 4);
	ASSERT(cap.complete_count == 1);
	ASSERT(cap.stream_id == 1u);
	ASSERT(cap.headers_complete_flags == HIVE_FLAG_END_STREAM);
	ASSERT(cap.begin_seq < cap.first_header_seq);
	ASSERT(cap.last_header_seq < cap.complete_seq);
	ASSERT(cap.saw_method == 1);
	ASSERT(cap.saw_scheme == 1);
	ASSERT(cap.saw_path == 1);
	ASSERT(cap.saw_authority == 1);

	hive_session_free(s);

	opt = hive_options_new();
	ASSERT(opt != NULL);
	ASSERT(hive_options_set_max_header_count(opt, 2u) == HIVE_OK);
	memset(&cap_limit, 0, sizeof(cap_limit));
	s_limit = hive_session_server_upgrade(
	    NULL, opt, &cb, &cap_limit, &empty_settings, 0u);
	hive_options_free(opt);
	ASSERT(s_limit != NULL);
	ASSERT(hive_session_feed_upgrade_headers(s_limit, nva, 3u, 1) ==
	    HIVE_ERR_PROTOCOL);
	hive_session_free(s_limit);

	return 1;
}

int
test_session_new_custom_alloc(void)
{
	hive_callbacks_t cb;
	hive_mem_t mem;
	alloc_track_t st;
	hive_session_t *s;
	int ret;
	size_t before;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	memset(&st, 0, sizeof(st));
	mem = track_mem(&st);

	s = hive_session_server_new(&mem, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(st.alloc_calls == 12u);

	before = st.alloc_calls;
	ret = hive_session_send(s);
	ASSERT(ret == HIVE_OK);
	ASSERT(st.alloc_calls == before);

	hive_session_free(s);
	ASSERT(st.outstanding == 0u);
	return 1;
}

int
test_session_free_all_allocations(void)
{
	hive_callbacks_t cb;
	hive_mem_t mem;
	alloc_track_t st;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	memset(&st, 0, sizeof(st));
	mem = track_mem(&st);

	s = hive_session_server_new(&mem, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(st.outstanding > 0u);

	hive_session_free(s);
	ASSERT(st.outstanding == 0u);
	ASSERT(st.free_calls == st.alloc_calls);
	return 1;
}

int
test_session_new_alloc_failure(void)
{
	hive_callbacks_t cb;
	hive_mem_t mem;
	alloc_track_t st;
	hive_session_t *s;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	memset(&st, 0, sizeof(st));
	st.fail_after = 3;
	mem = track_mem(&st);

	s = hive_session_server_new(&mem, NULL, &cb, NULL);
	ASSERT(s == NULL);
	ASSERT(st.outstanding == 0u);
	ASSERT(st.free_calls == 2u);
	return 1;
}

int
test_options_set_max_concurrent(void)
{
	hive_callbacks_t cb;
	hive_options_t *opt;
	hive_session_t *s;
	int ret;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	opt = hive_options_new();
	ASSERT(opt != NULL);
	ret = hive_options_set_max_concurrent_streams(opt, 50u);
	ASSERT(ret == HIVE_OK);

	s = hive_session_server_new(NULL, opt, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->opt_max_concurrent_streams == 50u);
	ASSERT(s->stream_free_top == 50u);
	ASSERT(s->stream_free_stack[0] == 0u);
	ASSERT(s->stream_free_stack[49] == 49u);
	ASSERT(s->stream_hash_mask == 127u);

	hive_session_free(s);
	hive_options_free(opt);
	return 1;
}

int
test_stream_open_lookup_close(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	hive_stream_t *st;
	int ret;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	ret = stream_open(s, 1u, HIVE_STREAM_OPEN);
	ASSERT(ret == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	ASSERT(st->stream_id == 1u);
	ASSERT(st->state == HIVE_STREAM_OPEN);

	stream_close(s, st);
	ASSERT(stream_lookup(s, 1u) == NULL);
	ASSERT(s->stream_open_count == 0u);

	hive_session_free(s);
	return 1;
}

int
test_stream_hash_collision(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	hive_stream_t *a;
	hive_stream_t *b;
	uint32_t id1;
	uint32_t id2;
	uint32_t x;
	uint32_t y;
	int found;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	found = 0;
	id1 = 1u;
	id2 = 3u;
	for (x = 1u; x < 4096u && !found; x += 2u) {
		for (y = x + 2u; y < 4096u; y += 2u) {
			if (stream_hash_fn(x, s->stream_hash_mask) ==
			    stream_hash_fn(y, s->stream_hash_mask)) {
				id1 = x;
				id2 = y;
				found = 1;
				break;
			}
		}
	}
	ASSERT(found == 1);

	ASSERT(stream_open(s, id1, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(stream_open(s, id2, HIVE_STREAM_OPEN) == HIVE_OK);
	a = stream_lookup(s, id1);
	b = stream_lookup(s, id2);
	ASSERT(a != NULL);
	ASSERT(b != NULL);
	ASSERT(a->stream_id == id1);
	ASSERT(b->stream_id == id2);

	hive_session_free(s);
	return 1;
}

int
test_stream_free_stack(void)
{
	hive_callbacks_t cb;
	hive_options_t *opt;
	hive_session_t *s;
	hive_stream_t *st;
	int ret;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	opt = hive_options_new();
	ASSERT(opt != NULL);
	ASSERT(hive_options_set_max_concurrent_streams(opt, 2u) == HIVE_OK);

	s = hive_session_server_new(NULL, opt, &cb, NULL);
	ASSERT(s != NULL);

	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(stream_open(s, 3u, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(s->stream_open_count == 2u);
	ASSERT(s->stream_free_top == 0u);

	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	stream_close(s, st);
	ASSERT(s->stream_open_count == 1u);
	ASSERT(s->stream_free_top == 1u);

	ret = stream_open(s, 5u, HIVE_STREAM_OPEN);
	ASSERT(ret == HIVE_OK);
	ASSERT(s->stream_open_count == 2u);
	ASSERT(stream_lookup(s, 5u) != NULL);

	hive_session_free(s);
	hive_options_free(opt);
	return 1;
}

int
test_stream_compaction(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	hive_stream_t *st;
	uint32_t sid;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	for (sid = 1u; sid <= 129u; sid += 2u)
		ASSERT(stream_open(s, sid, HIVE_STREAM_OPEN) == HIVE_OK);

	for (sid = 1u; sid <= 129u; sid += 2u) {
		st = stream_lookup(s, sid);
		ASSERT(st != NULL);
		stream_close(s, st);
	}

	ASSERT(s->stream_open_count == 0u);
	ASSERT(s->tombstone_count == 0u);
	ASSERT(s->closes_since_compact == 0u);

	hive_session_free(s);
	return 1;
}

int
test_recv_headers_opens_new_stream(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t frame[9];

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	frame_hdr_write_at(frame, 0u, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS,
	    1u);
	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	ASSERT(stream_lookup(s, 1u) != NULL);
	ASSERT(s->stream_open_count == 1u);
	ASSERT(s->peer_stream_open_count == 1u);
	ASSERT(s->last_stream_id_remote == 1u);

	hive_session_free(s);
	return 1;
}

int
test_recv_get_request_headers(void)
{
	hive_callbacks_t cb;
	headers_capture_t cap;
	hive_session_t *s;
	uint8_t frame[29];
	uint8_t hpack_get[20];

	memset(&cb, 0, sizeof(cb));
	memset(&cap, 0, sizeof(cap));
	cb.send = send_cb_full;
	cb.on_begin_headers = on_begin_headers_capture;
	cb.on_header = on_header_capture;
	cb.on_headers_complete = on_headers_complete_capture;

	s = hive_session_server_new(NULL, NULL, &cb, &cap);
	ASSERT(s != NULL);

	/* Test only HEADERS processing path; skip client preface setup. */
	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	/* RFC 7541 C.3 request block: :method GET, :scheme http,
	 * :path /, :authority www.example.com. */
	hpack_get[0] = 0x82;
	hpack_get[1] = 0x86;
	hpack_get[2] = 0x84;
	hpack_get[3] = 0x41;
	hpack_get[4] = 0x0f;
	memcpy(hpack_get + 5, "www.example.com", 15u);

	frame_hdr_write_at(frame,
	                  sizeof(hpack_get),
	                  HIVE_FRAME_HEADERS,
	                  HIVE_FLAG_END_HEADERS | HIVE_FLAG_END_STREAM,
	                  1u);
	memcpy(frame + 9, hpack_get, sizeof(hpack_get));

	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	ASSERT(stream_lookup(s, 1u) != NULL);
	ASSERT(cap.begin_count == 1);
	ASSERT(cap.header_count == 4);
	ASSERT(cap.complete_count == 1);
	ASSERT(cap.begin_seq < cap.first_header_seq);
	ASSERT(cap.last_header_seq < cap.complete_seq);
	ASSERT(cap.stream_id == 1u);
	ASSERT(cap.headers_complete_flags == 1u);
	ASSERT(cap.saw_method == 1);
	ASSERT(cap.saw_scheme == 1);
	ASSERT(cap.saw_path == 1);
	ASSERT(cap.saw_authority == 1);

	hive_session_free(s);
	return 1;
}

int
test_stream_id_monotonicity(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t frame[9];

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;
	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	s->recv_state = RECV_FRAME_HEADER;
	s->preface_count = 0;

	frame_hdr_write_at(frame, 0u, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS,
	    5u);
	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	ASSERT(stream_lookup(s, 5u) != NULL);

	frame_hdr_write_at(frame, 0u, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS,
	    3u);
	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_settings_recv_and_ack(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	const uint8_t *ack;
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	settings_param_write(payload, HIVE_SETTINGS_INITIAL_WINDOW_SIZE, 131072u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(s->remote_settings.initial_window_size == 131072u);
	ASSERT(s->inbound_settings_count == 0u);
	ASSERT(s->send_iov_count == 1);
	ASSERT(cap.settings_count == 1);

	ack = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 9u);
	ASSERT(ack[3] == HIVE_FRAME_SETTINGS);
	ASSERT(ack[4] == HIVE_FLAG_ACK);
	ASSERT(ack[5] == 0u);
	ASSERT(ack[6] == 0u);
	ASSERT(ack[7] == 0u);
	ASSERT(ack[8] == 0u);

	hive_session_free(s);
	return 1;
}

int
test_settings_recv_ack(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t frame[9];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	s->pending_count = 1;
	s->pending_head = 0;
	n = build_settings_frame(frame, HIVE_FLAG_ACK, NULL, 0u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(s->pending_count == 0u);
	ASSERT(s->pending_head == 1u);
	ASSERT(cap.settings_ack_count == 1);


	hive_session_free(s);
	return 1;
}

int
test_settings_invalid_window_size(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	settings_param_write(
	    payload, HIVE_SETTINGS_INITIAL_WINDOW_SIZE, 0x80000000u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_settings_invalid_frame_size(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	settings_param_write(payload, HIVE_SETTINGS_MAX_FRAME_SIZE, 16383u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_settings_header_table_size_updates_encoder(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	settings_param_write(payload, HIVE_SETTINGS_HEADER_TABLE_SIZE, 512u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(s->remote_settings.header_table_size == 512u);
	ASSERT(s->enc_table.pending_max == 512u);
	ASSERT(s->enc_table.pending_min == 512u);
	ASSERT(s->enc_table.has_pending == 1u);

	hive_session_free(s);
	return 1;
}

int
test_settings_header_table_size_pending_min(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	settings_param_write(payload, HIVE_SETTINGS_HEADER_TABLE_SIZE, 2048u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	settings_param_write(payload, HIVE_SETTINGS_HEADER_TABLE_SIZE, 3072u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(s->enc_table.pending_min == 2048u);
	ASSERT(s->enc_table.pending_max == 3072u);
	ASSERT(s->enc_table.has_pending == 1u);

	hive_session_free(s);
	return 1;
}

int
test_settings_initial_window_retroactive_adjust(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	hive_stream_t *st1;
	hive_stream_t *st3;
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	ASSERT(stream_open(s, 3u, HIVE_STREAM_OPEN) == HIVE_OK);
	st1 = stream_lookup(s, 1u);
	st3 = stream_lookup(s, 3u);
	ASSERT(st1 != NULL);
	ASSERT(st3 != NULL);
	ASSERT(st1->send_window == 65535);
	ASSERT(st3->send_window == 65535);

	settings_param_write(payload, HIVE_SETTINGS_INITIAL_WINDOW_SIZE, 70000u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);

	ASSERT(st1->send_window == 70000);
	ASSERT(st3->send_window == 70000);

	hive_session_free(s);
	return 1;
}

int
test_settings_initial_window_retroactive_overflow(void)
{
	settings_capture_t cap;
	hive_session_t *s;
	uint8_t payload[6];
	uint8_t frame[15];
	hive_stream_t *st;
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_recv_session(&cap);

	ASSERT(stream_open(s, 1u, HIVE_STREAM_OPEN) == HIVE_OK);
	st = stream_lookup(s, 1u);
	ASSERT(st != NULL);
	st->send_window = 0x7fffffff;

	settings_param_write(
	    payload, HIVE_SETTINGS_INITIAL_WINDOW_SIZE, 2147483647u);
	n = build_settings_frame(frame, 0u, payload, sizeof(payload));
	ASSERT(hive_session_recv(s, frame, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_FLOW_CONTROL);
	ASSERT(s->last_h2_err == HIVE_H2_FLOW_CONTROL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_server_preface_valid(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t in[24 + 9];
	size_t n;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->recv_state == RECV_CLIENT_PREFACE);

	memcpy(in, test_client_preface_magic, sizeof(test_client_preface_magic));
	n = build_settings_frame(in + sizeof(test_client_preface_magic),
	    0u, NULL, 0u);

	ASSERT(hive_session_recv(s, in, sizeof(test_client_preface_magic) + n) ==
	    (ssize_t)(sizeof(test_client_preface_magic) + n));
	ASSERT(s->recv_state == RECV_FRAME_HEADER);
	ASSERT(s->preface_count == 0u);

	hive_session_free(s);
	return 1;
}

int
test_server_preface_invalid(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t in[24];

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_server_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);

	memcpy(in, test_client_preface_magic, sizeof(test_client_preface_magic));
	in[0] = 'X';

	ASSERT(hive_session_recv(s, in, sizeof(in)) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_client_preface_first_frame_not_settings(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t in[17];

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_client_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->recv_state == RECV_SERVER_PREFACE);

	frame_hdr_write_at(in, 8u, HIVE_FRAME_PING, 0u, 0u);
	memset(in + 9, 0, 8);

	ASSERT(hive_session_recv(s, in, sizeof(in)) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_client_preface_settings_with_ack(void)
{
	hive_callbacks_t cb;
	hive_session_t *s;
	uint8_t in[9];
	size_t n;

	memset(&cb, 0, sizeof(cb));
	cb.send = send_cb_full;

	s = hive_session_client_new(NULL, NULL, &cb, NULL);
	ASSERT(s != NULL);
	ASSERT(s->recv_state == RECV_SERVER_PREFACE);

	n = build_settings_frame(in, HIVE_FLAG_ACK, NULL, 0u);
	ASSERT(n == sizeof(in));

	ASSERT(hive_session_recv(s, in, n) == -1);
	ASSERT(s->last_err == HIVE_ERR_PROTOCOL);
	ASSERT(s->last_h2_err == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

int
test_on_settings_ack_fires(void)
{
	callback_capture_t cap;
	hive_session_t *s;
	uint8_t frame[9];
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_callback_session(&cap, 0u);

	s->pending_count = 1;
	s->pending_head = 0;
	n = build_settings_frame(frame, HIVE_FLAG_ACK, NULL, 0u);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.settings_ack_count == 1);

	hive_session_free(s);
	return 1;
}

int
test_on_goaway_fires(void)
{
	callback_capture_t cap;
	hive_session_t *s;
	uint8_t frame[64];
	static const uint8_t dbg[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_callback_session(&cap, 0u);

	n = build_goaway_frame(
	    frame, 3u, HIVE_H2_PROTOCOL_ERROR, dbg, sizeof(dbg));
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.goaway_count == 1);
	ASSERT(cap.goaway_last_stream_id == 3u);
	ASSERT(cap.goaway_error_code == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(cap.goaway_debug_data == s->reassembly_buf);
	ASSERT(cap.goaway_debug_len == sizeof(dbg));
	ASSERT(memcmp(cap.goaway_debug_data, dbg, sizeof(dbg)) == 0);

	hive_session_free(s);
	return 1;
}

int
test_on_ping_fires_when_no_auto_ack(void)
{
	callback_capture_t cap;
	hive_session_t *s;
	uint8_t frame[17];
	static const uint8_t opaque[8] =
	    {0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u};
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_callback_session(&cap, 1u);

	n = build_ping_frame(frame, 0u, opaque);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.ping_count == 1);
	ASSERT(cap.ping_ack_count == 0);
	ASSERT(memcmp(cap.ping_opaque, opaque, sizeof(opaque)) == 0);
	ASSERT(s->send_iov_count == 0);

	hive_session_free(s);
	return 1;
}

int
test_on_ping_ack_fires(void)
{
	callback_capture_t cap;
	hive_session_t *s;
	uint8_t frame[17];
	static const uint8_t opaque[8] =
	    {0xa1u, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u, 0xa8u};
	size_t n;

	memset(&cap, 0, sizeof(cap));
	s = new_server_callback_session(&cap, 0u);

	n = build_ping_frame(frame, HIVE_FLAG_ACK, opaque);
	ASSERT(hive_session_recv(s, frame, n) == (ssize_t)n);
	ASSERT(cap.ping_count == 0);
	ASSERT(cap.ping_ack_count == 1);
	ASSERT(memcmp(cap.ping_ack_opaque, opaque, sizeof(opaque)) == 0);

	hive_session_free(s);
	return 1;
}

int
test_on_connection_error_fires_before_goaway(void)
{
	callback_capture_t cap;
	hive_session_t *s;
	uint8_t frame[9];
	const uint8_t *out;

	memset(&cap, 0, sizeof(cap));
	s = new_server_callback_session(&cap, 0u);

	frame_hdr_write_at(frame, 0u, HIVE_FRAME_DATA, 0u, 0u);
	ASSERT(hive_session_recv(s, frame, sizeof(frame)) == -1);
	ASSERT(cap.connection_error_count == 1);
	ASSERT(cap.connection_error_hive_err == HIVE_ERR_PROTOCOL);
	ASSERT(cap.connection_error_h2_err == HIVE_H2_PROTOCOL_ERROR);
	ASSERT(cap.connection_error_saw_goaway_queued == 0);

	ASSERT(s->send_iov_count == 1);
	out = s->send_iov[0].iov_base;
	ASSERT(s->send_iov[0].iov_len == 17u);
	ASSERT(out[3] == HIVE_FRAME_GOAWAY);
	ASSERT(out[4] == 0u);
	ASSERT(out[5] == 0u && out[6] == 0u && out[7] == 0u && out[8] == 0u);
	ASSERT(read_u32_be(out + 9) == 0u);
	ASSERT(read_u32_be(out + 13) == HIVE_H2_PROTOCOL_ERROR);

	hive_session_free(s);
	return 1;
}

