/*
 * hive_frame.h — frame parser and serialiser (internal)
 *
 * Defines all HTTP/2 frame type constants, frame flag constants,
 * the parsed frame header struct, and the receive state machine enum.
 *
 * See ARCHITECTURE.md §3.1 for the full state machine state list.
 * See ARCHITECTURE.md §3.2 for the frame header structure and frame type
 * constants.
 * See ARCHITECTURE.md §6 for frame header serialisation.
 * RFC 9113: https://www.rfc-editor.org/rfc/rfc9113
 *
 * Not included by embedders — internal to the library only.
 */

#ifndef HIVE_FRAME_H
#define HIVE_FRAME_H

#include <stdint.h>

#include "../include/hive.h"
#include "hive_internal.h"

/* ------------------------------------------------------------------ */
/* Frame type constants — RFC 9113 §4.1                               */
/* See ARCHITECTURE.md §3.2.                                          */
/* ------------------------------------------------------------------ */

#define HIVE_FRAME_DATA          0x0u
#define HIVE_FRAME_HEADERS       0x1u
#define HIVE_FRAME_PRIORITY      0x2u
#define HIVE_FRAME_RST_STREAM    0x3u
#define HIVE_FRAME_SETTINGS      0x4u
#define HIVE_FRAME_PUSH_PROMISE  0x5u
#define HIVE_FRAME_PING          0x6u
#define HIVE_FRAME_GOAWAY        0x7u
#define HIVE_FRAME_WINDOW_UPDATE 0x8u
#define HIVE_FRAME_CONTINUATION  0x9u

/* ------------------------------------------------------------------ */
/* Frame flag constants — RFC 9113 §4.1 and per-frame sections        */
/* ------------------------------------------------------------------ */

/*
 * HIVE_FLAG_END_STREAM (0x01): set on DATA or HEADERS frames to mark
 * the last frame for the stream. RFC 9113 §6.1, §6.2.
 *
 * HIVE_FLAG_ACK (0x01): set on SETTINGS frames to acknowledge a peer
 * SETTINGS, and on PING frames to mark a PING response. RFC 9113 §6.5,
 * §6.7. Shares bit 0x01 with HIVE_FLAG_END_STREAM; they apply to
 * different frame types and are never ambiguous in context.
 */
#define HIVE_FLAG_END_STREAM  0x01u /* DATA, HEADERS */
#define HIVE_FLAG_ACK         0x01u /* SETTINGS, PING */

/*
 * HIVE_FLAG_END_HEADERS (0x04): set on HEADERS, PUSH_PROMISE, or
 * CONTINUATION frames to indicate the header block is complete.
 * RFC 9113 §6.2, §6.6, §6.10.
 */
#define HIVE_FLAG_END_HEADERS 0x04u /* HEADERS, PUSH_PROMISE, CONTINUATION */

/*
 * HIVE_FLAG_PADDED (0x08): set on DATA, HEADERS, or PUSH_PROMISE frames
 * to indicate a Pad Length field and padding bytes are present.
 * RFC 9113 §6.1, §6.2, §6.6.
 */
#define HIVE_FLAG_PADDED 0x08u /* DATA, HEADERS, PUSH_PROMISE */

/*
 * HIVE_FLAG_PRIORITY (0x20): set on HEADERS frames to indicate a stream
 * dependency and weight (PRIORITY prefix, 5 bytes) is present.
 * Deprecated by RFC 9113 — received and silently consumed.
 * RFC 9113 §6.2.
 */
#define HIVE_FLAG_PRIORITY 0x20u /* HEADERS */

/* ------------------------------------------------------------------ */
/* Frame header structure — ARCHITECTURE.md §3.2                      */
/* ------------------------------------------------------------------ */

/*
 * Parsed 9-byte frame header.
 *
 * Populated from frame_hdr_buf[9] in the session once all 9 bytes have
 * accumulated via frame_hdr_count. Not stored persistently — it lives in
 * the session as cur_frame and is overwritten on each new frame.
 *
 * Wire layout (RFC 9113 §4.1):
 *   Bytes 0–2: 24-bit payload length (big-endian)
 *   Byte  3:   frame type
 *   Byte  4:   flags
 *   Bytes 5–8: reserved bit (R) + 31-bit stream identifier (big-endian)
 *
 * The R bit is masked off when populating stream_id.
 */
typedef struct {
	uint32_t length;    /* 24-bit payload length, network byte order → host */
	uint8_t  type;      /* HIVE_FRAME_* constant */
	uint8_t  flags;     /* HIVE_FLAG_* bitmask */
	uint32_t stream_id; /* 31-bit stream identifier; R bit masked off */
} frame_hdr_t;

/* ------------------------------------------------------------------ */
/* Receive state machine — ARCHITECTURE.md §3.1                       */
/* ------------------------------------------------------------------ */

/*
 * All 18 states of the incremental frame receive state machine.
 *
 * The state machine is advanced inside hive_session_recv() on each call.
 * Byte-by-byte delivery is handled correctly: any state may suspend mid-
 * frame and resume correctly on the next hive_session_recv() call.
 *
 * See ARCHITECTURE.md §3 for the full processing loop pseudocode.
 */
typedef enum {
	RECV_CLIENT_PREFACE        = 0,  /* server: consume 24-byte PRI * magic */
	RECV_SERVER_PREFACE        = 1,  /* client: first frame must be SETTINGS non-ACK */
	RECV_FRAME_HEADER          = 2,  /* accumulate frame_hdr_buf[9] via frame_hdr_count */
	RECV_DATA_PAYLOAD          = 3,  /* streaming DATA delivery into caller's buffer */
	RECV_DATA_PAD              = 4,  /* skip padding bytes after DATA payload */
	RECV_HEADERS_PAYLOAD       = 5,  /* copy header block bytes into reassembly_buf */
	RECV_HEADERS_PAD           = 6,  /* skip padding bytes after HEADERS payload */
	RECV_CONTINUATION_PAYLOAD  = 7,  /* continue header block; any other frame = conn error */
	RECV_PUSH_PROMISE_PAYLOAD  = 8,  /* extract 4-byte promised stream_id, copy remainder */
	RECV_PUSH_PROMISE_PAD      = 9,  /* skip padding bytes after PUSH_PROMISE payload */
	RECV_SETTINGS_PAYLOAD      = 10, /* process 6-byte parameters via ctrl_staging */
	RECV_PING_PAYLOAD          = 11, /* accumulate 8 bytes via ctrl_staging */
	RECV_RST_STREAM_PAYLOAD    = 12, /* accumulate 4 bytes via ctrl_staging */
	RECV_WINDOW_UPDATE_PAYLOAD = 13, /* accumulate 4 bytes via ctrl_staging */
	RECV_GOAWAY_PAYLOAD        = 14, /* accumulate first 8 bytes (last_stream_id + error_code) */
	RECV_GOAWAY_DEBUG          = 15, /* collect optional GOAWAY debug bytes into reassembly_buf */
	RECV_PRIORITY_PAYLOAD      = 16, /* accumulate 5 bytes; discard — deprecated RFC 9113 */
	RECV_SKIP_PAYLOAD          = 17, /* skip N bytes for unknown frame types (RFC 9113 §4.1) */
} hive_recv_state_t;

#endif /* HIVE_FRAME_H */

