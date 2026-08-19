/*
 * hive_internal.h - internal shared types and forward declarations
 *
 * Not included by embedders. Internal to the library only.
 * See ARCHITECTURE.md §2 for the full session struct layout (regions A–J).
 * See ARCHITECTURE.md §5 for the stream table types.
 */

#ifndef HIVE_INTERNAL_H
#define HIVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include "../include/hive.h"
#include "hive_frame.h"
#include "hive_hpack.h"

/* ------------------------------------------------------------------ */
/* ASan helpers (HIVE_DEBUG builds only)                               */
/* See TECH_STACK.md §7.2 and CODING_STANDARDS.md §3.5.               */
/* ------------------------------------------------------------------ */

#if defined(HIVE_DEBUG) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HIVE_HAS_ADDRESS_SANITIZER 1
#endif
#endif

#if defined(HIVE_DEBUG) &&                                                  \
    (defined(__SANITIZE_ADDRESS__) || defined(HIVE_HAS_ADDRESS_SANITIZER))
#include <sanitizer/asan_interface.h>
#define HIVE_ASAN_POISON(ptr, size) __asan_poison_memory_region((ptr), (size))
#define HIVE_ASAN_UNPOISON(ptr, size)                                          \
	__asan_unpoison_memory_region((ptr), (size))
#else
#define HIVE_ASAN_POISON(ptr, size) ((void)0)
#define HIVE_ASAN_UNPOISON(ptr, size) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Send path compile-time cap (ARCHITECTURE.md §6.6).                 */
/* opt_max_send_iov must never exceed HIVE_SEND_IOV_MAX.              */
/* Default opt_max_send_iov is 512; this cap is 1024.                 */
/* ------------------------------------------------------------------ */

#define HIVE_SEND_IOV_MAX 1024

/* ------------------------------------------------------------------ */
/* Session state enumeration (ARCHITECTURE.md §2.3).                  */
/* ------------------------------------------------------------------ */

typedef enum {
	HIVE_SESSION_OPEN = 0,
	HIVE_SESSION_GOAWAY_SENT = 1, /* we sent GOAWAY, draining in-flight */
	HIVE_SESSION_GOAWAY_RECV = 2, /* peer sent GOAWAY, no new streams   */
	HIVE_SESSION_CLOSED = 3,      /* session is dead, free it           */
} hive_session_state_t;

/* ------------------------------------------------------------------ */
/* Stream table types (ARCHITECTURE.md §5)                            */
/* ------------------------------------------------------------------ */

/*
 * Hash entry sentinel values.
 * EMPTY (0xFFFFFFFF): slot has never been written.
 * TOMBSTONE (0xFFFFFFFE): slot held a stream that was closed.
 * Both sentinels are invalid HTTP/2 stream IDs (stream IDs are 31-bit,
 * so 0xFFFFFFFE and 0xFFFFFFFF cannot collide with real IDs).
 */
#define STREAM_HASH_EMPTY 0xFFFFFFFFu
#define STREAM_HASH_TOMBSTONE 0xFFFFFFFEu

/*
 * Two-layer hash table entry.  Layer 1 maps stream_id → slot_index.
 * Layer 2 (stream_slots) holds the full hive_stream_t objects.
 * See ARCHITECTURE.md §5.2.
 */
typedef struct {
	uint32_t
	    stream_id; /* STREAM_HASH_EMPTY, STREAM_HASH_TOMBSTONE, or ID */
	uint32_t slot_index; /* index into stream_slots[] */
} stream_hash_entry_t;       /* 8 bytes */

/*
 * Stream state machine values.
 * See ARCHITECTURE.md §5.3 and RFC 9113 §5.1.
 */
typedef enum {
	HIVE_STREAM_IDLE = 0,
	HIVE_STREAM_OPEN = 1,
	HIVE_STREAM_HALF_CLOSED_LOCAL = 2,  /* we sent END_STREAM        */
	HIVE_STREAM_HALF_CLOSED_REMOTE = 3, /* peer sent END_STREAM       */
	HIVE_STREAM_CLOSED = 4,
	HIVE_STREAM_RESERVED_LOCAL = 5,  /* server push promised       */
	HIVE_STREAM_RESERVED_REMOTE = 6, /* client received PUSH_PROMISE */
} hive_stream_state_t;

/* Internal hive_stream_t::flags bits. */
#define HIVE_STREAM_FLAG_HEADERS_SEEN 0x01u

/*
 * Per-stream state object - pool-allocated from stream_slots[].
 * stream_id == 0 means the slot is not in use.
 * See ARCHITECTURE.md §5.3.
 *
 * Layout: 4+1+1+1+1 + 4+4+4+4 + 8 + 8 + 8 + 16 = 64 bytes.
 */
