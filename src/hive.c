/*
 * hive.c — session lifecycle and public API entry points
 *
 * See ARCHITECTURE.md §1 and §9 for full documentation.
 * See CODING_STANDARDS.md §2.1 — this is the ONLY file in src/ that may
 * call malloc/free/calloc/realloc directly (in the NULL-allocator shim).
 */

#include <stdlib.h>

#include "../include/hive.h"
#include "hive_frame.h"
#include "hive_internal.h"

/*
 * NULL-allocator shim.
 * This is the ONLY permitted direct use of malloc/free/calloc/realloc
 * in the library source. All other src/ files use s->mem.* exclusively.
 * See CODING_STANDARDS.md §2.1.
 */
static void *
null_alloc_malloc(size_t size, void *ctx)
{
	(void)ctx;
	return malloc(size);
}

static void
null_alloc_free(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);
}

static void *
null_alloc_calloc(size_t nmemb, size_t size, void *ctx)
{
	(void)ctx;
	return calloc(nmemb, size);
}

static void *
null_alloc_realloc(void *ptr, size_t size, void *ctx)
{
	(void)ctx;
	return realloc(ptr, size);
}

static const hive_mem_t null_allocator = {
    null_alloc_malloc,
    null_alloc_free,
    null_alloc_calloc,
    null_alloc_realloc,
    NULL,
};

/*
 * Stub implementations — Phase 1 skeleton only.
 * Full implementations land in Phase 4 onward.
 */

hive_options_t *
hive_options_new(void)
{
	return NULL;
}

void
hive_options_free(hive_options_t *opt)
{
	(void)opt;
}

hive_session_t *
hive_session_server_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	(void)null_allocator;
	return NULL;
}

hive_session_t *
hive_session_client_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	return NULL;
}

hive_session_t *
hive_session_server_upgrade(const hive_mem_t *mem,
                            const hive_options_t *opt,
                            const hive_callbacks_t *callbacks,
                            void *user_data,
                            const char *http2_settings_b64,
                            uint32_t upgraded_stream_id)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	(void)http2_settings_b64;
	(void)upgraded_stream_id;
	return NULL;
}

void
hive_session_free(hive_session_t *session)
{
	(void)session;
}

ssize_t
hive_session_recv(hive_session_t *session, const uint8_t *data, size_t len)
{
	if (session == NULL) {
		return -1;
	}
	return frame_recv_process(session, data, len);
}

int
hive_session_send(hive_session_t *session)
{
	(void)session;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_session_want_read(hive_session_t *session)
{
	(void)session;
	return 0;
}

int
hive_session_want_write(hive_session_t *session)
{
	(void)session;
	return 0;
}

int
hive_submit_response(hive_session_t *session,
                     uint32_t stream_id,
                     const hive_nv_t *nva,
                     size_t nvlen,
                     hive_data_source_t *data_source)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	(void)data_source;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_trailers(hive_session_t *session,
                     uint32_t stream_id,
                     const hive_nv_t *nva,
                     size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_interim_response(hive_session_t *session,
                             uint32_t stream_id,
                             const hive_nv_t *nva,
                             size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_push_promise(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen,
                         uint32_t *promised_stream_id_out)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	(void)promised_stream_id_out;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_request(hive_session_t *session,
                    const hive_nv_t *nva,
                    size_t nvlen,
                    hive_data_source_t *data_source,
                    uint32_t *stream_id_out)
{
	(void)session;
	(void)nva;
	(void)nvlen;
	(void)data_source;
	(void)stream_id_out;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_rst_stream(hive_session_t *session,
                       uint32_t stream_id,
                       uint32_t error_code)
{
	(void)session;
	(void)stream_id;
	(void)error_code;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_goaway_prepare(hive_session_t *session)
{
	(void)session;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_goaway_final(hive_session_t *session,
                         uint32_t error_code,
                         const uint8_t *debug_data,
                         size_t debug_len)
{
	(void)session;
	(void)error_code;
	(void)debug_data;
	(void)debug_len;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_ping(hive_session_t *session, const uint8_t opaque[8])
{
	(void)session;
	(void)opaque;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_ping_ack(hive_session_t *session, const uint8_t opaque[8])
{
	(void)session;
	(void)opaque;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_session_feed_upgrade_headers(hive_session_t *session,
                                  uint32_t stream_id,
                                  const hive_nv_t *nva,
                                  size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_buf_retain(hive_session_t *session, hive_buf_t *buf)
{
	(void)session;
	(void)buf;
	return HIVE_ERR_SESSION_CLOSED;
}

void
hive_buf_free(hive_session_t *session, hive_buf_t *buf)
{
	(void)session;
	(void)buf;
}

int32_t
hive_session_get_remote_window_size(hive_session_t *session)
{
	(void)session;
	return 0;
}

hive_settings_t
hive_session_get_local_settings(hive_session_t *session)
{
	hive_settings_t s = {0, 0, 0, 0, 0, 0};
	(void)session;
	return s;
}

hive_settings_t
hive_session_get_remote_settings(hive_session_t *session)
{
	hive_settings_t s = {0, 0, 0, 0, 0, 0};
	(void)session;
	return s;
}

int
hive_stream_get_state(hive_session_t *session, uint32_t stream_id)
{
	(void)session;
	(void)stream_id;
	return -1;
}

int
hive_stream_set_user_data(hive_session_t *session,
                          uint32_t stream_id,
                          void *user_data)
{
	(void)session;
	(void)stream_id;
	(void)user_data;
	return HIVE_ERR_SESSION_CLOSED;
}

void *
hive_stream_get_user_data(hive_session_t *session, uint32_t stream_id)
{
	(void)session;
	(void)stream_id;
	return NULL;
}

int
hive_options_set_header_table_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_enable_push(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_concurrent_streams(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_initial_window_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_frame_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_header_list_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_continuation_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_send_iov(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_settings_flood_limit(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_rst_flood_threshold(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_rst_flood_window_secs(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

hive_hpack_encoder_t *
hive_hpack_encoder_new(const hive_mem_t *mem, uint32_t max_table_size)
{
	(void)mem;
	(void)max_table_size;
	return NULL;
}

void
hive_hpack_encoder_free(hive_hpack_encoder_t *enc)
{
	(void)enc;
}

int
hive_hpack_encode(hive_hpack_encoder_t *enc,
                  const hive_nv_t *nva,
                  size_t nvlen,
                  uint8_t *out,
                  size_t out_cap,
                  size_t *out_len)
{
	(void)enc;
	(void)nva;
	(void)nvlen;
	(void)out;
	(void)out_cap;
	(void)out_len;
	return HIVE_ERR_SESSION_CLOSED;
}

hive_hpack_decoder_t *
hive_hpack_decoder_new(const hive_mem_t *mem, uint32_t max_table_size)
{
	(void)mem;
	(void)max_table_size;
	return NULL;
}

void
hive_hpack_decoder_free(hive_hpack_decoder_t *dec)
{
	(void)dec;
}

int
hive_hpack_decode(hive_hpack_decoder_t *dec,
                  const uint8_t *data,
                  size_t len,
                  int (*on_header)(hive_buf_t *name,
                                   hive_buf_t *value,
                                   void *ud),
                  void *user_data)
{
	(void)dec;
	(void)data;
	(void)len;
	(void)on_header;
	(void)user_data;
	return HIVE_ERR_SESSION_CLOSED;
}
