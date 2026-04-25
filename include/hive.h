/*
 * hive.h — HTTP/2 library public API
 *
 * Create a session with hive_session_server_new() or hive_session_client_new().
 * Feed incoming bytes with hive_session_recv().
 * Submit responses with hive_submit_response().
 * Drain outgoing frames with hive_session_send().
 *
 * See ARCHITECTURE.md for full protocol and data flow documentation.
 * See README.md for a quickstart integration example.
 */

#ifndef HIVE_H
#define HIVE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/*
 * Opaque session handle. Created by hive_session_server_new() or
 * hive_session_client_new(). All library state is contained within.
 * Not thread-safe — one session, one thread.
 */
typedef struct hive_session hive_session_t;

/*
 * Opaque options handle. Created by hive_options_new(), passed to
 * session constructors, and freed by hive_options_free().
 */
typedef struct hive_options hive_options_t;

/*
 * Buffer handle for header name/value delivery.
 * Valid only within the on_header callback unless hive_buf_retain() is called.
 * See ARCHITECTURE.md §9.3 and CODING_STANDARDS.md §3.5.
 */
typedef struct hive_buf {
	const uint8_t *data;
	size_t len;
	uint8_t flags;
	uint8_t _pad[7];
} hive_buf_t;

/* hive_buf_t flags */
#define HIVE_BUF_VALID 0x01u /* handle is currently valid */
#define HIVE_BUF_OWNED 0x02u /* data is arena-allocated (retained) */

/*
 * Header name/value pair — used for submit functions and the HPACK
 * static table. See ARCHITECTURE.md §9.3.
 *
 * `flags` carries per-pair indexing hints (currently only
 * HIVE_NV_FLAG_NO_INDEX). All static-table entries have flags == 0.
 *
 * NOTE: Field names are `name_len`/`value_len` (not `namelen`/`valuelen`
 * as the architecture pseudocode shows) — the snake-case form matches
 * the rest of the public surface (`hive_buf_t.len`, etc.).
 */
typedef struct hive_nv {
	const uint8_t *name;
	const uint8_t *value;
	size_t name_len;
	size_t value_len;
	uint8_t flags;
	/* 7 bytes of trailing padding inserted by the compiler for
	 * size_t alignment on 64-bit; do not add fields after `flags`
	 * without re-checking sizeof in src/hive_internal.h (Phase 1.5). */
} hive_nv_t;

/* hive_nv_t flags */
#define HIVE_NV_FLAG_NO_INDEX                                                  \
	0x01u /* never-indexed: do not store in HPACK table */

/*
 * Allocator interface. Inject a custom allocator at session creation.
 * Pass NULL for mem to use system malloc/free/calloc/realloc.
 * See ARCHITECTURE.md §2.1 and CODING_STANDARDS.md §2.
 */
typedef struct hive_mem {
	void *(*malloc)(size_t size, void *ctx);
	void (*free)(void *ptr, void *ctx);
	void *(*calloc)(size_t nmemb, size_t size, void *ctx);
	void *(*realloc)(void *ptr, size_t size, void *ctx);
	void *ctx;
} hive_mem_t;

/*
 * Data source for submit functions. The read_callback is called repeatedly
 * to drain body data into the send queue. See ARCHITECTURE.md §6.5.
 */
typedef struct hive_data_source hive_data_source_t;

typedef ssize_t (*hive_read_callback_t)(hive_session_t *session,
                                        uint32_t stream_id,
                                        uint8_t **buf,
                                        uint32_t flags,
                                        void *user_data);

struct hive_data_source {
	hive_read_callback_t read_callback;
	void *user_data;
};

/* hive_data_source flags (passed to read_callback, returned by it) */
#define HIVE_DATA_FLAG_EOF 0x01u /* this is the last chunk */
#define HIVE_DATA_FLAG_NO_COPY                                                 \
	0x02u /* zero-copy: redirect *buf to caller mem */

/* Session roles */
typedef enum {
	HIVE_ROLE_SERVER = 0,
	HIVE_ROLE_CLIENT = 1,
} hive_role_t;