typedef struct hive_stream {
	uint32_t stream_id;               /* 0 = slot not in use         */
	uint8_t state;                    /* hive_stream_state_t         */
	uint8_t flags;                    /* HIVE_STREAM_FLAG_* (Phase 5+)*/
	uint8_t weight;                   /* PRIORITY weight - stored    */
	uint8_t _pad;                     /* padding to uint32_t align   */
	int32_t send_window;              /* stream-level send window    */
	int32_t recv_window;              /* stream-level recv window    */
	uint32_t recv_consumed;           /* unacked recv bytes          */
	uint32_t _pad2;                   /* padding to int64_t align    */
	int64_t content_length_expected;  /* from Content-Length; -1=absent */
	uint64_t content_length_received; /* DATA bytes received so far  */
	void *user_data;                  /* per-stream caller context   */
	hive_data_source_t data_source;   /* body source (Phase 6+)      */
} hive_stream_t; /* 4+1+1+1+1+4+4+4+4+8+8+8+16 = 64 bytes */

/* ------------------------------------------------------------------ */
/* Compile-time struct size checks (CODING_STANDARDS.md §1.3).        */
/* These must not be removed - they guard against silent layout drift. */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(hive_stream_t) == 64,
               "hive_stream_t size changed - update ARCHITECTURE.md §5.3");
_Static_assert(
    sizeof(stream_hash_entry_t) == 8,
    "stream_hash_entry_t size changed - update ARCHITECTURE.md §5.2");
_Static_assert(sizeof(hpack_entry_t) == 8,
               "hpack_entry_t size changed - update ARCHITECTURE.md §4.2");
_Static_assert(sizeof(hive_settings_t) == 24,
               "hive_settings_t size changed - update ARCHITECTURE.md §2.5");
/* huff_entry_t size check is active in src/hive_hpack.h next to the type. */

/* ------------------------------------------------------------------ */
/* Options struct (ARCHITECTURE.md §9.5)                              */
/* Allocated with system calloc by hive_options_new(); freed with     */
/* system free by hive_options_free().  Not per-session.              */
/* ------------------------------------------------------------------ */

struct hive_options {
	uint32_t opt_header_table_size;      /* dflt 4096,  range 0–65536     */
	uint32_t opt_enable_push;            /* dflt 1,     range 0–1         */
	uint32_t opt_max_concurrent_streams; /* dflt 100,   range 1–65535     */
	uint32_t opt_initial_window_size;    /* dflt 65535, range 1–2^31-1   */
	uint32_t opt_max_frame_size; /* dflt 16384, range 16384–16777215 */
	uint32_t opt_max_header_list_size; /* dflt 65536, range 1–16777215  */
	uint32_t opt_max_header_count;     /* dflt 100,   range 1–65535     */
	uint32_t
	    opt_max_continuation_size; /* dflt 65536, range 16384–16777215 */
	uint32_t opt_max_settings_pending;  /* dflt 3,     range 1–255       */
	uint32_t opt_rst_flood_threshold;   /* dflt 100,   range 1–65535     */
	uint32_t opt_rst_flood_window_secs; /* dflt 10,    range 1–3600      */
	uint32_t opt_max_send_iov; /* dflt 512,   range 64–HIVE_SEND_IOV_MAX */
	uint32_t opt_max_header_string_size; /* dflt 8192,  range 256–65536   */
	uint8_t opt_no_http_messaging;       /* dflt 0,     range 0–1         */
	uint8_t opt_no_auto_ping_ack;        /* dflt 0,     range 0–1         */
};

/* ------------------------------------------------------------------ */
/* Full hive_session_t - all 10 regions (ARCHITECTURE.md §2.1–2.10)  */
/* ------------------------------------------------------------------ */

struct hive_session {

	/* ------------------------------------------------------------ */
	/* Region A - Identity and Callbacks (read-only after init)     */
	/* ARCHITECTURE.md §2.1                                         */
	/* ------------------------------------------------------------ */
	hive_mem_t mem; /* allocator - copied at creation              */
	hive_callbacks_t callbacks; /* event callbacks - copied at creation */
	void *user_data; /* passed unchanged to every callback          */
	uint8_t role;    /* HIVE_ROLE_SERVER or HIVE_ROLE_CLIENT        */

