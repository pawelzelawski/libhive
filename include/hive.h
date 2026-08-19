/*
 * hive.h - HTTP/2 library public API
 *
 * Create a session with hive_session_server_new() or hive_session_client_new().
 * Feed incoming bytes with hive_session_recv().
 * Submit responses with hive_submit_response().
 * Drain outgoing frames with hive_session_send().
 *
 * See ARCHITECTURE.md for full protocol and data flow documentation.
 */

#ifndef HIVE_H
#define HIVE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/*
 * Opaque session handle. Created by hive_session_server_new() or
 * hive_session_client_new(). All library state is contained within.
 * Not thread-safe - one session, one thread.
 */
typedef struct hive_session hive_session_t;

/*
 * Opaque options handle. Created by hive_options_new(), passed to
 * session constructors, and freed by hive_options_free().
 */
typedef struct hive_options hive_options_t;

/*
 * Buffer handle for header name/value delivery.
 *
 * `hive_buf_t` values passed to `on_header` are transient handles owned by the
 * library and are delivered by pointer (`hive_buf_t *`). The library clears
 * `HIVE_BUF_VALID` after `on_header` returns. Call `hive_buf_retain()` inside
 * the callback if bytes are needed after callback return.
 *
 * See ARCHITECTURE.md §9.3.
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
 * Header name/value pair - used for submit functions and the HPACK
 * static table. See ARCHITECTURE.md §9.3.
 *
 * `flags` carries per-pair indexing hints (currently only
 * HIVE_NV_FLAG_NO_INDEX). All static-table entries have flags == 0.
 *
 * NOTE: Field names are `name_len`/`value_len` (not `namelen`/`valuelen`
 * as the architecture pseudocode shows) - the snake-case form matches
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
 * See ARCHITECTURE.md §2.1.
 */
typedef struct hive_mem {
	void *(*malloc)(size_t size, void *ctx);
	void (*free)(void *ptr, void *ctx);
	void *(*calloc)(size_t nmemb, size_t size, void *ctx);
	void *(*realloc)(void *ptr, size_t size, void *ctx);
	void *ctx;
} hive_mem_t;

/*
 * Data source for submit functions.
 *
 * read_callback is invoked by hive_session_send() to produce DATA payload
 * bytes. The callback may either write into *buf (copy path) or set
 * HIVE_DATA_FLAG_NO_COPY and redirect *buf to caller-owned memory.
 * See ARCHITECTURE.md §6.5 and §9.4.
 */
typedef struct hive_data_source hive_data_source_t;

/*
 * read_callback signature used by hive_data_source_t.
 *
 * session:   active session invoking the callback.
 * stream_id: stream being drained.
 * buf:       in/out payload pointer; redirect when returning NO_COPY.
 * length:    max bytes requested for this invocation.
 * data_flags:
 *   - set HIVE_DATA_FLAG_EOF on last chunk
 *   - set HIVE_DATA_FLAG_NO_COPY when *buf points to caller memory
 * source:    owning data source object.
 * user_data: session user_data pointer.
 *
 * Return value:
 *   - >= 0 bytes produced (0 allowed)
 *   - < 0 on callback failure
 */
typedef ssize_t (*hive_read_callback_t)(hive_session_t *session,
                                        uint32_t stream_id,
                                        uint8_t **buf,
                                        size_t length,
                                        uint32_t *data_flags,
                                        hive_data_source_t *source,
                                        void *user_data);