/*
 * Return codes. HIVE_OK == 0. All error codes are negative.
 */
#define HIVE_OK 0
#define HIVE_ERR_NOMEM -1
#define HIVE_ERR_INVALID_ARG -2
#define HIVE_ERR_PROTOCOL -3
#define HIVE_ERR_COMPRESSION -4
#define HIVE_ERR_FLOW_CONTROL -5
#define HIVE_ERR_REFUSED_STREAM -6
#define HIVE_ERR_STREAM_CLOSED -7
#define HIVE_ERR_GOAWAY -8
#define HIVE_ERR_SESSION_CLOSED -9

/*
 * HTTP/2 wire error codes (for GOAWAY and RST_STREAM frames).
 * See RFC 9113 §7.
 * NOTE: HIVE_H2_NO_ERROR == 0x0 and HIVE_OK == 0 share the same value
 * but serve different purposes. Never mix them.
 */
#define HIVE_H2_NO_ERROR 0x0u
#define HIVE_H2_PROTOCOL_ERROR 0x1u
#define HIVE_H2_INTERNAL_ERROR 0x2u
#define HIVE_H2_FLOW_CONTROL_ERROR 0x3u
#define HIVE_H2_SETTINGS_TIMEOUT 0x4u
#define HIVE_H2_STREAM_CLOSED 0x5u
#define HIVE_H2_FRAME_SIZE_ERROR 0x6u
#define HIVE_H2_REFUSED_STREAM 0x7u
#define HIVE_H2_CANCEL 0x8u
#define HIVE_H2_COMPRESSION_ERROR 0x9u
#define HIVE_H2_CONNECT_ERROR 0xau
#define HIVE_H2_ENHANCE_YOUR_CALM 0xbu
#define HIVE_H2_INADEQUATE_SECURITY 0xcu
#define HIVE_H2_HTTP_1_1_REQUIRED 0xdu

/*
 * SETTINGS parameter identifiers. See RFC 9113 §6.5.2.
 */
#define HIVE_SETTINGS_HEADER_TABLE_SIZE 0x1u
#define HIVE_SETTINGS_ENABLE_PUSH 0x2u
#define HIVE_SETTINGS_MAX_CONCURRENT_STREAMS 0x3u
#define HIVE_SETTINGS_INITIAL_WINDOW_SIZE 0x4u
#define HIVE_SETTINGS_MAX_FRAME_SIZE 0x5u
#define HIVE_SETTINGS_MAX_HEADER_LIST_SIZE 0x6u

/*
 * SETTINGS values as a struct. Used for introspection.
 * See ARCHITECTURE.md §2.5.
 */
typedef struct hive_settings {
	uint32_t header_table_size;      /* HEADER_TABLE_SIZE */
	uint32_t enable_push;            /* ENABLE_PUSH */
	uint32_t max_concurrent_streams; /* MAX_CONCURRENT_STREAMS */
	uint32_t initial_window_size;    /* INITIAL_WINDOW_SIZE */
	uint32_t max_frame_size;         /* MAX_FRAME_SIZE */
	uint32_t max_header_list_size;   /* MAX_HEADER_LIST_SIZE */
} hive_settings_t;

/*
 * Callback set. All fields are optional (NULL = not registered).
 * See ARCHITECTURE.md §9.5 for full callback documentation.
 */