	/* ------------------------------------------------------------ */
	/* Region B - Options (flat copies from hive_options_t)         */
	/* ARCHITECTURE.md §2.2                                         */
	/* ------------------------------------------------------------ */
	uint32_t
	    opt_header_table_size; /* SETTINGS_HEADER_TABLE_SIZE, dflt 4096  */
	uint32_t opt_enable_push;  /* SETTINGS_ENABLE_PUSH (client only)     */
	uint32_t opt_max_concurrent_streams; /* SETTINGS_MAX_CONCURRENT_STREAMS,
	                                        d 100 */
	uint32_t
	    opt_initial_window_size; /* SETTINGS_INITIAL_WINDOW_SIZE, d 65535 */
	uint32_t opt_max_frame_size; /* SETTINGS_MAX_FRAME_SIZE, dflt 16384 */
	uint32_t opt_max_header_list_size; /* decoded header list limit, dflt
	                                      65536  */
	uint32_t opt_max_header_count; /* max headers per block, dflt 100     */
	uint32_t opt_max_continuation_size; /* reassembly cap, dflt
	                                       4×max_frame_size  */
	uint32_t opt_max_settings_pending;  /* max unACK'd outbound SETTINGS,
	                                       dflt 3  */
	uint32_t opt_rst_flood_threshold; /* RST_STREAM rate limit, dflt 100 */
	uint32_t
	    opt_rst_flood_window_secs; /* RST_STREAM rate window, dflt 10 */
	uint32_t opt_max_send_iov; /* iovec array size, dflt 512             */
	uint32_t opt_max_header_string_size; /* Huffman scratch size/buf, dflt
	                                        8192    */
	uint8_t opt_no_http_messaging; /* 0 = validate per RFC 9113 §8       */
	uint8_t opt_no_auto_ping_ack;  /* 0 = auto-ACK PING frames	*/

	/* ------------------------------------------------------------ */
	/* Region C - Connection State                                   */
	/* ARCHITECTURE.md §2.3                                         */
	/* ------------------------------------------------------------ */
	uint8_t session_state; /* hive_session_state_t enum value         */
	uint8_t goaway_sent;   /* 1 = any GOAWAY queued or sent           */
	uint8_t goaway_prepare_sent; /* 1 = prepare-phase GOAWAY sent */
	uint8_t goaway_recv; /* 1 = GOAWAY received from peer           */
	uint32_t last_stream_id_local;  /* highest stream ID we opened  */
	uint32_t last_stream_id_remote; /* highest stream ID peer opened */
	uint32_t next_stream_id; /* next ID to assign (odd/even per role)   */
	uint32_t goaway_last_stream_id_sent; /* last_stream_id in our most
	                                        recent GOAWAY*/
	uint32_t
	    goaway_last_stream_id_recv;  /* last_stream_id in peer's GOAWAY  */
	uint32_t goaway_error_code_recv; /* error_code from peer's GOAWAY */

	/* ------------------------------------------------------------ */
	/* Region D - Connection-Level Flow Control                      */
	/* ARCHITECTURE.md §2.4                                         */
	/* ------------------------------------------------------------ */
	int32_t send_window; /* our send budget; decrements as we send DATA */
	int32_t recv_window; /* peer's send budget; decrement on DATA receipt */
	uint32_t
	    recv_consumed; /* bytes received, not yet ACK'd via WINDOW_UPDATE */

	/* ------------------------------------------------------------ */
	/* Region E - SETTINGS State                                     */
	/* ARCHITECTURE.md §2.5                                         */
	/* ------------------------------------------------------------ */
	hive_settings_t local_settings;  /* our current effective SETTINGS  */
	hive_settings_t remote_settings; /* peer's current effective SETTINGS */
	hive_settings_t
	    *pending_settings; /* ring of outbound SETTINGS awaiting ACK */
	uint8_t pending_head;  /* oldest entry (next ACK'd)           */
	uint8_t pending_tail;  /* where next outbound entry goes      */
	uint8_t pending_count; /* outbound SETTINGS awaiting ACK      */
	uint8_t inbound_settings_count; /* inbound SETTINGS not yet ACK'd */

	/* ------------------------------------------------------------ */
	/* Region F - RST_STREAM Flood Detection                         */
	/* ARCHITECTURE.md §2.6                                         */
	/* ------------------------------------------------------------ */
	uint32_t rst_flood_count; /* RST_STREAM frames in current window */
	uint64_t rst_flood_window_start; /* CLOCK_MONOTONIC seconds when window
	                                    started */