struct hive_data_source {
	hive_read_callback_t read_callback;
	void *ptr;
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
 * Library return codes. `HIVE_OK` means library-call success.
 * All error codes are negative.
 */
#define HIVE_OK 0
#define HIVE_ERR_NOMEM (-1)
#define HIVE_ERR_INVALID_ARG (-2)
#define HIVE_ERR_PROTOCOL (-3)
#define HIVE_ERR_COMPRESSION (-4)
#define HIVE_ERR_FLOW_CONTROL (-5)
#define HIVE_ERR_REFUSED_STREAM (-6)
#define HIVE_ERR_STREAM_CLOSED (-7)
#define HIVE_ERR_GOAWAY (-8)
#define HIVE_ERR_SESSION_CLOSED (-9)
#define HIVE_ERR_WOULDBLOCK (-10)

/*
 * HTTP/2 wire error codes (for GOAWAY and RST_STREAM frames).
 * See RFC 9113 §7.
 *
 * `HIVE_OK` (0) is a library return code for success.
 * `HIVE_H2_NO_ERROR` (0x0) is an HTTP/2 wire code for "no error".
 * They share a numeric value but are different semantic domains and are not
 * interchangeable.
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
 *
 * Return convention for callbacks returning int:
 *   - return 0 to continue processing
 *   - return non-zero to abort the current operation and propagate an error
 *
 * on_goaway debug_data and on_data_chunk data pointers are transient and valid
 * only for the duration of their callback. See ARCHITECTURE.md §9.6.
 */
typedef struct hive_callbacks {
	/*
	 * Called before decoding a header block for `stream_id`.
	 *
	 * Params: session, stream_id, user_data.
	 * Returns: 0 to continue, non-zero to fail the receive path.
	 */
	int (*on_begin_headers)(hive_session_t *session,
	                        uint32_t stream_id,
	                        void *user_data);

	/*
	 * Called for each decoded header field on `stream_id`.
	 *
	 * Params:
	 *   - session: owning session
	 *   - stream_id: stream being decoded
	 *   - name/value: transient handles delivered by pointer (`hive_buf_t
	 * *`)
	 *   - flags: header metadata flags
	 *   - user_data: application context
	 *
	 * Lifetime:
	 *   - `name` and `value` are valid only during this callback.
	 *   - `HIVE_BUF_VALID` is cleared after callback return.
	 *   - Call `hive_buf_retain()` within this callback to persist data.
	 *
	 * Returns: 0 to continue, non-zero to fail the receive path.
	 */
	int (*on_header)(hive_session_t *session,
	                 uint32_t stream_id,
	                 hive_buf_t *name,
	                 hive_buf_t *value,
	                 uint8_t flags,
	                 void *user_data);

	/*
	 * Called after a header block is fully decoded for `stream_id`.
	 *
	 * Params: session, stream_id, flags, user_data.
	 * Returns: 0 to continue, non-zero to fail the receive path.
	 */
	int (*on_headers_complete)(hive_session_t *session,
	                           uint32_t stream_id,
	                           uint8_t flags,
	                           void *user_data);

	/*
	 * Called for each DATA chunk for `stream_id`.
	 *
	 * Params: session, stream_id, data pointer, len, flags, user_data.
	 * Lifetime: `data` points into caller-provided recv bytes and is valid
	 * only during the callback. Returns: 0 to continue, non-zero to fail
	 * the receive path.
	 */
	int (*on_data_chunk)(hive_session_t *session,
	                     uint32_t stream_id,
	                     const uint8_t *data,
	                     size_t len,
	                     uint8_t flags,
	                     void *user_data);

	/* Called when a stream transitions to closed. */
	int (*on_stream_close)(hive_session_t *session,
	                       uint32_t stream_id,
	                       uint32_t error_code,
	                       void *user_data);

	/* Called when a PUSH_PROMISE is received. */
	int (*on_push_promise)(hive_session_t *session,
	                       uint32_t stream_id,
	                       uint32_t promised_stream_id,
	                       void *user_data);

	/* Called when peer SETTINGS (non-ACK) is applied and ACK is queued. */
	int (*on_settings)(hive_session_t *session, void *user_data);
	/* Called when a peer SETTINGS ACK acknowledges our pending SETTINGS. */
	int (*on_settings_ack)(hive_session_t *session, void *user_data);

	/* Called when GOAWAY is received. `debug_data` is transient. */
	int (*on_goaway)(hive_session_t *session,
	                 uint32_t last_stream_id,
	                 uint32_t error_code,
	                 const uint8_t *debug_data,
	                 size_t debug_len,
	                 void *user_data);

	/* Called on inbound PING when auto-ACK is disabled. */
	int (*on_ping)(hive_session_t *session,
	               const uint8_t opaque[8],
	               void *user_data);

	/* Called on inbound PING ACK. */
	int (*on_ping_ack)(hive_session_t *session,
	                   const uint8_t opaque[8],
	                   void *user_data);

	/* Called before library queues GOAWAY for a connection-level failure.
	 */
	int (*on_connection_error)(hive_session_t *session,
	                           int hive_err,
	                           uint32_t h2_error_code,
	                           void *user_data);

	/* Advisory callback for RST_STREAM flood threshold crossings. */
	int (*on_rst_stream_flood)(hive_session_t *session,
	                           uint32_t rate,
	                           void *user_data);

	/*
	 * Transport send callback, called once per hive_session_send()
	 * invocation.
	 *
	 * Returns `ssize_t` bytes written:
	 *   - short write (0..total-1) is not an error; library resumes later
	 *   - full write (total) drains the queued iovecs
	 *   - `-1` with `errno` set signals a fatal I/O error
	 */
	ssize_t (*send)(hive_session_t *session,
	                const struct iovec *iov,
	                int iovcnt,
	                void *user_data);
} hive_callbacks_t;

/* ------------------------------------------------------------------ */
/* Options API                                                         */
/* ------------------------------------------------------------------ */

/*
 * Allocate an options object initialised with library defaults.
 *
 * Returns:
 *   - non-NULL options object on success
 *   - NULL on allocation failure
 *
 * Lifetime: free with hive_options_free().
 */
hive_options_t *hive_options_new(void);
/*
 * Free an options object allocated by hive_options_new().
 *
 * opt: options object or NULL.
 */
void hive_options_free(hive_options_t *opt);

/*
 * Set local SETTINGS_HEADER_TABLE_SIZE advertised to peer.
 *
 * opt: options object from hive_options_new().
 * v:   value to advertise in outbound SETTINGS.
 *
 * Returns HIVE_OK on success, HIVE_ERR_INVALID_ARG on invalid opt/range.
 */
int hive_options_set_header_table_size(hive_options_t *opt, uint32_t v);
/*
 * Set local SETTINGS_ENABLE_PUSH advertised to peer.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_enable_push(hive_options_t *opt, uint32_t v);
/*
 * Set local SETTINGS_MAX_CONCURRENT_STREAMS advertised to peer.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_concurrent_streams(hive_options_t *opt, uint32_t v);
/*
 * Set local SETTINGS_INITIAL_WINDOW_SIZE advertised to peer.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_initial_window_size(hive_options_t *opt, uint32_t v);
/*
 * Set local SETTINGS_MAX_FRAME_SIZE advertised to peer.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_frame_size(hive_options_t *opt, uint32_t v);
/*
 * Set local SETTINGS_MAX_HEADER_LIST_SIZE advertised to peer.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_header_list_size(hive_options_t *opt, uint32_t v);
/*
 * Set max decoded header count per block before protocol rejection.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_header_count(hive_options_t *opt, uint32_t v);
/*
 * Set max total CONTINUATION bytes accepted for one header block.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_continuation_size(hive_options_t *opt, uint32_t v);
/*
 * Set max inbound SETTINGS frames pending local ACK.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_settings_pending(hive_options_t *opt, uint32_t v);
/*
 * Set per-window RST_STREAM flood threshold for advisory callback.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_rst_stream_flood_threshold(hive_options_t *opt,
                                                uint32_t v);
/*
 * Set RST_STREAM flood accounting window in seconds.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_rst_stream_flood_window_secs(hive_options_t *opt,
                                                  uint32_t v);
/*
 * Set maximum iovec entries queued for one send operation.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_send_iov(hive_options_t *opt, uint32_t v);
/*
 * Set maximum decoded header string length accepted by HPACK decode.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_max_header_string_size(hive_options_t *opt, uint32_t v);
/*
 * Disable HTTP messaging validation rules when set to non-zero.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_no_http_messaging(hive_options_t *opt, uint32_t v);
/*
 * Disable automatic PING ACK queuing when set to non-zero.
 *
 * Returns HIVE_OK or HIVE_ERR_INVALID_ARG.
 */
int hive_options_set_no_auto_ping_ack(hive_options_t *opt, uint32_t v);

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                   */
/* ------------------------------------------------------------------ */

/*
 * Create a server-role HTTP/2 session.
 *
 * Params:
 *   - mem: allocator callbacks or NULL for system allocator
 *   - opt: options object or NULL for defaults
 *   - callbacks: callback table (must be non-NULL, callbacks->send required)
 *   - user_data: opaque pointer passed to callbacks
 * Returns:
 *   - non-NULL session on success
 *   - NULL on allocation/setup failure
 */
hive_session_t *hive_session_server_new(const hive_mem_t *mem,
                                        const hive_options_t *opt,
                                        const hive_callbacks_t *callbacks,
                                        void *user_data);

/*
 * Create a client-role HTTP/2 session.
 *
 * Params:
 *   - mem: allocator callbacks or NULL for system allocator
 *   - opt: options object or NULL for defaults
 *   - callbacks: callback table (must be non-NULL, callbacks->send required)
 *   - user_data: opaque pointer passed to callbacks
 * Returns:
 *   - non-NULL session on success
 *   - NULL on allocation/setup failure
 */
hive_session_t *hive_session_client_new(const hive_mem_t *mem,
                                        const hive_options_t *opt,
                                        const hive_callbacks_t *callbacks,
                                        void *user_data);

/*
 * Create a server-role session for HTTP/1.1 Upgrade (h2c) path.
 *
 * Params match hive_session_server_new() plus:
 *   - settings_payload/settings_len: decoded HTTP2-Settings payload bytes
 * Returns non-NULL on success, NULL on failure.
 */
hive_session_t *hive_session_server_upgrade(const hive_mem_t *mem,
                                            const hive_options_t *opt,
                                            const hive_callbacks_t *callbacks,
                                            void *user_data,
                                            const uint8_t *settings_payload,
                                            size_t settings_len);

/*
 * Free a session and all resources owned by it.
 *
 * session: session from hive_session_*_new(), or NULL.
 */
void hive_session_free(hive_session_t *session);

/* ------------------------------------------------------------------ */
/* Receive and send                                                    */
/* ------------------------------------------------------------------ */

/*
 * Feed received wire bytes into the session receive state machine.
 *
 * session: target session.
 * data:    input bytes from transport read path.
 * len:     number of bytes in data.
 *
 * Returns:
 *   - >= 0: number of bytes consumed from data
 *   - -1: fatal receive-path failure
 */
ssize_t
hive_session_recv(hive_session_t *session, const uint8_t *data, size_t len);

/*
 * Drain queued outbound frames through callbacks.send.
 *
 * Calls callbacks.send() at most once per invocation, using a batched iovec.
 * Partial writes are normal: callbacks.send() returns bytes written and
 * hive_session_send() retains the unsent tail for the next call.
 *
 * Returns HIVE_OK on success or a negative HIVE_ERR_* code on failure.
 */
int hive_session_send(hive_session_t *session);

/*
 * Advisory read-interest flag.
 *
 * Returns 1 when caller should continue reading, 0 otherwise.
 */
int hive_session_want_read(hive_session_t *session);
/*
 * Advisory write-interest flag.
 *
 * Returns 1 when queued/partial/pending data exists for send path, 0 otherwise.
 */
int hive_session_want_write(hive_session_t *session);

/* ------------------------------------------------------------------ */
/* Submit API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Queue a final response HEADERS block and optional DATA source for stream.
 *
 * session:     server or client session.
 * stream_id:   target stream.
 * nva/nvlen:   response headers.
 * data_source: optional body source; NULL means headers-only response.
 *
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_response(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen,
                         hive_data_source_t *data_source);

/*
 * Queue trailing headers with END_STREAM for stream_id.
 *
 * nva must not include pseudo-headers.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_trailers(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen);

/*
 * Queue informational (1xx) response headers for stream_id.
 *
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_interim_response(hive_session_t *session,
                                 uint32_t stream_id,
                                 const hive_nv_t *nva,
                                 size_t nvlen);

/*
 * Queue PUSH_PROMISE and reserve the promised stream.
 *
 * promised_stream_id_out receives the allocated stream id on success.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_push_promise(hive_session_t *session,
                             uint32_t stream_id,
                             const hive_nv_t *nva,
                             size_t nvlen,
                             uint32_t *promised_stream_id_out);

/*
 * Queue a client request and allocate a new client-initiated stream id.
 *
 * stream_id_out receives the allocated stream id on success.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_request(hive_session_t *session,
                        const hive_nv_t *nva,
                        size_t nvlen,
                        hive_data_source_t *data_source,
                        uint32_t *stream_id_out);

/*
 * Queue RST_STREAM for stream_id using HTTP/2 wire error code.
 *
 * error_code must be from HIVE_H2_* wire codes.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_rst_stream(hive_session_t *session,
                           uint32_t stream_id,
                           uint32_t error_code);

/*
 * Queue first-phase GOAWAY with last_stream_id=0x7fffffff to stop new work
 * while allowing in-flight streams to complete.
 *
 * This does not close the session.
 * Returns HIVE_OK or negative HIVE_ERR_*.
 */
int hive_submit_goaway_prepare(hive_session_t *session);

/*
 * Queue final GOAWAY with provided HTTP/2 wire error code and optional
 * debug bytes, transitioning to GOAWAY_SENT state.
 *
 * error_code is an HTTP/2 wire code (HIVE_H2_*), not a HIVE_ERR_* code.
 * Returns HIVE_OK or negative HIVE_ERR_*.
 */
int hive_submit_goaway_final(hive_session_t *session,
                             uint32_t error_code,
                             const uint8_t *debug_data,
                             size_t debug_len);

/*
 * Queue PING (ACK=0) with 8 opaque bytes.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_ping(hive_session_t *session, const uint8_t opaque[8]);
/*
 * Queue PING ACK (ACK=1) with 8 opaque bytes.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_submit_ping_ack(hive_session_t *session, const uint8_t opaque[8]);

/*
 * Inject HTTP/1.1-upgrade request headers into stream 1 callback flow.
 *
 * Used with sessions created by hive_session_server_upgrade().
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_session_feed_upgrade_headers(hive_session_t *session,
                                      const hive_nv_t *nva,
                                      size_t nvlen,
                                      int end_stream);

/* ------------------------------------------------------------------ */
/* Buffer retain / free                                                */
/* ------------------------------------------------------------------ */

/*
 * Retain a transient hive_buf_t by copying bytes into session-owned storage.
 * Must be called during the callback while HIVE_BUF_VALID is set.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_buf_retain(hive_session_t *session, hive_buf_t *buf);
/*
 * Free an owned buffer previously retained with hive_buf_retain().
 *
 * Safe to call with non-owned or empty buffers; buffer is reset to empty.
 */
void hive_buf_free(hive_session_t *session, hive_buf_t *buf);

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

/*
 * Return current connection-level remote flow-control window.
 *
 * Returns 0 if session is NULL.
 */
int32_t hive_session_get_remote_window_size(hive_session_t *session);
/*
 * Return current local SETTINGS values.
 *
 * Returns zeroed settings when session is NULL.
 */
hive_settings_t hive_session_get_local_settings(hive_session_t *session);
/*
 * Return current remote SETTINGS values.
 *
 * Returns zeroed settings when session is NULL.
 */
hive_settings_t hive_session_get_remote_settings(hive_session_t *session);
/*
 * Return stream state value for stream_id.
 *
 * Returns HIVE_STREAM_IDLE when session/stream is absent.
 */
int hive_stream_get_state(hive_session_t *session, uint32_t stream_id);
/*
 * Attach opaque user pointer to an open stream.
 *
 * Returns HIVE_OK or HIVE_ERR_STREAM_CLOSED.
 */
int hive_stream_set_user_data(hive_session_t *session,
                              uint32_t stream_id,
                              void *user_data);
/*
 * Get stream user pointer.
 *
 * Returns NULL if session/stream does not exist.
 */
void *hive_stream_get_user_data(hive_session_t *session, uint32_t stream_id);

/* ------------------------------------------------------------------ */
/* Standalone HPACK API                                                */
/* ------------------------------------------------------------------ */

typedef struct hive_hpack_encoder hive_hpack_encoder_t;
typedef struct hive_hpack_decoder hive_hpack_decoder_t;

#define HIVE_HPACK_DECODE_EMIT 1
#define HIVE_HPACK_DECODE_DONE 2

/*
 * Create a standalone HPACK encoder with max dynamic table size.
 *
 * enc receives a new encoder on success.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_hpack_encoder_new(hive_hpack_encoder_t **enc, size_t max_table_size);
/* Free standalone HPACK encoder (NULL-safe). */
void hive_hpack_encoder_free(hive_hpack_encoder_t *enc);
/*
 * Encode nvlen header fields from nva into out.
 *
 * out_len is input/output: capacity on entry, bytes written on success.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_hpack_encode(hive_hpack_encoder_t *enc,
                      const hive_nv_t *nva,
                      size_t nvlen,
                      uint8_t *out,
                      size_t *out_len);

/*
 * Create a standalone HPACK decoder with max dynamic table size.
 *
 * dec receives a new decoder on success.
 * Returns HIVE_OK or a negative HIVE_ERR_* code.
 */
int hive_hpack_decoder_new(hive_hpack_decoder_t **dec, size_t max_table_size);
/* Free standalone HPACK decoder (NULL-safe). */
void hive_hpack_decoder_free(hive_hpack_decoder_t *dec);
/*
 * Limitations of the standalone API vs the session decode path:
 *
 * (a) Size-update position rule (RFC 7541 §6.3): the session path enforces
 *     that dynamic table size updates only appear before the first header
 *     field in a block. This API has no block-boundary concept - the caller
 *     is responsible for enforcing this rule across calls.
 *
 * (b) HPACK bomb limits: the session path enforces opt_max_header_list_size
 *     and opt_max_header_count. This API has no options struct and does not
 *     enforce those limits. Callers embedding this in a security-sensitive
 *     context must impose their own size limits before or after calling.
 */
/*
 * Decode one header field incrementally from `in`.
 *
 * Params:
 *   - dec: decoder instance
 *   - in/in_len: input bytes
 *   - consumed: output count of bytes consumed from `in`
 *   - nv_out: output header field on HIVE_HPACK_DECODE_EMIT
 * Returns:
 *   - HIVE_HPACK_DECODE_EMIT when one header is emitted in `nv_out`
 *   - HIVE_HPACK_DECODE_DONE when input is exhausted with no emitted header
 *   - negative HIVE_ERR_* on decode failure
 */
int hive_hpack_decode(hive_hpack_decoder_t *dec,
                      const uint8_t *in,
                      size_t in_len,
                      size_t *consumed,
                      hive_nv_t *nv_out);

#endif /* HIVE_H */