typedef struct hive_callbacks {
	/* Receive-side header callbacks */
	int (*on_begin_headers)(hive_session_t *session,
	                        uint32_t stream_id,
	                        void *user_data);

	int (*on_header)(hive_session_t *session,
	                 uint32_t stream_id,
	                 hive_buf_t *name,
	                 hive_buf_t *value,
	                 uint8_t flags,
	                 void *user_data);

	int (*on_headers_complete)(hive_session_t *session,
	                           uint32_t stream_id,
	                           uint8_t flags,
	                           void *user_data);

	/* Receive-side DATA callback */
	int (*on_data_chunk)(hive_session_t *session,
	                     uint32_t stream_id,
	                     const uint8_t *data,
	                     size_t len,
	                     uint8_t flags,
	                     void *user_data);

	/* Stream lifecycle */
	int (*on_stream_close)(hive_session_t *session,
	                       uint32_t stream_id,
	                       uint32_t error_code,
	                       void *user_data);

	/* Push promise (server push) */
	int (*on_push_promise)(hive_session_t *session,
	                       uint32_t stream_id,
	                       uint32_t promised_stream_id,
	                       void *user_data);

	/* Connection-level events */
	int (*on_settings)(hive_session_t *session, void *user_data);
	int (*on_settings_ack)(hive_session_t *session, void *user_data);

	int (*on_goaway)(hive_session_t *session,
	                 uint32_t last_stream_id,
	                 uint32_t error_code,
	                 const uint8_t *debug_data,
	                 size_t debug_len,
	                 void *user_data);

	int (*on_ping)(hive_session_t *session,
	               const uint8_t opaque[8],
	               void *user_data);

	int (*on_ping_ack)(hive_session_t *session,
	                   const uint8_t opaque[8],
	                   void *user_data);

	int (*on_connection_error)(hive_session_t *session,
	                           int hive_err,
	                           uint32_t h2_error_code,
	                           void *user_data);

	/* RST_STREAM flood advisory */
	int (*on_rst_stream_flood)(hive_session_t *session,
	                           uint32_t rate,
	                           void *user_data);

	/* Send callback — fires once per hive_session_send() call */
	ssize_t (*send)(hive_session_t *session,
	                const struct iovec *iov,
	                int iovcnt,
	                void *user_data);
} hive_callbacks_t;

/* ------------------------------------------------------------------ */
/* Options API                                                         */
/* ------------------------------------------------------------------ */

hive_options_t *hive_options_new(void);
void hive_options_free(hive_options_t *opt);

int hive_options_set_header_table_size(hive_options_t *opt, uint32_t v);
int hive_options_set_enable_push(hive_options_t *opt, uint32_t v);
int hive_options_set_max_concurrent_streams(hive_options_t *opt, uint32_t v);
int hive_options_set_initial_window_size(hive_options_t *opt, uint32_t v);
int hive_options_set_max_frame_size(hive_options_t *opt, uint32_t v);
int hive_options_set_max_header_list_size(hive_options_t *opt, uint32_t v);
int hive_options_set_max_header_count(hive_options_t *opt, uint32_t v);
int hive_options_set_max_continuation_size(hive_options_t *opt, uint32_t v);
int hive_options_set_max_settings_pending(hive_options_t *opt, uint32_t v);
int hive_options_set_rst_stream_flood_threshold(hive_options_t *opt,
                                                uint32_t v);
int hive_options_set_rst_stream_flood_window_secs(hive_options_t *opt,
                                                  uint32_t v);
int hive_options_set_max_send_iov(hive_options_t *opt, uint32_t v);
int hive_options_set_max_header_string_size(hive_options_t *opt, uint32_t v);
int hive_options_set_no_http_messaging(hive_options_t *opt, uint32_t v);
int hive_options_set_no_auto_ping_ack(hive_options_t *opt, uint32_t v);

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                   */
/* ------------------------------------------------------------------ */

hive_session_t *hive_session_server_new(const hive_mem_t *mem,
                                        const hive_options_t *opt,
                                        const hive_callbacks_t *callbacks,
                                        void *user_data);

hive_session_t *hive_session_client_new(const hive_mem_t *mem,
                                        const hive_options_t *opt,
                                        const hive_callbacks_t *callbacks,
                                        void *user_data);

hive_session_t *hive_session_server_upgrade(const hive_mem_t *mem,
                                            const hive_options_t *opt,
                                            const hive_callbacks_t *callbacks,
                                            void *user_data,
                                            const uint8_t *settings_payload,
                                            size_t settings_len);

void hive_session_free(hive_session_t *session);

/* ------------------------------------------------------------------ */
/* Receive and send                                                    */
/* ------------------------------------------------------------------ */

ssize_t
hive_session_recv(hive_session_t *session, const uint8_t *data, size_t len);

int hive_session_send(hive_session_t *session);