	/* ------------------------------------------------------------ */
	/* Region G - Frame Receive State Machine                        */
	/* ARCHITECTURE.md §2.7                                         */
	/* ------------------------------------------------------------ */
	uint8_t frame_hdr_buf[9];    /* 9-byte frame header staging           */
	uint8_t frame_hdr_count;     /* bytes accumulated so far (0–9)        */
	uint8_t preface_count;       /* preface bytes consumed so far         */
	uint8_t recv_state;          /* hive_recv_state_t enum value          */
	frame_hdr_t cur_frame;       /* parsed frame header                   */
	uint32_t payload_remaining;  /* bytes left in current frame payload   */
	uint32_t pad_remaining;      /* padding bytes left to skip            */
	uint8_t pad_length_received; /* 1 = Pad Length byte extracted         */
	uint8_t pad_validated;       /* 1 = pad_length validated              */
	uint8_t fc_accounted; /* 1 = flow-control decremented for frame */
	uint32_t reassembly_stream_error_code; /* non-zero = RST code to send at
	                                          END_HEADERS */
	uint8_t ctrl_staging[8];    /* small control frame byte accumulator  */
	uint8_t ctrl_staging_count; /* bytes in ctrl_staging                 */
	uint32_t
	    reassembly_stream_id; /* stream_id of active HEADERS reassembly */
	uint32_t reassembly_promised_stream_id; /* promised stream_id
	                                           (PUSH_PROMISE) */
	uint8_t reassembly_type; /* 0 = HEADERS, 1 = PUSH_PROMISE         */
	uint8_t
	    reassembly_end_stream; /* END_STREAM from opening HEADERS frame  */
	uint8_t reassembly_active; /* 1 = HEADERS/PP block in progress       */
	uint32_t reassembly_len;   /* bytes written into reassembly_buf      */
	uint32_t reassembly_cap;   /* allocated bytes; includes GOAWAY debug */
	uint8_t *reassembly_buf;   /* pre-allocated reassembly buffer        */
	uint8_t priority_payload_len; /* PRIORITY prefix bytes remaining */

	/* ------------------------------------------------------------ */
	/* Region H - HPACK State                                        */
	/* ARCHITECTURE.md §2.8                                         */
	/* ------------------------------------------------------------ */
	hpack_table_t enc_table; /* encoder dynamic table                   */
	hpack_table_t dec_table; /* decoder dynamic table                   */
	uint8_t *hpack_scratch_name;   /* Huffman decode scratch (name)   */
	uint8_t *hpack_scratch_value;  /* Huffman decode scratch (value)  */
	hive_buf_t hpack_name_handle;  /* handle passed to on_header (name)  */
	hive_buf_t hpack_value_handle; /* handle passed to on_header (value) */

	/* ------------------------------------------------------------ */
	/* Region I - Stream Table                                       */
	/* ARCHITECTURE.md §2.9                                         */
	/* ------------------------------------------------------------ */
	stream_hash_entry_t
	    *stream_hash;            /* hash table, hash_table_size entries  */
	hive_stream_t *stream_slots; /* slot array, max_concurrent entries   */
	uint32_t *stream_free_stack; /* free slot indices                    */
	uint32_t stream_hash_mask;   /* hash_table_size - 1                  */
	uint32_t stream_free_top;    /* top of free stack (next pop index)   */
	uint32_t stream_open_count;  /* total currently open streams         */
	uint32_t peer_stream_open_count; /* peer-initiated open streams     */
	uint32_t tombstone_count; /* hash entries in TOMBSTONE state      */
	uint32_t closes_since_compact; /* closes since last compaction      */

	/* ------------------------------------------------------------ */
	/* Region J - Send Queue and Partial Send State                  */
	/* ARCHITECTURE.md §2.10                                        */
	/* ------------------------------------------------------------ */
	struct iovec *send_iov; /* iovec array, opt_max_send_iov entries    */
	uint8_t *send_iov_settings_ack; /* SETTINGS ACK completion markers       */
	int send_iov_count;     /* entries currently queued                 */
	uint8_t *send_buf;      /* frame serialisation buffer               */
	size_t send_buf_used;   /* bytes written; reset to 0 after full send */
	size_t send_buf_cap;    /* sized for HEADERS + CONTINUATION overhead */
	size_t
	    send_partial_offset; /* total bytes already sent from cur batch  */
	uint8_t send_partial;    /* 1 = unsent tail remains                  */

	/* ------------------------------------------------------------ */
	/* Test-visible error latch (Phase 2 parser tests)              */
	/* ------------------------------------------------------------ */
	int last_err;
	uint32_t last_h2_err;
	uint8_t closed;
};

/*
 * Client connection preface magic bytes (RFC 9113 §3.4).
 * Single definition shared by hive.c and hive_frame.c.
 */
static const uint8_t client_preface_magic[24] = {
    'P', 'R', 'I',  ' ',  '*',  ' ',  'H', 'T', 'T',  'P',  '/',  '2',
    '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};

/*
 * Stream table helpers (ARCHITECTURE.md §5.4–§5.6).
 * Internal-only API used by frame receive and unit tests.
 */
uint32_t stream_hash_fn(uint32_t stream_id, uint32_t hash_mask);
int stream_open(hive_session_t *s, uint32_t stream_id, uint8_t state);
hive_stream_t *stream_lookup(hive_session_t *s, uint32_t stream_id);
void stream_close(hive_session_t *s, hive_stream_t *stream);

#endif /* HIVE_INTERNAL_H */