int hive_session_want_read(hive_session_t *session);
int hive_session_want_write(hive_session_t *session);

/* ------------------------------------------------------------------ */
/* Submit API                                                          */
/* ------------------------------------------------------------------ */

int hive_submit_response(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen,
                         hive_data_source_t *data_source);

int hive_submit_trailers(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen);

int hive_submit_interim_response(hive_session_t *session,
                                 uint32_t stream_id,
                                 const hive_nv_t *nva,
                                 size_t nvlen);

int hive_submit_push_promise(hive_session_t *session,
                             uint32_t stream_id,
                             const hive_nv_t *nva,
                             size_t nvlen,
                             uint32_t *promised_stream_id_out);

int hive_submit_request(hive_session_t *session,
                        const hive_nv_t *nva,
                        size_t nvlen,
                        hive_data_source_t *data_source,
                        uint32_t *stream_id_out);

int hive_submit_rst_stream(hive_session_t *session,
                           uint32_t stream_id,
                           uint32_t error_code);

int hive_submit_goaway_prepare(hive_session_t *session);
int hive_submit_goaway_final(hive_session_t *session,
                             uint32_t error_code,
                             const uint8_t *debug_data,
                             size_t debug_len);

int hive_submit_ping(hive_session_t *session, const uint8_t opaque[8]);
int hive_submit_ping_ack(hive_session_t *session, const uint8_t opaque[8]);

int hive_session_feed_upgrade_headers(hive_session_t *session,
                                      const hive_nv_t *nva,
                                      size_t nvlen,
                                      int end_stream);

/* ------------------------------------------------------------------ */
/* Buffer retain / free                                                */
/* ------------------------------------------------------------------ */

int hive_buf_retain(hive_session_t *session, hive_buf_t *buf);
void hive_buf_free(hive_session_t *session, hive_buf_t *buf);

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

int32_t hive_session_get_remote_window_size(hive_session_t *session);
hive_settings_t hive_session_get_local_settings(hive_session_t *session);
hive_settings_t hive_session_get_remote_settings(hive_session_t *session);
int hive_stream_get_state(hive_session_t *session, uint32_t stream_id);
int hive_stream_set_user_data(hive_session_t *session,
                              uint32_t stream_id,
                              void *user_data);
void *hive_stream_get_user_data(hive_session_t *session, uint32_t stream_id);

/* ------------------------------------------------------------------ */
/* Standalone HPACK API                                                */
/* ------------------------------------------------------------------ */

typedef struct hive_hpack_encoder hive_hpack_encoder_t;
typedef struct hive_hpack_decoder hive_hpack_decoder_t;

#define HIVE_HPACK_DECODE_EMIT 1
#define HIVE_HPACK_DECODE_DONE 2

int hive_hpack_encoder_new(hive_hpack_encoder_t **enc, size_t max_table_size);
void hive_hpack_encoder_free(hive_hpack_encoder_t *enc);
int hive_hpack_encode(hive_hpack_encoder_t *enc,
                      const hive_nv_t *nva,
                      size_t nvlen,
                      uint8_t *out,
                      size_t *out_len);

int hive_hpack_decoder_new(hive_hpack_decoder_t **dec, size_t max_table_size);
void hive_hpack_decoder_free(hive_hpack_decoder_t *dec);
/*
 * Limitations of the standalone API vs the session decode path:
 *
 * (a) Size-update position rule (RFC 7541 §6.3): the session path enforces
 *     that dynamic table size updates only appear before the first header
 *     field in a block. This API has no block-boundary concept — the caller
 *     is responsible for enforcing this rule across calls.
 *
 * (b) HPACK bomb limits: the session path enforces opt_max_header_list_size
 *     and opt_max_header_count. This API has no options struct and does not
 *     enforce those limits. Callers embedding this in a security-sensitive
 *     context must impose their own size limits before or after calling.
 */
int hive_hpack_decode(hive_hpack_decoder_t *dec,
                      const uint8_t *in,
                      size_t in_len,
                      size_t *consumed,
                      hive_nv_t *nv_out);

#endif /* HIVE_H */
