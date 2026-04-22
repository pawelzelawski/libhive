# Architecture

## 1. Library Overview

### 1.1 Design Model

Hive is a protocol engine, not an I/O library. It owns no sockets, no file
descriptors, no threads, and no TLS state. The caller owns all of those. The
library owns HTTP/2 protocol processing — frame parsing, HPACK compression,
stream state management, flow control, and frame serialisation.

```
caller's network layer (TLS / plain socket)
        │
        │  raw bytes (after TLS decrypt)
        ▼
hive_session_recv(session, buf, n)
        │
        ├── frame header parsed from buf
        │
        ├── HEADERS frame ──► hpack_decode_block()
        │       ├── on_begin_headers(session, stream_id, user_data)
        │       ├── on_header(session, stream_id, &name, &value, flags, user_data)
        │       │       (one call per decoded header pair — handles passed by pointer)
        │       └── on_headers_complete(session, stream_id, end_stream, user_data)
        │
        ├── DATA frame ─────► on_data_chunk(session, stream_id, data, len, user_data)
        │       (fires for available bytes — may fire multiple times per frame)
        │       (data pointer is into caller's buf — zero copy)
        │
        ├── SETTINGS frame ──► apply peer settings; queue SETTINGS ACK
        │       ├── on_settings(session, user_data)  [after ACK queued]
        │
        ├── SETTINGS ACK ────► match pending outbound SETTINGS ring
        │       └── on_settings_ack(session, user_data)
        │
        ├── WINDOW_UPDATE ───► update flow control send windows
        │
        ├── PING frame ──────► queue PING ACK if auto-ping-ack enabled
        │       ├── on_ping(session, opaque_data, user_data)  [if auto-ACK disabled]
        │
        ├── PING ACK ────────► on_ping_ack(session, opaque_data, user_data)
        │
        ├── RST_STREAM ──────► on_stream_close(session, stream_id, error_code, user_data)
        │
        ├── GOAWAY ──────────► on_goaway(session, last_stream_id, error_code, ...)
        │       └── on_stream_close for streams with id > last_stream_id
        │
        └── protocol error ──► on_connection_error(session, err, h2_code, user_data)
                               followed by GOAWAY
        [return bytes_consumed]

caller calls hive_submit_response(session, stream_id, nva, nvlen, &data_source)
        │
        └── HEADERS frame serialised into send_buf; iovec entries added
            data_source stored in stream slot — read_callback called during send

hive_session_send(session)
        │
        ├── drives pending data_source streams (within flow control limits)
        │
        └── fires send(session, iov, iovcnt, user_data) — once per call
                │
                └── returns ssize_t bytes_written
                    library retains any unsent tail for next call
                    iov covers: SETTINGS ACK + WINDOW_UPDATE +
                                HEADERS + DATA header + DATA body (zero copy)

caller calls writev(fd, iov, iovcnt) or tls_write equivalent
```

The send callback returns the number of bytes actually written. The library
retains any unsent portion and resumes on the next `hive_session_send()` call.

### 1.2 Session Lifecycle

```
hive_session_server_new()   or   hive_session_client_new()
        │
        ├── allocate hive_session_t from caller's allocator
        ├── pre-allocate all sub-buffers from same allocator
        │     stream_hash, stream_slots, stream_free_stack,
        │     send_iov, send_buf, reassembly_buf,
        │     hpack_scratch_name, hpack_scratch_value,
        │     enc_table.ring, dec_table.ring,
        │     pending_settings ring (outbound SETTINGS awaiting ACK)
        │
        ├── server role: queue SETTINGS frame in send queue
        │   client role: queue 24-byte client preface + SETTINGS in send queue
        │
        └── return session pointer  (NULL on allocation failure)

[caller calls hive_session_send() to flush the connection preface]

[event loop runs: recv / submit / send per iteration]

hive_session_free(session)
        │
        └── free all sub-buffers, then free session struct
            (with arena allocator: single free of the entire arena block)
```

### 1.3 Thread Safety

A session is **not thread-safe**. The caller must ensure that
`hive_session_recv()`, `hive_session_send()`, `hive_submit_*()`, and
`hive_session_free()` are never called concurrently on the same session.
The intended model is one session per connection, one connection owned by
one event loop thread. No internal locking exists and none will be added.

---

## 2. Session Struct

`hive_session_t` is the root object. All state for one HTTP/2 connection
lives here or in blocks pointed to from here. There is no global state.

The struct is partitioned into logical regions for clarity. Fields marked
with `*` are pointers to separately allocated arena blocks; all others are
embedded directly in the struct.

### 2.1 Region A — Identity and Callbacks (read-only after init)

```c
hive_mem_t        mem;           /* allocator — copied from caller at creation */
hive_callbacks_t  callbacks;     /* copied from caller at creation */
void             *user_data;     /* passed unchanged to every callback */
uint8_t           role;          /* HIVE_ROLE_SERVER or HIVE_ROLE_CLIENT */
```

These fields are set at session creation and never modified. The allocator
and callbacks structs are copied by value — the caller does not need to keep
them alive after `hive_session_*_new()` returns.

### 2.2 Region B — Options

All option values are copied from the `hive_options_t` at session creation
and stored as flat fields. Changing a `hive_options_t` after session creation
has no effect on existing sessions.

```c
uint32_t opt_header_table_size;       /* SETTINGS_HEADER_TABLE_SIZE we advertise, default 4096 */
uint32_t opt_enable_push;             /* SETTINGS_ENABLE_PUSH we advertise (client role only), default 1 */
uint32_t opt_max_concurrent_streams;  /* SETTINGS_MAX_CONCURRENT_STREAMS we advertise, default 100 */
uint32_t opt_initial_window_size;     /* SETTINGS_INITIAL_WINDOW_SIZE we advertise, default 65535 */
uint32_t opt_max_frame_size;          /* SETTINGS_MAX_FRAME_SIZE we advertise, default 16384 */
uint32_t opt_max_header_list_size;    /* decoded header list limit, default 65536 */
uint32_t opt_max_header_count;        /* max headers per block, default 100 */
uint32_t opt_max_continuation_size;   /* reassembly cap, default opt_max_frame_size × 4 */
uint32_t opt_max_settings_pending;    /* max unACK'd outbound SETTINGS, default 3 */
uint32_t opt_rst_flood_threshold;     /* RST_STREAM rate limit, default 100 */
uint32_t opt_rst_flood_window_secs;   /* RST_STREAM rate window, default 10 */
uint32_t opt_max_send_iov;            /* iovec array size, default 512 */
uint32_t opt_max_header_string_size;  /* Huffman scratch size per buffer, default 8192 */
uint8_t  opt_no_http_messaging;       /* 0 = validate per RFC 9113 §8, default 0 */
uint8_t  opt_no_auto_ping_ack;        /* 0 = auto-ACK PING frames, default 0 */
```

**SETTINGS_ENABLE_PUSH directionality**: `opt_enable_push` is only included
in the outbound SETTINGS frame for client-role sessions. RFC 9113 §6.5.2
prohibits servers from sending SETTINGS_ENABLE_PUSH=1; a conformant client
will treat that as a connection error PROTOCOL_ERROR. Server-role sessions
must never include this parameter in their outbound SETTINGS frame regardless
of the opt_enable_push value.

### 2.3 Region C — Connection State

```c
uint8_t  session_state;               /* hive_session_state_t enum */
uint8_t  goaway_sent;                 /* 1 = any GOAWAY queued or sent */
uint8_t  goaway_prepare_sent;         /* 1 = prepare-phase GOAWAY (0x7FFFFFFF) sent */
uint8_t  goaway_recv;                 /* 1 = GOAWAY received from peer */
uint32_t last_stream_id_local;        /* highest stream ID we have opened */
uint32_t last_stream_id_remote;       /* highest stream ID peer has opened */
uint32_t next_stream_id;              /* next ID to assign (client: odd; server: even) */
uint32_t goaway_last_stream_id_sent;  /* last_stream_id in our most recent GOAWAY */
uint32_t goaway_last_stream_id_recv;  /* last_stream_id received in peer's GOAWAY */
uint32_t goaway_error_code_recv;      /* error_code from peer's GOAWAY; persists across RECV_GOAWAY_DEBUG */
```

Session state values:
```c
typedef enum {
    HIVE_SESSION_OPEN         = 0,
    HIVE_SESSION_GOAWAY_SENT  = 1,  /* we sent GOAWAY, draining in-flight */
    HIVE_SESSION_GOAWAY_RECV  = 2,  /* peer sent GOAWAY, no new streams */
    HIVE_SESSION_CLOSED       = 3,  /* session is dead, free it */
} hive_session_state_t;
```

### 2.4 Region D — Connection-Level Flow Control

```c
int32_t  send_window;    /* our send budget — peer's recv window; decrements as we send DATA */
int32_t  recv_window;    /* peer's send budget — our recv window; decrements as peer sends DATA */
uint32_t recv_consumed;  /* bytes received but not yet acknowledged via WINDOW_UPDATE */
```

`send_window` starts at 65535 (RFC default initial window size) and is updated
by received WINDOW_UPDATE frames. `recv_window` tracks how much DATA the peer
is permitted to send; it decrements on DATA receipt and increments when we send
WINDOW_UPDATE. If the peer sends DATA that would push `recv_window` below zero,
Hive returns FLOW_CONTROL_ERROR (§8.7). `recv_consumed` drives WINDOW_UPDATE
coalescing (§7.7): a WINDOW_UPDATE is queued when it exceeds `recv_window / 2`.

`recv_window` is restored by the WINDOW_UPDATE increment when the update is
queued into the send buffer (queue-time restoration). See §7.7 for the
coalescing implementation and the rationale for this timing choice.

### 2.5 Region E — SETTINGS State

```c
hive_settings_t  local_settings;     /* our current effective SETTINGS (24 bytes) */
hive_settings_t  remote_settings;    /* peer's current effective SETTINGS (24 bytes) */
hive_settings_t *pending_settings;   /* ring of outbound SETTINGS we sent awaiting ACK */
uint8_t          pending_head;        /* oldest entry in ring (next to be ACK'd) */
uint8_t          pending_tail;        /* where next outbound entry goes */
uint8_t          pending_count;       /* number of outbound SETTINGS awaiting ACK */
uint8_t          inbound_settings_count; /* inbound SETTINGS received, not yet ACK'd */
```

`pending_settings` ring tracks **outbound** SETTINGS frames we have sent and
for which we have not yet received a SETTINGS ACK. This is used to match
incoming ACKs and fire `on_settings_ack`. Size: `opt_max_settings_pending ×
sizeof(hive_settings_t)`, pre-allocated from the session arena.

`pending_head` and `pending_tail` must be advanced with modulo arithmetic:
`pending_head = (pending_head + 1) % opt_max_settings_pending` and equivalently
for `pending_tail`. Plain increment without modulo will access out-of-bounds
memory after the ring wraps.

`inbound_settings_count` tracks **inbound** SETTINGS frames received but not
yet ACK'd. This is the flood protection counter (§8.4). It is a separate
single counter, not a ring. It is decremented when the SETTINGS ACK is queued
into the send buffer (not when transmitted, not at receipt). With
`opt_max_settings_pending = 3` (default), flood protection allows at most 3
unACK'd inbound SETTINGS at any time.

```c
typedef struct {
    uint32_t header_table_size;
    uint32_t enable_push;
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_header_list_size;
} hive_settings_t;  /* 24 bytes */
```

**SETTINGS directionality**:
- `local_settings` = what we have advertised to the peer. Incoming frames are
  validated against `local_settings.max_frame_size` — the limit we told the
  peer we can accept.
- `remote_settings` = what the peer has advertised to us. Outgoing frames are
  sized to honor `remote_settings.max_frame_size` — the limit the peer told
  us it can accept.
- `remote_settings.header_table_size` bounds our **encoder** dynamic table
  (`enc_table.max_size`): we must not produce a table the peer cannot decode.
- `local_settings.header_table_size` bounds our **decoder** dynamic table
  (`dec_table.max_size`): this is the limit we advertised the peer must respect.

### 2.6 Region F — RST_STREAM Flood Detection

```c
uint32_t rst_flood_count;          /* RST_STREAM frames received in current window */
uint64_t rst_flood_window_start;   /* monotonic seconds (CLOCK_MONOTONIC) when window started */
```

Updated on every received RST_STREAM. See §8.5 for the flood detection logic.

### 2.7 Region G — Frame Receive State Machine

```c
uint8_t       frame_hdr_buf[9];      /* 9-byte frame header staging */
uint8_t       frame_hdr_count;       /* bytes accumulated so far (0–9) */
uint8_t       preface_count;         /* bytes of client/server preface consumed so far */
uint8_t       recv_state;            /* hive_recv_state_t enum */
frame_hdr_t   cur_frame;             /* parsed from frame_hdr_buf once complete */
uint32_t      payload_remaining;     /* bytes left in current frame payload */
uint32_t      pad_remaining;         /* padding bytes left to skip */
uint8_t       pad_length_received;   /* 1 = pad_length byte has been extracted for cur frame */
uint8_t       pad_validated;         /* 1 = pad_length validated after all fixed-prefix fields */
uint8_t       fc_accounted;          /* 1 = flow-control windows decremented for cur DATA frame */
uint32_t      reassembly_stream_error_code; /* non-zero = stream was in illegal state when HEADERS
                                              * arrived; carry RST code through to END_HEADERS */
uint8_t       ctrl_staging[8];       /* small control frame byte accumulator */
uint8_t       ctrl_staging_count;    /* bytes in ctrl_staging */
uint32_t      reassembly_stream_id;  /* stream_id of active HEADERS/PUSH_PROMISE reassembly */
uint32_t      reassembly_promised_stream_id; /* promised stream_id from PUSH_PROMISE (fragmented path) */
uint8_t       reassembly_type;       /* 0 = HEADERS reassembly, 1 = PUSH_PROMISE reassembly */
uint8_t       reassembly_end_stream; /* END_STREAM flag from opening HEADERS frame */
uint8_t       reassembly_active;     /* 1 = HEADERS/PUSH_PROMISE block in progress (no END_HEADERS yet) */
uint32_t      reassembly_len;        /* bytes written into reassembly_buf so far */
uint8_t      *reassembly_buf;        /* pre-allocated, opt_max_continuation_size bytes */
uint8_t       priority_payload_len;  /* bytes of PRIORITY prefix remaining in HEADERS payload */
```

The `frame_hdr_buf` / `frame_hdr_count` pair handles frame header arrival
across multiple `hive_session_recv()` calls without any dynamic allocation.
The 9-byte frame header is assembled here, then parsed into `cur_frame` which
drives the state transition. See §3 for the full state machine.

`preface_count` is used by both RECV_CLIENT_PREFACE (server role: counting
the 24-byte client magic) and RECV_SERVER_PREFACE (client role: the first
frame from the server must be a SETTINGS frame).

`ctrl_staging[8]` covers every fixed-structure small control frame payload:
PING (8 bytes), GOAWAY first 8 bytes, RST_STREAM (4 bytes), WINDOW_UPDATE
(4 bytes), SETTINGS parameters (6 bytes per parameter), PRIORITY (5 bytes).
The same buffer is reused for every frame; `ctrl_staging_count` resets to 0
at each state transition.

`reassembly_active` is set to 1 when a HEADERS or PUSH_PROMISE frame without
END_HEADERS is received, and cleared when the final CONTINUATION frame with
END_HEADERS is processed. It is the authoritative flag for CONTINUATION lockout
(§3.5). Note: a HEADERS frame with a zero-length compressed payload is valid;
`reassembly_len` would be 0 in that case but `reassembly_active` would still
be 1, indicating a CONTINUATION is expected.

`priority_payload_len` persists the number of PRIORITY prefix bytes remaining
in the current HEADERS payload across multiple recv calls. Set in
RECV_FRAME_HEADER when a HEADERS frame with the PRIORITY flag is parsed; reset
to 0 when the prefix is fully consumed in RECV_HEADERS_PAYLOAD.

`pad_length_received` is set to 1 once the Pad Length byte has been read from
the first byte of a PADDED frame's payload in RECV_DATA_PAYLOAD,
RECV_HEADERS_PAYLOAD, or RECV_PUSH_PROMISE_PAYLOAD. Reset to 0 at every
RECV_FRAME_HEADER transition. Without this flag, a split delivery that suspends
after reading the Pad Length byte would re-read it on the next call.

`pad_validated` is set to 1 once the pad_length value has been validated
against the remaining payload after all fixed-prefix fields (PRIORITY prefix
in HEADERS, promised stream ID in PUSH_PROMISE) are consumed. Validation is
deferred to this point because those fixed fields reduce available space and
must be excluded from the comparison. Reset to 0 at every RECV_FRAME_HEADER
transition. For DATA frames (no fixed prefix), pad_length is validated
immediately after extraction.

`reassembly_promised_stream_id` stores the promised stream ID extracted from a
PUSH_PROMISE frame when the header block is fragmented (no END_HEADERS). Set in
RECV_PUSH_PROMISE_PAYLOAD when the promised_stream_id is first parsed; used
when the final CONTINUATION frame with END_HEADERS fires `on_push_promise`.
Reset to 0 at RECV_FRAME_HEADER entry only when `reassembly_active == 0` (no
ongoing reassembly). When `reassembly_active == 1`, the field must be preserved
across the RECV_FRAME_HEADER visits for each CONTINUATION frame, so that
RECV_CONTINUATION_PAYLOAD can use it when END_HEADERS arrives.

`reassembly_end_stream` stores the END_STREAM flag from the opening HEADERS
frame when the header block is fragmented (no END_HEADERS). Set in
RECV_HEADERS_PAYLOAD at the point of transitioning to
RECV_CONTINUATION_PAYLOAD. Used after the final CONTINUATION to determine the
correct stream state transition. Not used for PUSH_PROMISE reassembly (push
streams are never closed by END_STREAM in the HEADERS block).

`fc_accounted` is set to 1 when flow-control windows have been decremented for
the current DATA frame (by `cur_frame.length`, covering the full payload
including padding). Reset to 0 at every RECV_FRAME_HEADER transition. Ensures
the window decrement and enforcement check happen exactly once per frame
regardless of how many `hive_session_recv()` calls are needed to deliver it.

`reassembly_stream_error_code` carries the RST_STREAM error code when a HEADERS
block arrives on a stream in an illegal state (HALF_CLOSED_REMOTE, RESERVED_LOCAL,
or CLOSED). The block is still reassembled and decoded for HPACK table synchronization,
but callback delivery is suppressed via `stream_error_pending`. When END_HEADERS
arrives, the stored code drives the RST_STREAM and stream_close(). Zero means no
error. Reset to 0 after the error is processed.
0 = HEADERS, 1 = PUSH_PROMISE. Set when reassembly_active is set. Used in
RECV_CONTINUATION_PAYLOAD to choose the correct END_HEADERS completion path —
HEADERS fires on_headers_complete; PUSH_PROMISE fires on_push_promise with
reassembly_promised_stream_id.

### 2.8 Region H — HPACK State

```c
hpack_table_t  enc_table;          /* encoder dynamic table */
hpack_table_t  dec_table;          /* decoder dynamic table */
uint8_t       *hpack_scratch_name; /* Huffman decode scratch for header name, opt_max_header_string_size bytes */
uint8_t       *hpack_scratch_value;/* Huffman decode scratch for header value, opt_max_header_string_size bytes */
```

Both tables are embedded directly in the session struct. See §4 for the
full `hpack_table_t` layout and operation.

`enc_table.max_size` is bounded by `remote_settings.header_table_size` —
the encoder must not exceed the table size the peer can handle.
`dec_table.max_size` is bounded by `local_settings.header_table_size` —
the decoder's table matches what we advertised.

Two separate Huffman decode scratch buffers are required because a single
header field can have both a Huffman-encoded name and a Huffman-encoded value,
and both decoded strings must remain valid simultaneously when the `on_header`
callback fires. Using a single buffer would cause the name decode to be
overwritten when the value is decoded. Each buffer is `opt_max_header_string_size`
bytes; the default is 8192 bytes per buffer.

### 2.9 Region I — Stream Table

```c
stream_hash_entry_t *stream_hash;        /* hash table, hash_table_size entries */
hive_stream_t       *stream_slots;       /* slot array, opt_max_concurrent_streams entries */
uint32_t            *stream_free_stack;  /* free slot indices, opt_max_concurrent_streams */
uint32_t             stream_hash_mask;   /* hash_table_size - 1 */
uint32_t             stream_free_top;    /* index of top of free stack */
uint32_t             stream_open_count;  /* total currently open streams (all initiators) */
uint32_t             peer_stream_open_count; /* open streams initiated by the peer (enforced against opt_max_concurrent_streams) */
uint32_t             tombstone_count;    /* hash entries in TOMBSTONE state */
uint32_t             closes_since_compact; /* stream closes since last compaction */
```

`stream_open_count` tracks all open streams regardless of who initiated them.
`peer_stream_open_count` tracks only streams initiated by the peer (odd IDs
for server role, even IDs for client role) that are in OPEN or either
HALF_CLOSED state. This is what is compared against `opt_max_concurrent_streams`
when the peer opens a new stream — RFC 9113 §5.1.2 specifies that
SETTINGS_MAX_CONCURRENT_STREAMS is directional and constrains only the peer's
ability to open streams, not locally initiated streams.

`stream_slots` is sized to `opt_max_concurrent_streams`. This means the slot
pool is a total cap on all simultaneously open streams from both directions
combined, not just peer-initiated streams. This is a deliberate stricter-than-RFC
constraint: Hive does not resize the slot pool after receiving the peer's
SETTINGS_MAX_CONCURRENT_STREAMS. If the total of peer-initiated and locally
initiated open streams would exceed `opt_max_concurrent_streams`, any new stream
(regardless of direction) is refused with REFUSED_STREAM. Callers that need
true bidirectional concurrency at the RFC maximum must set
`opt_max_concurrent_streams` to accommodate streams from both sides.

See §5 for the full two-layer stream table design.

### 2.10 Region J — Send Queue and Partial Send State

```c
struct iovec *send_iov;           /* iovec array, opt_max_send_iov entries */
int           send_iov_count;     /* entries currently queued */
uint8_t      *send_buf;           /* frame serialisation buffer */
size_t        send_buf_used;      /* bytes written; reset to 0 after full send */
size_t        send_buf_cap;       /* see below */
size_t        send_partial_offset;/* total bytes already sent from current batch */
uint8_t       send_partial;       /* 1 = unsent tail remains from previous send call */
```

`send_buf_cap` is sized to hold a full HEADERS block plus CONTINUATION frame
headers in the worst case:
```
send_buf_cap = opt_max_continuation_size
             + ceil(opt_max_continuation_size / 16384) * 9
             + 2048  /* headroom for control frames and alignment */
```
The formula uses 16384 because that is both the RFC minimum and the
session-creation default for `remote_settings.max_frame_size`. At defaults:
65536 + 4×9 + 2048 = 67620 bytes.

`send_partial_offset` tracks how many bytes of the current iov batch have
already been sent in a previous partial write. On the next `hive_session_send()`
call, the library advances through the iov array to skip `send_partial_offset`
bytes before calling the send callback again. Reset to 0 on full send.

---

## 3. Receive Path

### 3.1 State Machine States

```c
typedef enum {
    RECV_CLIENT_PREFACE,        /* server role: consume 24-byte PRI * magic */
    RECV_SERVER_PREFACE,        /* client role: first frame must be SETTINGS */
    RECV_FRAME_HEADER,          /* accumulate frame_hdr_buf[9] via frame_hdr_count */
    RECV_DATA_PAYLOAD,          /* streaming DATA delivery into caller's buffer */
    RECV_DATA_PAD,              /* skip padding bytes after DATA payload */
    RECV_HEADERS_PAYLOAD,       /* copy into reassembly_buf */
    RECV_HEADERS_PAD,           /* skip padding bytes after HEADERS payload */
    RECV_CONTINUATION_PAYLOAD,  /* continuation; any other frame = connection error */
    RECV_PUSH_PROMISE_PAYLOAD,  /* extract 4-byte promised stream_id, copy remainder */
    RECV_PUSH_PROMISE_PAD,      /* skip padding bytes after PUSH_PROMISE payload */
    RECV_SETTINGS_PAYLOAD,      /* process 6-byte parameters via ctrl_staging */
    RECV_PING_PAYLOAD,          /* accumulate 8 bytes via ctrl_staging */
    RECV_RST_STREAM_PAYLOAD,    /* accumulate 4 bytes via ctrl_staging */
    RECV_WINDOW_UPDATE_PAYLOAD, /* accumulate 4 bytes via ctrl_staging */
    RECV_GOAWAY_PAYLOAD,        /* accumulate first 8 bytes; collect debug data */
    RECV_GOAWAY_DEBUG,          /* collect optional GOAWAY debug bytes into reassembly_buf */
    RECV_PRIORITY_PAYLOAD,      /* accumulate 5 bytes; ignore and discard */
    RECV_SKIP_PAYLOAD,          /* skip N bytes (unknown frame types, RFC 9113 §4.1) */
} hive_recv_state_t;
```

### 3.2 Frame Header Structure

```c
typedef struct {
    uint32_t length;    /* 24-bit payload length, network byte order → host */
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id; /* R bit masked off */
} frame_hdr_t;
```

This is a stack-local value populated from `frame_hdr_buf[9]` once all 9
bytes have accumulated. It is not stored in the session struct beyond the
`cur_frame` field.

Frame type constants:
```c
#define HIVE_FRAME_DATA          0x0
#define HIVE_FRAME_HEADERS       0x1
#define HIVE_FRAME_PRIORITY      0x2
#define HIVE_FRAME_RST_STREAM    0x3
#define HIVE_FRAME_SETTINGS      0x4
#define HIVE_FRAME_PUSH_PROMISE  0x5
#define HIVE_FRAME_PING          0x6
#define HIVE_FRAME_GOAWAY        0x7
#define HIVE_FRAME_WINDOW_UPDATE 0x8
#define HIVE_FRAME_CONTINUATION  0x9
```

### 3.3 Processing Loop

`hive_session_recv()` is a single loop over the caller's input buffer. The
caller's buffer is never modified. Bytes are read in-place (DATA path) or
copied into specific pre-allocated locations (reassembly, ctrl_staging).

```
hive_session_recv(session, data, len):

  consumed = 0

  while consumed < len:

    switch recv_state:

      RECV_CLIENT_PREFACE:   [server role only]
        n = min(24 - preface_count, len - consumed)
        compare data[consumed..+n] against
            "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
        consumed += n; preface_count += n
        if preface_count == 24:
          if mismatch: return session_error(HIVE_ERR_PROTOCOL, ...)
          preface_count = 1  /* flag: next frame must be non-ACK SETTINGS */
          transition RECV_FRAME_HEADER

      RECV_SERVER_PREFACE:   [client role only]
        /* First frame from server must be SETTINGS (non-ACK) per RFC 9113 §3.4 */
        /* Delegate to RECV_FRAME_HEADER; check is in RECV_FRAME_HEADER transition */
        preface_count = 1  /* flag: still expecting initial server SETTINGS */
        transition RECV_FRAME_HEADER

      RECV_FRAME_HEADER:
        n = min(9 - frame_hdr_count, len - consumed)
        copy data[consumed..+n] → frame_hdr_buf[frame_hdr_count..]
        frame_hdr_count += n; consumed += n
        if frame_hdr_count == 9:
          parse cur_frame from frame_hdr_buf (network byte order)
          frame_hdr_count = 0

          /* Preface first-frame check (both roles use preface_count == 1 flag) */
          if preface_count == 1:
            if cur_frame.type != SETTINGS || (cur_frame.flags & ACK):
              return session_error(HIVE_ERR_PROTOCOL, ...)
            preface_count = 0

          /* CONTINUATION lockout — checked before anything else */
          /* Uses reassembly_active, not reassembly_len, to handle zero-length blocks */
          if reassembly_active &&
              (cur_frame.type != HIVE_FRAME_CONTINUATION ||
               cur_frame.stream_id != reassembly_stream_id):
            return session_error(s, HIVE_ERR_PROTOCOL,
                HIVE_H2_PROTOCOL_ERROR, 0)  /* connection error */
          if !reassembly_active && cur_frame.type == HIVE_FRAME_CONTINUATION:
            return session_error(s, HIVE_ERR_PROTOCOL,
                HIVE_H2_PROTOCOL_ERROR, 0)  /* connection error */

          /* Validate inbound frame length against what WE advertised (local_settings) */
          validate: cur_frame.length <= local_settings.max_frame_size
            → else: FRAME_SIZE_ERROR connection error

          /* Fixed-length frame validation */
          switch cur_frame.type:
            SETTINGS with ACK flag: length must be 0 → else FRAME_SIZE_ERROR
            SETTINGS without ACK:   length must be multiple of 6 → else FRAME_SIZE_ERROR
            PING:           length must be 8  → else FRAME_SIZE_ERROR
            RST_STREAM:     length must be 4  → else FRAME_SIZE_ERROR
            WINDOW_UPDATE:  length must be 4  → else FRAME_SIZE_ERROR
            PRIORITY:       length must be 5  → else FRAME_SIZE_ERROR
            GOAWAY:         length must be >= 8 → else FRAME_SIZE_ERROR
            PUSH_PROMISE:   if PADDED flag: length must be >= 5
                            else:           length must be >= 4
                            → else FRAME_SIZE_ERROR
            DATA:           if PADDED flag: length must be >= 1
                            (Pad Length field must be present)
                            → else FRAME_SIZE_ERROR
            HEADERS:        if PADDED and PRIORITY: length must be >= 6
                            else if PRIORITY only:  length must be >= 5
                            else if PADDED only:    length must be >= 1
                            (PRIORITY prefix = 5 bytes, Pad Length field = 1 byte)
                            → else FRAME_SIZE_ERROR

          /* Stream-id vs frame-type validation */
          DATA, HEADERS, RST_STREAM, PUSH_PROMISE, CONTINUATION:
            stream_id must be != 0 → else PROTOCOL_ERROR connection error
          PRIORITY:
            stream_id must be != 0 → else PROTOCOL_ERROR connection error
            (RFC 9113 §6.3: PRIORITY on stream 0 is a connection error)
          SETTINGS, PING, GOAWAY:
            stream_id must be == 0 → else PROTOCOL_ERROR connection error
          WINDOW_UPDATE: stream_id = 0 is connection-level, any other is stream-level

          /* HEADERS with PRIORITY flag: persist priority prefix byte count */
          if type == HEADERS && (flags & HIVE_FLAG_PRIORITY):
            priority_payload_len = 5  /* consumed first from payload in RECV_HEADERS_PAYLOAD */

          set payload_remaining = cur_frame.length
          /* NOTE: pad_length is a payload byte, not a frame header byte.
           * It is NOT extracted here. It is extracted as the first byte of
           * the appropriate payload state (RECV_DATA_PAYLOAD,
           * RECV_HEADERS_PAYLOAD, RECV_PUSH_PROMISE_PAYLOAD) using the
           * ctrl_staging accumulation pattern. */
          transition to appropriate RECV_*_PAYLOAD state

      RECV_DATA_PAYLOAD:
        /* First, establish stream validity before any field dereference.
         * Parity-aware idle vs closed classification: a stream is idle only if
         * its ID has never been opened by the appropriate initiator. Locally-
         * initiated streams (server push on server, client requests on client)
         * are tracked by last_stream_id_local; peer-initiated streams are
         * tracked by last_stream_id_remote. Using only last_stream_id_remote
         * would misclassify locally-initiated closed streams as idle. */
        stream = lookup stream slot for cur_frame.stream_id
        if stream == NULL:
          locally_initiated = (role == HIVE_ROLE_SERVER) ?
              (cur_frame.stream_id % 2 == 0) :   /* server push: even = server-initiated */
              (cur_frame.stream_id % 2 == 1)      /* client req: odd = client-initiated */
          if locally_initiated:
            is_idle = (cur_frame.stream_id > last_stream_id_local)
          else:
            is_idle = (cur_frame.stream_id > last_stream_id_remote)
          if is_idle:
            /* DATA on idle stream — connection error per RFC 9113 §5.1 */
            return session_error(s, HIVE_ERR_PROTOCOL,
                HIVE_H2_PROTOCOL_ERROR, 0)
          else:
            /* Recently closed — RST_STREAM STREAM_CLOSED.
             * RFC 9113 §6.9.1: still account payload against connection window
             * since bytes were received on the wire. No per-stream accounting. */
            session->recv_window   -= cur_frame.length
            session->recv_consumed += cur_frame.length
            check WINDOW_UPDATE coalescing threshold (§7.7)
            n = min(payload_remaining, len - consumed)
            skip n bytes; consumed += n; payload_remaining -= n
            send RST_STREAM STREAM_CLOSED for cur_frame.stream_id
            if payload_remaining == 0: transition RECV_FRAME_HEADER
            break
        if stream->state != OPEN && stream->state != HALF_CLOSED_LOCAL:
          n = min(payload_remaining, len - consumed)
          skip n bytes; consumed += n; payload_remaining -= n
          /* RFC 9113 §5.1.1: reserved streams → PROTOCOL_ERROR stream error;
           * half-closed(remote)/closed → STREAM_CLOSED stream error.
           * RST_STREAM is terminal; close the stream locally in all cases.
           * RFC 9113 §6.9.1: still account payload against connection window;
           * omit per-stream accounting since the stream is being closed. */
          session->recv_window   -= cur_frame.length
          session->recv_consumed += cur_frame.length
          check WINDOW_UPDATE coalescing threshold (§7.7)
          rst_code = (stream->state == RESERVED_LOCAL ||
                      stream->state == RESERVED_REMOTE) ?
              HIVE_H2_PROTOCOL_ERROR : HIVE_H2_STREAM_CLOSED
          fire on_stream_close(session, cur_frame.stream_id, rst_code, user_data)
          stream_close(session, stream)
          send RST_STREAM rst_code for cur_frame.stream_id
          if payload_remaining > 0: transition RECV_SKIP_PAYLOAD
          else: transition RECV_FRAME_HEADER
          break

        /* Extract pad_length from first payload byte if PADDED flag set.
         * pad_length_received persists across split recv calls. */
        if (cur_frame.flags & HIVE_FLAG_PADDED) && !pad_length_received:
          if len - consumed == 0: break  /* wait for more input */
          pad_length = data[consumed++]; payload_remaining--
          /* SECURITY: pad_length must be strictly less than remaining payload.
           * RFC 9113 §6.1: pad_length > payload_remaining is invalid; equality
           * (pad_length == payload_remaining) means zero data bytes and is valid.
           * Use > not >= to allow zero-data-byte DATA frames. */
          if pad_length > payload_remaining:
            return stream_error(s, cur_frame.stream_id, HIVE_ERR_PROTOCOL,
                HIVE_H2_PROTOCOL_ERROR)
          pad_remaining = pad_length
          pad_length_received = 1

        n = min(payload_remaining - pad_remaining, len - consumed)

        /* Flow-control accounting covers the FULL frame payload per RFC 9113
         * §6.1 and §6.9.1, including Pad Length field and padding octets.
         * Enforcement and window decrement happen once per frame against
         * cur_frame.length via fc_accounted flag, not per partial chunk. */
        if !fc_accounted:
          if (uint32_t)cur_frame.length > (uint32_t)stream->recv_window:
            return stream_error(s, cur_frame.stream_id, HIVE_ERR_FLOW_CONTROL,
                HIVE_H2_FLOW_CONTROL_ERROR)
          if (uint32_t)cur_frame.length > (uint32_t)session->recv_window:
            return session_error(s, HIVE_ERR_FLOW_CONTROL,
                HIVE_H2_FLOW_CONTROL_ERROR, 0)
          stream->recv_window    -= cur_frame.length
          session->recv_window   -= cur_frame.length
          stream->recv_consumed  += cur_frame.length
          session->recv_consumed += cur_frame.length
          fc_accounted = 1
          check WINDOW_UPDATE coalescing threshold (§7.7)

        /* Content-Length tracking: increment by application data bytes only
         * (not padding). opt_no_http_messaging check deferred to END_STREAM. */
        stream->content_length_received += n

        cb_ret = fire on_data_chunk(session, stream_id, data + consumed, n, user_data)
        /* data + consumed is directly in caller's buffer — zero copy */
        /* on_data_chunk return value handling:
         * - HIVE_OK: continue normally
         * - any HIVE_ERR_*: stream error — send RST_STREAM CANCEL, reset stream.
         *   The current DATA frame bytes are already consumed; remaining payload
         *   bytes in this frame are skipped before transitioning. */
        if cb_ret != HIVE_OK:
          /* Skip remaining payload bytes before resetting */
          skip = min(payload_remaining - n, len - consumed - n)
          consumed += n + skip; payload_remaining -= n + skip
          send RST_STREAM CANCEL for cur_frame.stream_id
          stream = lookup(cur_frame.stream_id)
          if stream != NULL:
            fire on_stream_close(session, stream_id, HIVE_H2_CANCEL, user_data)
            stream_close(session, stream)
          if payload_remaining > 0: transition RECV_SKIP_PAYLOAD
          else: transition RECV_FRAME_HEADER
          break

        consumed += n; payload_remaining -= n
        if payload_remaining == 0:
          if pad_remaining > 0: transition RECV_DATA_PAD
          else:
            /* State transition and Content-Length check on END_STREAM */
            if cur_frame.flags & HIVE_FLAG_END_STREAM:
              /* SECURITY: Content-Length consistency per RFC 9113 §8.1.1 */
              if opt_no_http_messaging == 0 &&
                  stream->content_length_expected != -1 &&
                  stream->content_length_received !=
                      (uint64_t)stream->content_length_expected:
                fire on_stream_close(session, stream_id, HIVE_H2_PROTOCOL_ERROR, user_data)
                stream_close(session, stream)
                return stream_error(s, cur_frame.stream_id, HIVE_ERR_PROTOCOL,
                    HIVE_H2_PROTOCOL_ERROR)
              if stream->state == OPEN:
                stream->state = HALF_CLOSED_REMOTE
              else if stream->state == HALF_CLOSED_LOCAL:
                stream->state = CLOSED
                fire on_stream_close(session, stream_id, HIVE_H2_NO_ERROR, user_data)
                stream_close(session, stream)
            transition RECV_FRAME_HEADER

      RECV_DATA_PAD:
        n = min(pad_remaining, len - consumed)
        consumed += n; pad_remaining -= n
        if pad_remaining == 0:
          if cur_frame.flags & HIVE_FLAG_END_STREAM:
            /* Content-Length check before state transition */
            if opt_no_http_messaging == 0 &&
                stream->content_length_expected != -1 &&
                stream->content_length_received !=
                    (uint64_t)stream->content_length_expected:
              fire on_stream_close(session, stream_id, HIVE_H2_PROTOCOL_ERROR, user_data)
              stream_close(session, stream)
              return stream_error(s, cur_frame.stream_id, HIVE_ERR_PROTOCOL,
                  HIVE_H2_PROTOCOL_ERROR)
            if stream->state == OPEN:
              stream->state = HALF_CLOSED_REMOTE
            else if stream->state == HALF_CLOSED_LOCAL:
              stream->state = CLOSED
              fire on_stream_close(session, stream_id, HIVE_H2_NO_ERROR, user_data)
              stream_close(session, stream)
          transition RECV_FRAME_HEADER

      RECV_HEADERS_PAYLOAD:
        /* Stream state legality check — before padding extraction.
         * RFC 9113 §5.1/§5.1.1: HEADERS is valid in OPEN, HALF_CLOSED_LOCAL,
         * and RESERVED_REMOTE. All other states are stream errors.
         *
         * HPACK SYNCHRONIZATION: even for illegal states, the header block
         * must be fully reassembled and decoded (with delivery suppressed via
         * suppress_callbacks = 1) to keep the dynamic table synchronized with
         * the peer encoder per RFC 7541 §2.3.2. Skipping raw bytes would
         * desynchronize the table and corrupt all subsequent HEADERS.
         * Additionally, if END_HEADERS is absent, we must set reassembly_active
         * so that subsequent CONTINUATION frames are consumed correctly — if we
         * do not, the CONTINUATION lockout check would escalate a stream error
         * into a connection error. */
        stream = lookup(cur_frame.stream_id)
        if stream == NULL:
          /* Distinguish idle (never opened) from recently-closed (removed
           * from hash table by stream_close). New streams opening via HEADERS
           * are handled by §3.4 in RECV_FRAME_HEADER before this state.
           * Any HEADERS reaching RECV_HEADERS_PAYLOAD with NULL lookup means
           * the stream was either idle (peer protocol error) or recently closed
           * (should decode with suppressed callbacks and send RST_STREAM
           * STREAM_CLOSED for HPACK table synchronization). */
          locally_initiated = (role == HIVE_ROLE_SERVER) ?
              (cur_frame.stream_id % 2 == 0) :
              (cur_frame.stream_id % 2 == 1)
          if locally_initiated:
            is_idle = (cur_frame.stream_id > last_stream_id_local)
          else:
            is_idle = (cur_frame.stream_id > last_stream_id_remote)
          if is_idle:
            /* Idle stream — connection error per RFC 9113 §5.1 */
            return session_error(s, HIVE_ERR_PROTOCOL,
                HIVE_H2_PROTOCOL_ERROR, 0)
          else:
            /* Recently closed — decode with suppressed callbacks then RST */
            stream_error_pending_for_stream = cur_frame.stream_id
            stream_error_code               = HIVE_H2_STREAM_CLOSED
            /* Fall through to reassembly/decode below */
        else if stream->state == HALF_CLOSED_REMOTE ||
            stream->state == RESERVED_LOCAL  ||
            stream->state == CLOSED:
          error_code = (stream->state == RESERVED_LOCAL) ?
              HIVE_H2_PROTOCOL_ERROR : HIVE_H2_STREAM_CLOSED
          /* Set stream_error_pending to suppress callback delivery while
           * still decoding the full block for table synchronization. */
          stream_error_pending_for_stream = cur_frame.stream_id
          stream_error_code               = error_code
          /* Fall through to normal payload reassembly below.
           * RST_STREAM and stream_close() happen after hpack_decode_block()
           * completes. See end of unfragmented/fragmented paths below. */
          /* else: OPEN, HALF_CLOSED_LOCAL, RESERVED_REMOTE — proceed normally */

        /* Extract pad_length from first payload byte if PADDED flag set */
        if (cur_frame.flags & HIVE_FLAG_PADDED) && !pad_length_received:
          if len - consumed == 0: break
          pad_length = data[consumed++]; payload_remaining--
          pad_length_received = 1
          /* NOTE: pad_length validation must be deferred until after the
           * PRIORITY prefix is consumed (if PRIORITY flag is also set), since
           * the fixed-field bytes reduce available space for data and padding.
           * Do NOT validate here — validate after priority_payload_len is zero. */

        /* Skip PRIORITY prefix if PRIORITY flag was set (5 bytes: stream dep + weight) */
        if priority_payload_len > 0:
          skip = min(priority_payload_len, len - consumed, payload_remaining)
          priority_payload_len -= skip; consumed += skip
          payload_remaining -= skip

        /* SECURITY: validate pad_length after all fixed fields are consumed.
         * RFC 9113 §6.2: pad_length must be strictly less than remaining payload
         * (i.e. pad_length < payload_remaining). Equality means zero header bytes
         * which is valid. Use > not >= to allow zero-length compressed payload.
         * Check only when pad_length_received == 1 and priority_payload_len == 0. */
        if pad_length_received && priority_payload_len == 0 && !pad_validated:
          if pad_length > payload_remaining:
            return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)
          pad_remaining = pad_length
          pad_validated = 1

        n = min(payload_remaining - pad_remaining, len - consumed)
        /* SECURITY: CONTINUATION flood — connection error, not stream error */
        if reassembly_len + n > opt_max_continuation_size:
          return session_error(s, HIVE_ERR_PROTOCOL,
              HIVE_H2_PROTOCOL_ERROR, 0)
        copy data[consumed..+n] → reassembly_buf[reassembly_len..]
        reassembly_len += n; consumed += n; payload_remaining -= n
        if payload_remaining == pad_remaining:
          /* All header bytes consumed; padding bytes remain */
          if cur_frame.flags & HIVE_FLAG_END_HEADERS:
            /* Set reassembly_stream_id for the unfragmented path so
             * hpack_decode_block fires callbacks with the correct stream ID. */
            reassembly_stream_id = cur_frame.stream_id
            /* suppress_callbacks = 1 when stream was in illegal state;
             * hpack_decode_block will decode for table sync but suppress
             * on_begin_headers, on_header, and on_headers_complete. */
            suppress = (stream_error_pending_for_stream == cur_frame.stream_id) ? 1 : 0
            ret = hpack_decode_block(session, reassembly_buf, reassembly_len,
                suppress, cur_frame.stream_id)  /* error targets HEADERS stream */
            reassembly_len = 0
            reassembly_active = 0
            if ret == HIVE_ERR_COMPRESSION:
              return ret  /* connection error already sent by hpack_decode_block */
            /* If stream was in an illegal state, send RST_STREAM now that
             * the block is fully decoded (table synchronization complete). */
            if stream_error_pending_for_stream == cur_frame.stream_id:
              stream = lookup(cur_frame.stream_id)
              if stream != NULL:
                fire on_stream_close(session, cur_frame.stream_id,
                    stream_error_code, user_data)
                stream_close(session, stream)
              send RST_STREAM stream_error_code for cur_frame.stream_id
              stream_error_pending_for_stream = 0
              if pad_remaining > 0: transition RECV_HEADERS_PAD
              else: transition RECV_FRAME_HEADER
              break
            else if ret != HIVE_OK:
              /* For normal (non-suppress) errors: RST_STREAM sent and stream
               * closed inside hpack_decode_block targeting error_stream_id.
               * For suppress errors: caller handles RST via stream_error_pending_for_stream
               * block above — hpack_decode_block returned HIVE_ERR_PROTOCOL without
               * sending RST. Both paths transition back to RECV_FRAME_HEADER. */
              if pad_remaining > 0: transition RECV_HEADERS_PAD
              else: transition RECV_FRAME_HEADER
              break
            /* State transition on END_STREAM — use cur_frame.flags directly. */
            if cur_frame.flags & HIVE_FLAG_END_STREAM:
              stream = lookup(cur_frame.stream_id)
              if stream == NULL:
                /* Stale stream — closed during hpack_decode_block callbacks */
                if pad_remaining > 0: transition RECV_HEADERS_PAD
                else: transition RECV_FRAME_HEADER
                break
              /* Content-Length consistency for headers-only messages (no DATA). */
              if opt_no_http_messaging == 0 &&
                  stream->content_length_expected != -1 &&
                  stream->content_length_received !=
                      (uint64_t)stream->content_length_expected:
                fire on_stream_close(session, cur_frame.stream_id,
                    HIVE_H2_PROTOCOL_ERROR, user_data)
                stream_close(session, stream)
                return stream_error(s, cur_frame.stream_id, HIVE_ERR_PROTOCOL,
                    HIVE_H2_PROTOCOL_ERROR)
              if stream->state == OPEN:
                stream->state = HALF_CLOSED_REMOTE
              else if stream->state == HALF_CLOSED_LOCAL:
                stream->state = CLOSED
                fire on_stream_close(...)
                stream_close(session, stream)
              else if stream->state == RESERVED_REMOTE:
                stream->state = CLOSED
                fire on_stream_close(...)
                stream_close(session, stream)
            else:
              stream = lookup(cur_frame.stream_id)
              if stream != NULL && stream->state == RESERVED_REMOTE:
                stream->state = HALF_CLOSED_LOCAL
            if pad_remaining > 0: transition RECV_HEADERS_PAD
            else: transition RECV_FRAME_HEADER
          else:
            /* Header block continues in CONTINUATION frames.
             * Transition to RECV_FRAME_HEADER — the next bytes on the wire
             * are a new 9-byte CONTINUATION frame header, not payload bytes.
             * The CONTINUATION lockout in RECV_FRAME_HEADER enforces that the
             * next frame must be CONTINUATION for the same stream_id. */
            reassembly_stream_id  = cur_frame.stream_id
            reassembly_end_stream = (cur_frame.flags & HIVE_FLAG_END_STREAM) ? 1 : 0
            reassembly_type       = 0  /* HEADERS reassembly */
            reassembly_active     = 1
            /* Persist illegal-state error for RECV_CONTINUATION_PAYLOAD to
             * handle after decoding. reassembly_stream_error_code carries the
             * RST_STREAM code; non-zero means the stream was illegal. */
            reassembly_stream_error_code = stream_error_pending_for_stream ?
                stream_error_code : 0
            stream_error_pending_for_stream = 0
            if pad_remaining > 0: transition RECV_HEADERS_PAD
            else: transition RECV_FRAME_HEADER

      RECV_HEADERS_PAD:
        n = min(pad_remaining, len - consumed)
        consumed += n; pad_remaining -= n
        if pad_remaining == 0:
          /* Always return to RECV_FRAME_HEADER — the next frame (CONTINUATION
           * or otherwise) starts with a 9-byte header, not payload bytes.
           * If reassembly_active == 1, the CONTINUATION lockout in
           * RECV_FRAME_HEADER enforces that the next frame is CONTINUATION. */
          transition RECV_FRAME_HEADER

      RECV_CONTINUATION_PAYLOAD:
        [same copy and flood-cap logic as RECV_HEADERS_PAYLOAD, no padding handling]
        cur_frame.stream_id == reassembly_stream_id is enforced in RECV_FRAME_HEADER
        on END_HEADERS:
          /* suppress_callbacks when stream was in illegal state at block start */
          suppress = (reassembly_stream_error_code != 0) ? 1 : 0
          /* For PUSH_PROMISE reassembly (reassembly_type==1), HPACK decode errors
           * must target reassembly_promised_stream_id, not reassembly_stream_id.
           * Pass the correct error target based on reassembly_type. */
          err_target = (reassembly_type == 1) ?
              reassembly_promised_stream_id : reassembly_stream_id
          ret = hpack_decode_block(session, reassembly_buf, reassembly_len,
              suppress, err_target)
          reassembly_len = 0
          reassembly_active = 0
          if ret == HIVE_ERR_COMPRESSION:
            return ret  /* connection error already sent by hpack_decode_block */
          /* If stream was in an illegal state, RST_STREAM now that table is synced */
          if reassembly_stream_error_code != 0:
            stream = lookup(reassembly_stream_id)
            if stream != NULL:
              fire on_stream_close(session, reassembly_stream_id,
                  reassembly_stream_error_code, user_data)
              stream_close(session, stream)
            send RST_STREAM reassembly_stream_error_code for reassembly_stream_id
            reassembly_stream_error_code = 0
            transition RECV_FRAME_HEADER
            break
          if ret != HIVE_OK:
            /* Stream error: RST_STREAM sent and stream closed inside
             * hpack_decode_block. Transition back to RECV_FRAME_HEADER. */
            transition RECV_FRAME_HEADER
            break
          if reassembly_type == 0:
            /* HEADERS reassembly — apply END_STREAM state transition */
            if reassembly_end_stream:
              stream = lookup(reassembly_stream_id)
              if stream != NULL:
                /* Content-Length check for fragmented headers-only END_STREAM.
                 * Mirrors the unfragmented RECV_HEADERS_PAYLOAD check (D-37). */
                if opt_no_http_messaging == 0 &&
                    stream->content_length_expected != -1 &&
                    stream->content_length_received !=
                        (uint64_t)stream->content_length_expected:
                  fire on_stream_close(session, reassembly_stream_id,
                      HIVE_H2_PROTOCOL_ERROR, user_data)
                  stream_close(session, stream)
                  return stream_error(s, reassembly_stream_id, HIVE_ERR_PROTOCOL,
                      HIVE_H2_PROTOCOL_ERROR)
                if stream->state == OPEN:
                  stream->state = HALF_CLOSED_REMOTE
                else if stream->state == HALF_CLOSED_LOCAL:
                  stream->state = CLOSED
                  fire on_stream_close(...)
                  stream_close(session, stream)
                else if stream->state == RESERVED_REMOTE:
                  /* Fragmented push-response HEADERS with END_STREAM:
                   * reserved(remote) → closed per RFC 9113 §5.1.1 */
                  stream->state = CLOSED
                  fire on_stream_close(...)
                  stream_close(session, stream)
            else:
              /* No END_STREAM — check reserved(remote) transition */
              stream = lookup(reassembly_stream_id)
              if stream != NULL && stream->state == RESERVED_REMOTE:
                /* Fragmented push-response HEADERS without END_STREAM:
                 * reserved(remote) → half-closed(local) per RFC 9113 §5.1.1 */
                stream->state = HALF_CLOSED_LOCAL
          else:
            /* PUSH_PROMISE reassembly — fire on_push_promise with promised ID.
             * The promised-request headers were delivered to on_header under
             * reassembly_stream_id (the carrying stream). on_push_promise
             * provides the promised_stream_id so the caller can correlate. */
            cb_ret = fire on_push_promise(session, reassembly_stream_id,
                reassembly_promised_stream_id, user_data)
            if cb_ret == HIVE_ERR_REFUSED_STREAM:
              /* Caller rejected the push — close the reserved stream */
              send RST_STREAM REFUSED_STREAM for reassembly_promised_stream_id
              stream = lookup(reassembly_promised_stream_id)
              if stream != NULL:
                fire on_stream_close(session, reassembly_promised_stream_id,
                    HIVE_H2_REFUSED_STREAM, user_data)
                stream_close(session, stream)
          transition RECV_FRAME_HEADER

      RECV_PUSH_PROMISE_PAYLOAD:
        /* Role check: servers must not receive PUSH_PROMISE */
        if role == HIVE_ROLE_SERVER:
          return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)

        /* Push-disabled check */
        if opt_enable_push == 0:
          return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)

        /* Carrying stream state check */
        stream = lookup(cur_frame.stream_id)
        if stream == NULL || (stream->state != OPEN &&
            stream->state != HALF_CLOSED_LOCAL):
          return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)

        /* Extract pad_length if PADDED flag set */
        if (cur_frame.flags & HIVE_FLAG_PADDED) && !pad_length_received:
          if len - consumed == 0: break
          pad_length = data[consumed++]; payload_remaining--
          pad_length_received = 1
          /* NOTE: pad_length validation deferred until after the 4-byte
           * promised stream ID is consumed, since that fixed field reduces
           * available space. Do NOT validate here — validate after ctrl_staging
           * accumulation is complete. */

        /* First 4 bytes: reserved bit + promised stream_id */
        accumulate 4 bytes into ctrl_staging via ctrl_staging_count
        on ctrl_staging_count == 4:
          promised_stream_id = uint32 from ctrl_staging & 0x7FFFFFFF
          /* SECURITY: validate pad_length after promised stream ID consumed.
           * RFC 9113 §6.6: pad_length must be < remaining payload. The 4-byte
           * promised stream ID has been consumed from ctrl_staging but
           * payload_remaining -= 4 happens below. Validate against
           * payload_remaining - 4 so the comparison reflects the actual
           * remaining header-block space. Use > not >= (equality = zero
           * header bytes, which is valid). */
          if pad_length_received && !pad_validated:
            if pad_length > payload_remaining - 4:
              return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)
            pad_remaining = pad_length
            pad_validated = 1
          promised_stream_id = uint32 from ctrl_staging & 0x7FFFFFFF
          validate promised_stream_id: even, > last server stream seen
          /* Concurrency checks before opening the promised stream slot.
           * Same rules as §3.4 new-stream validation apply to push streams.
           * peer_stream_open_count covers server-initiated streams (pushes). */
          if peer_stream_open_count >= opt_max_concurrent_streams:
            /* Peer's push stream limit exceeded — RST the promised stream */
            send RST_STREAM REFUSED_STREAM for promised_stream_id
            /* Skip remaining payload bytes then return to RECV_FRAME_HEADER */
            [skip remainder into reassembly_buf; discard on END_HEADERS]
            break
          if stream_open_count >= opt_max_concurrent_streams:
            /* Total slot pool exhausted — RST the promised stream */
            send RST_STREAM REFUSED_STREAM for promised_stream_id
            [skip remainder into reassembly_buf; discard on END_HEADERS]
            break
          open promised_stream_id in RESERVED_REMOTE state
          peer_stream_open_count++
          stream_open_count++
          /* Persist fields needed for both single-frame and fragmented completion */
          reassembly_stream_id          = cur_frame.stream_id  /* carrying stream */
          reassembly_promised_stream_id = promised_stream_id
          reassembly_type               = 1  /* PUSH_PROMISE reassembly */
          ctrl_staging_count = 0
          payload_remaining -= 4
          /* Remaining bytes are compressed headers for the push request */
          [copy remainder (minus padding) into reassembly_buf as in RECV_HEADERS_PAYLOAD]
          on END_HEADERS (single-frame):
            ret = hpack_decode_block(session, reassembly_buf, reassembly_len,
                0, reassembly_promised_stream_id)
                /* error_stream_id = promised stream: HPACK decode errors on
                 * PUSH_PROMISE target the promised stream, not the carrying stream.
                 * RFC 9113 §6.6: the carrying stream is unaffected. */
            reassembly_len = 0
            reassembly_active = 0
            if ret == HIVE_ERR_COMPRESSION:
              return ret  /* connection error already sent */
            if ret != HIVE_OK:
              /* stream error RST_STREAM already sent; skip on_push_promise */
              if pad_remaining > 0: transition RECV_PUSH_PROMISE_PAD
              else: transition RECV_FRAME_HEADER
              break
            cb_ret = fire on_push_promise(session, reassembly_stream_id,
                reassembly_promised_stream_id, user_data)
            if cb_ret == HIVE_ERR_REFUSED_STREAM:
              send RST_STREAM REFUSED_STREAM for reassembly_promised_stream_id
              stream = lookup(reassembly_promised_stream_id)
              if stream != NULL:
                fire on_stream_close(session, reassembly_promised_stream_id,
                    HIVE_H2_REFUSED_STREAM, user_data)
                stream_close(session, stream)
          on !END_HEADERS (fragmented):
            reassembly_active = 1
            /* reassembly_type = 1 already set; CONTINUATION will complete via
             * RECV_CONTINUATION_PAYLOAD PUSH_PROMISE path */
          if pad_remaining > 0: transition RECV_PUSH_PROMISE_PAD
          else: transition RECV_FRAME_HEADER

      RECV_PUSH_PROMISE_PAD:
        n = min(pad_remaining, len - consumed)
        consumed += n; pad_remaining -= n
        if pad_remaining == 0: transition RECV_FRAME_HEADER

      RECV_SETTINGS_PAYLOAD:
        process in 6-byte chunks via ctrl_staging:
        n = min(6 - ctrl_staging_count, payload_remaining, len - consumed)
        copy n bytes → ctrl_staging[ctrl_staging_count..]
        ctrl_staging_count += n; consumed += n; payload_remaining -= n
        if ctrl_staging_count == 6:
          param_id  = uint16 from ctrl_staging[0..1] (big-endian)
          param_val = uint32 from ctrl_staging[2..5] (big-endian)
          apply known parameter to remote_settings; ignore unknown (RFC 9113 §6.5)

          SETTINGS_HEADER_TABLE_SIZE:
            remote_settings.header_table_size = param_val
            /* Track both the minimum reached and the final value for the
             * two-update requirement (RFC 7541 §6.3). If has_pending is
             * already set, preserve the lower of pending_min and param_val. */
            if !enc_table.has_pending:
              enc_table.pending_min = param_val
            else:
              enc_table.pending_min = min(enc_table.pending_min, param_val)
            enc_table.pending_max = param_val
            enc_table.has_pending = 1
            /* Encoder must emit size update(s) on next encode block */

          SETTINGS_ENABLE_PUSH:
            /* Value must be 0 or 1; any other value is a connection error */
            if param_val > 1:
              return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)
            /* A client receiving SETTINGS_ENABLE_PUSH=1 from a server is an error */
            /* (Servers must never send this parameter — see §2.2 and B-13) */
            if role == HIVE_ROLE_CLIENT && param_val == 1:
              return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0)
            remote_settings.enable_push = param_val

          SETTINGS_INITIAL_WINDOW_SIZE:
            validate <= 2^31-1 (→ FLOW_CONTROL_ERROR connection error)
            delta = (int32_t)param_val - (int32_t)remote_settings.initial_window_size
            remote_settings.initial_window_size = param_val
            /* Retroactively adjust send_window for all open streams */
            for each open stream s:
              new_window = (int64_t)s->send_window + delta
              if new_window > 0x7FFFFFFF:
                return session_error(session, HIVE_ERR_FLOW_CONTROL,
                    HIVE_H2_FLOW_CONTROL_ERROR, 0)  /* connection error per RFC 9113 §6.9.2 */
              s->send_window = (int32_t)new_window
              /* Note: negative stream windows are valid; DATA is blocked until
               * WINDOW_UPDATE makes the window positive again */

          SETTINGS_MAX_FRAME_SIZE:
            validate in [16384, 16777215] (→ PROTOCOL_ERROR connection error)
            remote_settings.max_frame_size = param_val

          SETTINGS_MAX_CONCURRENT_STREAMS:
            remote_settings.max_concurrent_streams = param_val
            /* No retroactive action needed: existing streams are unaffected.
             * hive_submit_request() and hive_submit_push_promise() check this
             * value before opening new streams. */

          SETTINGS_MAX_HEADER_LIST_SIZE:
            remote_settings.max_header_list_size = param_val
            /* Informs us of the peer's header list size limit for outbound
             * HEADERS. Does not affect inbound decoding (opt_max_header_list_size
             * governs our decoder). */

          ctrl_staging_count = 0

        if payload_remaining == 0:
          if ACK flag:
            /* Match against our outbound pending_settings ring */
            if pending_count == 0: return session_error(...PROTOCOL_ERROR...)  /* unsolicited ACK */
            pop pending_settings[pending_head]
            pending_head = (pending_head + 1) % opt_max_settings_pending
            pending_count--
            fire on_settings_ack(session, user_data)
          else:
            /* Inbound SETTINGS flood check (§8.4) */
            inbound_settings_count++
            if inbound_settings_count > opt_max_settings_pending:
              return session_error(HIVE_ERR_PROTOCOL,
                  HIVE_H2_PROTOCOL_ERROR, 0)  /* GOAWAY */
            queue SETTINGS ACK in send queue (9-byte frame, empty payload)
            /* inbound_settings_count is decremented here, when ACK is queued */
            inbound_settings_count--
            fire on_settings(session, user_data)
          transition RECV_FRAME_HEADER

      RECV_PING_PAYLOAD:
        accumulate 8 bytes into ctrl_staging via ctrl_staging_count
        on complete:
          if ACK flag:
            fire on_ping_ack(session, ctrl_staging, user_data)
          else:
            if not opt_no_auto_ping_ack:
              queue PING ACK with same 8 bytes
            else:
              fire on_ping(session, ctrl_staging, user_data)
              /* caller must call hive_submit_ping_ack() or ignore */
          ctrl_staging_count = 0
          transition RECV_FRAME_HEADER

      RECV_RST_STREAM_PAYLOAD:
        accumulate 4 bytes into ctrl_staging
        on complete:
          error_code = uint32 from ctrl_staging (big-endian)
          /* Update flood counter before stream lookup so that RSTs on
           * recently-closed streams still count toward the rate limit.
           * An attacker must not be able to evade §8.5 flood detection
           * by targeting closed stream IDs. */
          update RST_STREAM flood counter (§8.5)
          /* RST_STREAM on idle stream is a connection error per RFC 9113 §5.1. */
          stream = lookup(cur_frame.stream_id)
          if stream == NULL:
            locally_initiated = (role == HIVE_ROLE_SERVER) ?
                (cur_frame.stream_id % 2 == 0) :
                (cur_frame.stream_id % 2 == 1)
            if locally_initiated:
              is_idle = (cur_frame.stream_id > last_stream_id_local)
            else:
              is_idle = (cur_frame.stream_id > last_stream_id_remote)
            if is_idle:
              return session_error(s, HIVE_ERR_PROTOCOL,
                  HIVE_H2_PROTOCOL_ERROR, 0)
            /* else: recently closed — silently ignore per grace period */
          else:
            fire on_stream_close(session, stream_id, error_code, user_data)
            stream_close(session, stream)
          ctrl_staging_count = 0
          transition RECV_FRAME_HEADER

      RECV_WINDOW_UPDATE_PAYLOAD:
        accumulate 4 bytes into ctrl_staging
        on complete:
          increment = uint32 from ctrl_staging & 0x7FFFFFFF (big-endian)
          if increment == 0:
            if cur_frame.stream_id == 0:
              return session_error(s, HIVE_ERR_PROTOCOL,
                  HIVE_H2_PROTOCOL_ERROR, 0)  /* connection error */
            else:
              return stream_error(s, cur_frame.stream_id, HIVE_ERR_PROTOCOL,
                  HIVE_H2_PROTOCOL_ERROR)  /* stream error — return, do not fall through */
          if cur_frame.stream_id == 0:
            /* Use int64_t to avoid false overflow from negative int32_t send_window */
            if (int64_t)session->send_window + increment > 0x7FFFFFFF:
              return session_error(...FLOW_CONTROL_ERROR connection error...)
            session->send_window += increment
          else:
            stream = lookup(cur_frame.stream_id)
            if stream == NULL:
              /* Distinguish idle (never opened) from closed (recently freed).
               * Use parity-aware classification matching DATA and RST_STREAM. */
              locally_initiated = (role == HIVE_ROLE_SERVER) ?
                  (cur_frame.stream_id % 2 == 0) :
                  (cur_frame.stream_id % 2 == 1)
              if locally_initiated:
                is_idle = (cur_frame.stream_id > last_stream_id_local)
              else:
                is_idle = (cur_frame.stream_id > last_stream_id_remote)
              if is_idle:
                return session_error(s, HIVE_ERR_PROTOCOL,
                    HIVE_H2_PROTOCOL_ERROR, 0)
              /* else: recently closed — silently ignore per grace period */
            else:
              if (int64_t)stream->send_window + increment > 0x7FFFFFFF:
                return stream_error(...RST_STREAM FLOW_CONTROL_ERROR...)
              stream->send_window += increment
          ctrl_staging_count = 0
          transition RECV_FRAME_HEADER

      RECV_GOAWAY_PAYLOAD:
        accumulate first 8 bytes (last_stream_id + error_code) via ctrl_staging
        on ctrl_staging_count == 8:
          last_stream_id = uint32 from ctrl_staging[0..3] & 0x7FFFFFFF
          error_code     = uint32 from ctrl_staging[4..7]
          session->goaway_recv = 1
          session->goaway_last_stream_id_recv = last_stream_id
          session->goaway_error_code_recv     = error_code
          /* Close streams that were not processed by the sender of the GOAWAY.
           * RFC 9113 §6.8: the GOAWAY sender's last_stream_id applies to streams
           * initiated by the RECEIVER of this GOAWAY (i.e., our locally-initiated
           * streams). Only locally-initiated streams with ID > last_stream_id
           * were not processed; peer-initiated streams (push streams the sender
           * promised) are not subject to this — the sender controls its own
           * push streams and is responsible for their handling separately.
           * Implementation: close locally-initiated streams with id > last_stream_id
           * (REFUSED_STREAM). Peer-initiated streams with id > last_stream_id
           * were never promised to us above that limit and do not exist open on
           * our side, so no action is needed for those. */
          for each open stream s where s.stream_id > last_stream_id &&
              s is locally-initiated:
            fire on_stream_close(session, s.stream_id, HIVE_H2_REFUSED_STREAM, user_data)
            stream_close(session, s)
          ctrl_staging_count = 0
          debug_len = cur_frame.length - 8
          if debug_len > 0:
            /* Collect debug data into reassembly_buf before firing callback.
             * reassembly_buf is available: GOAWAY cannot arrive during CONTINUATION. */
            payload_remaining = debug_len
            transition RECV_GOAWAY_DEBUG
          else:
            fire on_goaway(session, last_stream_id, error_code, NULL, 0, user_data)
            transition RECV_FRAME_HEADER

      RECV_GOAWAY_DEBUG:
        /* Collect GOAWAY debug bytes into reassembly_buf */
        n = min(payload_remaining, len - consumed,
                opt_max_continuation_size - reassembly_len)
        copy data[consumed..+n] → reassembly_buf[reassembly_len..]
        reassembly_len += n; consumed += n; payload_remaining -= n
        if payload_remaining == 0 || reassembly_len == opt_max_continuation_size:
          fire on_goaway(session, goaway_last_stream_id_recv, goaway_error_code_recv,
                         reassembly_buf, reassembly_len, user_data)
          reassembly_len = 0
          if payload_remaining > 0:
            transition RECV_SKIP_PAYLOAD  /* skip any debug bytes that did not fit */
          else:
            transition RECV_FRAME_HEADER
          /* Note: after GOAWAY recv, hive_session_want_read() returns 0 (advisory). */

      RECV_PRIORITY_PAYLOAD:
        accumulate 5 bytes into ctrl_staging; discard; transition RECV_FRAME_HEADER
        [PRIORITY is deprecated by RFC 9113 — accept and ignore]

      RECV_SKIP_PAYLOAD:
        n = min(payload_remaining, len - consumed)
        consumed += n; payload_remaining -= n
        if payload_remaining == 0: transition RECV_FRAME_HEADER

  /* After the main loop: if recv_state != RECV_FRAME_HEADER and
   * payload_remaining == 0, execute one additional pass through the payload
   * state to complete zero-length frames (e.g. SETTINGS ACK) that arrived
   * at the end of the caller's buffer. */
  if recv_state != RECV_FRAME_HEADER && payload_remaining == 0:
    [execute one pass of the current payload state]

  return consumed
```

### 3.4 New Stream Validation

When `RECV_FRAME_HEADER` determines a HEADERS frame opens a new stream:

1. `stream_id` must be odd (client-initiated, server role) or even
   (server-initiated, client role). Wrong parity: PROTOCOL_ERROR.
2. `stream_id` must be strictly greater than `last_stream_id_remote`.
   Non-monotonic or reuse of a closed stream: PROTOCOL_ERROR.
3. `peer_stream_open_count` must be less than `opt_max_concurrent_streams`.
   Exceeded: RST_STREAM REFUSED_STREAM. Additionally, `stream_open_count` must
   be less than `opt_max_concurrent_streams` (the total slot pool cap — see
   §2.9). If the total is at capacity, RST_STREAM REFUSED_STREAM regardless of
   direction.
4. If `stream_id > (0x7FFFFFFFu - 1000u)`: initiate auto-GOAWAY (§8.6).
5. Allocate slot from `stream_free_stack`, insert into `stream_hash`,
   initialise `hive_stream_t`. Update `last_stream_id_remote`.
6. Increment both `stream_open_count` and `peer_stream_open_count`.

On stream close, decrement `stream_open_count` unconditionally. Decrement
`peer_stream_open_count` only if the stream was peer-initiated.

### 3.5 CONTINUATION Frame Lockout

Between a HEADERS or PUSH_PROMISE frame without END_HEADERS and the final
CONTINUATION frame that sets END_HEADERS, the connection is in CONTINUATION
lockout. The lockout state is tracked by `reassembly_active` (§2.7), not by
`reassembly_len`. Using `reassembly_len` as the sentinel is incorrect because
a HEADERS frame with a zero-length compressed payload is valid and results in
`reassembly_len == 0` while still requiring a CONTINUATION.

In `RECV_FRAME_HEADER`, the lockout checks are:

```c
/* If a header block is in progress, only CONTINUATION for the same stream
 * is permitted. Any other frame is a connection error. */
if (reassembly_active &&
    (cur_frame.type != HIVE_FRAME_CONTINUATION ||
     cur_frame.stream_id != reassembly_stream_id))
    return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0);

/* If no header block is in progress, a CONTINUATION frame is illegal. */
if (!reassembly_active && cur_frame.type == HIVE_FRAME_CONTINUATION)
    return session_error(s, HIVE_ERR_PROTOCOL, HIVE_H2_PROTOCOL_ERROR, 0);
```

Both checks are connection errors per RFC 9113 §4.3, not stream errors.
These checks occur before any other frame header processing.

---

## 4. HPACK

### 4.1 Dynamic Table Structure

Both encoder and decoder maintain independent dynamic tables. Each is an
instance of `hpack_table_t` embedded directly in `hive_session_t`.

```c
typedef struct {
    hpack_entry_t **ring;        /* pointer ring, ring_cap entries */
    uint32_t        ring_cap;    /* next_power_of_two(max_size / 32); min 4 */
    uint32_t        ring_head;   /* index of newest entry (insertion point) */
    uint32_t        count;       /* number of live entries */
    uint32_t        size;        /* current RFC size (sum of name+value+32) */
    uint32_t        max_size;    /* current ceiling */
    uint32_t        pending_max; /* final new ceiling awaiting application */
    uint32_t        pending_min; /* lowest value reached since last encode;
                                  * equals pending_max when no decrease occurred;
                                  * encoder emits pending_min first if != pending_max */
    uint8_t         has_pending; /* 1 = encoder must emit size update prefix */

    /* hash index — allocated only when max_size > HPACK_LINEAR_THRESHOLD */
    hpack_hash_slot_t *hash;     /* NULL when using linear scan */
    uint32_t           hash_mask;
} hpack_table_t;
```

**Table ownership by direction**:
- `enc_table.max_size` is bounded by `remote_settings.header_table_size`.
  When the peer reduces their header table limit, `enc_table.pending_max` is
  set and `has_pending = 1`. The encoder emits a size update before the next
  header block, then applies the new limit. `enc_table.pending_min` tracks the
  lowest value the ceiling reached since the last encode call. If the peer sends
  multiple SETTINGS_HEADER_TABLE_SIZE values before the encoder runs (e.g.
  4096 → 1024 → 2048), `pending_min` captures 1024 and `pending_max` captures
  2048. The encoder emits both updates: first `pending_min` (required by RFC
  7541 §6.3 when an intermediate decrease occurred), then `pending_max`. When
  only one SETTINGS has arrived, `pending_min == pending_max` and only one
  update is emitted. When `SETTINGS_HEADER_TABLE_SIZE` arrives, update:
  `pending_min = min(pending_min, new_val)` and `pending_max = new_val`.
- `dec_table.max_size` is bounded by `local_settings.header_table_size`
  (what we advertised). The peer's encoder must not exceed this.

**Decoder-side pending_max**: For `dec_table`, `pending_max` is initialized to
`opt_header_table_size` at session creation. It is updated to the new value
whenever a local SETTINGS frame with a new HEADER_TABLE_SIZE is applied
(i.e., when the SETTINGS ACK is received from the peer, confirming the peer
has applied our new limit). It represents the maximum table size the decoder
will accept from inbound size updates. Inbound size updates larger than
`dec_table.pending_max` are a COMPRESSION_ERROR (connection error).

`HPACK_LINEAR_THRESHOLD` is 16384 — a compile-time constant. Below this
threshold, lookup is a linear scan from newest entry backward (temporal
locality). Above it, an auxiliary hash index is allocated from the session
arena. For the RFC default table size of 4096 bytes, `hash` is always NULL.

At default `opt_header_table_size = 4096`:
- Maximum entries: 4096 / 32 = 128
- `ring_cap` = 128 (power of two)
- Ring pointer array: 128 × 8 bytes = 1 KB per table
- `hash` = NULL — the null check is on the hot path, never taken at defaults

### 4.2 Entry Layout

Each dynamic table entry is a single contiguous allocation:

```
[ hpack_entry_t header (8 bytes) | name bytes | value bytes ]
                                  ▲             ▲
                           HPACK_ENTRY_NAME     HPACK_ENTRY_VALUE
```

```c
typedef struct {
    uint32_t name_len;
    uint32_t value_len;
    /*
     * name bytes immediately follow at (uint8_t *)(entry + 1)
     * value bytes immediately follow name
     */
} hpack_entry_t;

#define HPACK_ENTRY_NAME(e)     ((uint8_t *)((e) + 1))
#define HPACK_ENTRY_VALUE(e)    ((uint8_t *)((e) + 1) + (e)->name_len)
#define HPACK_ENTRY_RFC_SIZE(e) ((e)->name_len + (e)->value_len + 32)
```

**Insertion**: Before allocating, check for integer overflow:
```c
if (name_len > SIZE_MAX - value_len ||
    name_len + value_len > SIZE_MAX - sizeof(hpack_entry_t))
    return HIVE_ERR_COMPRESSION;
```
Then allocate `sizeof(hpack_entry_t) + name_len + value_len` bytes via the
session allocator. Copy name and value bytes in. Store pointer in
`ring[ring_head]`. Advance `ring_head = (ring_head + 1) & (ring_cap - 1)`.
Evict oldest entries until `size + new_rfc_size <= max_size`.

**Oversized entry** (RFC 7541 §4.4): if the new entry's `rfc_size > max_size`,
the eviction loop will empty the table entirely (since size never drops below
zero). After eviction, `size == 0` but `rfc_size > max_size` still, so the
entry is **not inserted**. The table remains empty. This is correct per the
RFC and must be implemented explicitly — do not skip the check after eviction.

**Eviction**: oldest entry is at `ring[(ring_head - count) & (ring_cap - 1)]`.
Free the allocation via session allocator. With an arena allocator the free
is a no-op — memory is reclaimed when the session closes. With system malloc,
individual entries are freed normally. Decrement `count`, subtract from `size`.

When the HPACK hash index is active (max_size > HPACK_LINEAR_THRESHOLD), the
evicted entry's hash slot must be marked as tombstone (see §4.9) rather than
cleared to EMPTY. Clearing to EMPTY would break probe chains.

**Dynamic table size updates**: when the peer sends `SETTINGS_HEADER_TABLE_SIZE`,
`enc_table.pending_max` is set and `enc_table.has_pending = 1`. On the next
`hpack_encode_block()` call, the encoder emits the size update representation
**as the first byte(s) of the header block**, before any header fields. If the
limit changes more than once before the encoder runs, the encoder MUST emit the
smallest value reached first, then the final value (RFC 7541 §6.3). This
two-emission requirement is not optional: emitting only the final value when
an intermediate decrease occurred is a protocol violation.

### 4.3 Static Table

The static table (RFC 7541 Appendix A) is a compile-time array of 61 entries.
No allocation. Index 1 = `:authority`, index 2 = `:method GET`, etc.

```c
static const hive_nv_t hpack_static_table[61] = {
    /* index 1 */  { (const uint8_t *)":authority",      NULL, 10, 0, 0 },
    /* index 2 */  { (const uint8_t *)":method", (const uint8_t *)"GET", 7, 3, 0 },
    /* ... */
};
```

### 4.4 Huffman Decode

HPACK uses a canonical Huffman code (RFC 7541 Appendix B) where code lengths
range from 5 to 30 bits. The decode strategy is two-tier:

1. **Fast path** — a 256-entry table of 4 bytes per entry (1 KB total) covers
   every 8-bit input pattern. Symbols whose Huffman code is ≤ 8 bits decode in
   one lookup. Indices `0xfe` and `0xff` (i.e. patterns starting with the
   prefix `1111 1110` or `1111 1111`) carry `complete = 0`; every code longer
   than 8 bits begins with one of these two prefixes, so a fast-path miss
   uniquely identifies a code requiring the slow path.
2. **Slow path** — `huff_decode_long()` linear-scans the 257-entry encode
   table looking for the unique code of length L (10 ≤ L ≤ 30) whose top L
   bits match the accumulator. Because canonical Huffman codes are prefix-free
   the first match at any L is *the* match; longer codes cannot share an
   L-bit prefix with a shorter code. Returns the number of bits consumed
   (or 0 to signal "need more input", or –1 on EOS).

A flat 256-entry table on its own is insufficient: indexing always reads the
top 8 bits of the accumulator, so once the prefix is `0xfe` or `0xff` the
fast-path lookup keeps returning `complete = 0` regardless of how many more
bytes are accumulated. The slow path is the mechanism that resolves the
remaining 22 bit-lengths (10 through 30) without a multi-level table.

```c
typedef struct {
    uint8_t  sym;           /* decoded symbol */
    uint8_t  bits_consumed; /* input bits consumed by this lookup (1–8) */
    uint8_t  complete;      /* 1 = a complete symbol was produced */
    uint8_t  eos;           /* 1 = EOS symbol encountered (error mid-string) */
} huff_entry_t;

static const huff_entry_t huff_table[256];  /* initialised at compile time */
```

**Decode loop** (writes decoded bytes into `hpack_scratch_name` or
`hpack_scratch_value` as appropriate — see §4.6):

```c
/* Huffman decode of src[0..src_len] into scratch[0..max_len].
 * Outer loop is a byte-fed accumulator; inner loop drains one symbol per
 * iteration via the fast-path table or, on a fast-path miss, via the
 * slow path huff_decode_long(). */
static int
huff_decode(const uint8_t *src, size_t src_len,
    uint8_t *scratch, size_t max_len, size_t *out_len)
{
    uint64_t acc   = 0;   /* bit accumulator (≥ 30 bits required) */
    int      nbits = 0;   /* bits currently in accumulator */
    size_t   out   = 0;

    for (size_t i = 0; i < src_len; i++) {
        acc    = (acc << 8) | src[i];
        nbits += 8;

        while (nbits >= 8) {
            /* Fast path: top 8 bits index the 256-entry table */
            uint8_t idx = (uint8_t)(acc >> (nbits - 8));
            const huff_entry_t *e = &huff_table[idx];

            if (e->complete) {
                if (e->eos)
                    return HIVE_ERR_COMPRESSION;  /* EOS mid-string */
                if (out >= max_len)
                    return HIVE_ERR_COMPRESSION;  /* decoded string too long */
                scratch[out++] = e->sym;
                nbits -= e->bits_consumed;
            } else {
                /* Slow path: prefix is 0xfe/0xff — code is > 8 bits */
                uint8_t sym;
                int bits = huff_decode_long(acc, nbits, &sym);
                if (bits < 0)
                    return HIVE_ERR_COMPRESSION;  /* EOS in slow path */
                if (bits == 0)
                    break;                        /* need more input */
                if (out >= max_len)
                    return HIVE_ERR_COMPRESSION;
                scratch[out++] = sym;
                nbits -= bits;
            }
            acc &= ((uint64_t)1 << nbits) - 1;    /* clear consumed bits */
        }
    }

    /* Validate padding: remaining bits must be high-order EOS bits (all 1s).
     * After the decode loop, acc holds remaining bits in the LOW positions
     * (bits 0..nbits-1). Valid padding is all-ones in those positions. */
    if (nbits > 7)
        return HIVE_ERR_COMPRESSION;  /* too many leftover bits */
    if (nbits > 0) {
        uint64_t expected = ((uint64_t)1 << nbits) - 1;
        if (acc != expected)
            return HIVE_ERR_COMPRESSION;  /* invalid padding */
    }

    *out_len = out;
    return HIVE_OK;
}

/* Slow-path linear scan over the 257-entry encode table.
 * Invoked only when the fast-path entry has complete == 0 — i.e. the
 * accumulator's top 8 bits are 0xfe or 0xff and the next symbol is one
 * of the 116 codes longer than 8 bits (including EOS).
 * Returns L > 0 on match (bits consumed), 0 if more input is needed
 * to disambiguate at any L, or -1 if EOS is decoded mid-string. */
static int
huff_decode_long(uint64_t acc, int nbits, uint8_t *out_sym);
```

The `huff_table` entry for a given 8-bit pattern reflects the longest complete
code that starts with those 8 bits. Every entry except `0xfe` and `0xff` has
`complete == 1`. Those two patterns have `complete = 0` and route to the slow
path. For codes shorter than 8 bits, `bits_consumed < 8` and the remaining
bits remain in the accumulator for the next iteration. The accumulator is
`uint64_t` because the longest Huffman code (EOS, 30 bits) plus up to 7
trailing-padding bits plus a partial accumulated byte easily exceeds 32 bits
in transient state. Fast path is L1 cache resident after the first HEADERS
frame. The slow path is exercised only when input contains symbols whose
Huffman code is longer than 8 bits — uncommon in typical HTTP/2 header
traffic — so its O(257 × 21) worst-case scan is not on the hot path.

### 4.5 Decode Block Processing

`hpack_decode_block(session, data, len, suppress_callbacks, error_stream_id)` processes
a complete compressed header block (from `reassembly_buf`). It enforces bomb
protection and fires the header callbacks. When `suppress_callbacks == 1`, all
callbacks are suppressed and `stream_error_pending` starts as 1 — but the full
block is still decoded and all dynamic table updates applied. The caller uses
`suppress_callbacks = 1` when the HEADERS block arrived on an illegal-state stream.
`error_stream_id` specifies which stream to send RST_STREAM on and close when a
stream error occurs. For HEADERS blocks this is `reassembly_stream_id` (the stream
carrying the HEADERS). For PUSH_PROMISE blocks this is `reassembly_promised_stream_id`
(the promised stream) — the carrying stream is unaffected by HPACK decode errors.

Size updates (bit pattern `0x20`) are only valid at the beginning of a header
block, before any header field representations. After the first header field
is decoded, any size update is a COMPRESSION_ERROR (fatal, connection error).

**Dynamic table consistency on stream errors**: Because the HPACK dynamic
table is connection-scoped, a stream-level error (bomb limit, messaging
violation) must NOT abort decoding mid-block. The decoder must continue
consuming and applying dynamic table updates for all remaining entries in
the header block while suppressing `on_header` delivery. Aborting mid-block
desynchronizes the encoder's and decoder's tables, corrupting all subsequent
HEADERS processing on the connection. Only HPACK-level errors (malformed
encoding, invalid index) remain connection errors that abort immediately.

```
hpack_decode_block(session, data, len, suppress_callbacks, error_stream_id):

  decoded_size      = 0    /* bomb limit 1 running total */
  decoded_count     = 0    /* bomb limit 2 running total */
  size_update_phase = 1    /* 1 = size updates still permitted */
  pseudo_done       = 0    /* 1 = pseudo-headers phase ended */
  has_method        = 0; has_scheme = 0; has_path = 0
  has_status        = 0; has_authority = 0
  /* stream_error_pending: 1 suppresses callback delivery while still
   * decoding for table synchronization. Pre-set to 1 when suppress_callbacks
   * is set (illegal-state stream). */
  stream_error_pending = suppress_callbacks ? 1 : 0
  pos               = 0

  if !suppress_callbacks:
    cb_ret = fire on_begin_headers(session, reassembly_stream_id, user_data)
    /* on_begin_headers return value handling:
     * - HIVE_OK:               continue normally
     * - HIVE_ERR_COMPRESSION:  connection error — COMPRESSION_ERROR GOAWAY,
     *                           return immediately (table state is still valid
     *                           since no entries were decoded yet)
     * - any other HIVE_ERR_*:  set stream_error_pending = 1, continue decoding
     *                           to maintain dynamic table consistency */
    if cb_ret == HIVE_ERR_COMPRESSION:
      return session_error(s, HIVE_ERR_COMPRESSION, HIVE_H2_COMPRESSION_ERROR, 0)
    else if cb_ret != HIVE_OK:
      stream_error_pending = 1

  while pos < len:

    first_byte = data[pos]

    if first_byte & 0x80:
      /* Indexed Header Field (RFC 7541 §6.1) */
      size_update_phase = 0
      decode varint index with 7-bit prefix (§4.7)
      if index == 0: return HIVE_ERR_COMPRESSION
      if index <= 61:
        lookup hpack_static_table[index - 1]
      else:
        dyn_idx = index - 62
        if dyn_idx >= dec_table.count: return HIVE_ERR_COMPRESSION
        lookup dec_table entry at dyn_idx (newest = 0, oldest = count-1)
      name_buf  = {&name, name_len}
      value_buf = {&value, value_len}

    else if first_byte & 0x40:
      /* Literal with Incremental Indexing (RFC 7541 §6.2.1) */
      size_update_phase = 0
      decode name and value (§4.6)
      insert copy into dec_table dynamic table (always copy — §8.1)
      name_buf / value_buf → decoded strings

    else if first_byte & 0x20:
      /* Dynamic Table Size Update (RFC 7541 §6.3) */
      if !size_update_phase:
        return HIVE_ERR_COMPRESSION
      new_size = decode varint with 5-bit prefix
      if new_size > dec_table.pending_max:
        return HIVE_ERR_COMPRESSION
      hpack_table_evict_to(&dec_table, new_size)
      dec_table.max_size = new_size
      continue  /* no header emitted; loop again */

    else:
      /* Literal without indexing / never indexed (RFC 7541 §6.2.2, 6.2.3) */
      size_update_phase = 0
      decode name and value (§4.6)
      no table insertion
      if never_indexed: flags |= HIVE_NV_FLAG_NO_INDEX

    /* Bomb protection — enforced incrementally (Security decision 2) */
    /* Size per RFC 7541 §4.1: name_len + value_len + 32 per entry */
    decoded_size += name_buf.len + value_buf.len + 32
    decoded_count += 1
    if decoded_size  > opt_max_header_list_size:
      stream_error_pending = 1  /* suppress delivery; continue decoding */
    if decoded_count > opt_max_header_count:
      stream_error_pending = 1

    /* HTTP messaging validation [if opt_no_http_messaging == 0] */
    if opt_no_http_messaging == 0 && !stream_error_pending:
      validate pseudo-header ordering (RFC 9113 §8.3):
        pseudo-headers must come before regular headers
        if !pseudo_done && name[0] != ':': pseudo_done = 1
        if pseudo_done && name[0] == ':':
          stream_error_pending = 1  /* out-of-order pseudo-header */
        reject duplicate pseudo-headers for same block
        reject unknown pseudo-headers
        track which pseudo-headers have been seen:
          if name == ":method":    has_method    = 1
          if name == ":scheme":    has_scheme    = 1
          if name == ":path":      has_path      = 1
          if name == ":status":    has_status    = 1
          if name == ":authority": has_authority = 1
      validate field names are lowercase (RFC 9113 §8.2):
        if uppercase found: stream_error_pending = 1
      validate forbidden headers (Connection, Keep-Alive, Proxy-Connection,
        Upgrade, Transfer-Encoding):
        if found: stream_error_pending = 1
      validate TE header: only "trailers" value permitted

    if !stream_error_pending:
      /* Invalidate hive_buf_t handles after callback — before next iteration */
      cb_ret = fire on_header(session, stream_id, &name_buf, &value_buf,
                              flags, user_data)
      name_buf.flags  &= ~HIVE_BUF_VALID  /* clear in library's own objects */
      value_buf.flags &= ~HIVE_BUF_VALID
      /* on_header return value handling:
       * - HIVE_OK:               continue normally
       * - HIVE_ERR_COMPRESSION:  connection error — COMPRESSION_ERROR GOAWAY;
       *                           dynamic table is still consistent at this
       *                           point (entry already inserted above), so
       *                           GOAWAY is safe to send immediately
       * - any other HIVE_ERR_*:  set stream_error_pending = 1, continue
       *                           decoding to maintain dynamic table consistency
       *                           per RFC 7541 §2.3.2; RST_STREAM sent after
       *                           full block is consumed */
      if cb_ret == HIVE_ERR_COMPRESSION:
        return session_error(s, HIVE_ERR_COMPRESSION, HIVE_H2_COMPRESSION_ERROR, 0)
      else if cb_ret != HIVE_OK:
        stream_error_pending = 1

  /* After all headers decoded: presence validation (RFC 9113 §8.3) */
  if opt_no_http_messaging == 0 && !stream_error_pending:
    /* Determine message type from stream context */
    if is_request_block:
      if !has_method: stream_error_pending = 1
      if method != "CONNECT":
        if !has_scheme || !has_path: stream_error_pending = 1
      else:  /* CONNECT */
        if has_scheme || has_path: stream_error_pending = 1
        if !has_authority: stream_error_pending = 1
      if has_status: stream_error_pending = 1  /* :status in request */
    if is_response_block:
      if !has_status: stream_error_pending = 1
      if has_method || has_scheme || has_path: stream_error_pending = 1
    if is_trailer_block:
      if pseudo_done == 0 && (has_method || has_scheme ||
          has_path || has_status): stream_error_pending = 1

  if stream_error_pending:
    /* When suppress_callbacks == 1, the caller owns RST_STREAM and stream_close.
     * Return HIVE_ERR_PROTOCOL so the caller knows decoding finished with
     * a suppressed error. */
    if suppress_callbacks:
      return HIVE_ERR_PROTOCOL
    /* Normal stream error: RST targets error_stream_id.
     * For HEADERS: error_stream_id == reassembly_stream_id (the HEADERS stream).
     * For PUSH_PROMISE: error_stream_id == reassembly_promised_stream_id
     * (the promised stream — the carrying stream is unaffected). */
    stream = lookup_stream(session, error_stream_id)
    if stream != NULL:
      fire on_stream_close(session, error_stream_id,
          HIVE_H2_PROTOCOL_ERROR, user_data)
      stream_close(session, stream)
    send RST_STREAM PROTOCOL_ERROR for error_stream_id
    /* Do NOT fire on_headers_complete — the stream is being reset. */
    return HIVE_ERR_PROTOCOL

  cb_ret = fire on_headers_complete(session, stream_id, end_stream, user_data)
  /* on_headers_complete return value handling:
   * - HIVE_OK:               done
   * - HIVE_ERR_COMPRESSION:  connection error — COMPRESSION_ERROR GOAWAY
   * - any other HIVE_ERR_*:  stream error — RST_STREAM PROTOCOL_ERROR */
  if cb_ret == HIVE_ERR_COMPRESSION:
    return session_error(s, HIVE_ERR_COMPRESSION, HIVE_H2_COMPRESSION_ERROR, 0)
  else if cb_ret != HIVE_OK:
    stream = lookup_stream(session, error_stream_id)
    if stream != NULL:
      fire on_stream_close(session, error_stream_id,
          HIVE_H2_PROTOCOL_ERROR, user_data)
      stream_close(session, stream)
    send RST_STREAM PROTOCOL_ERROR for error_stream_id
    return HIVE_ERR_PROTOCOL

  return HIVE_OK
```

### 4.6 String Decoding

**Huffman-encoded strings**: call `huff_decode()` into the appropriate scratch
buffer. For a header name, decode into `hpack_scratch_name`. For a header
value, decode into `hpack_scratch_value`. The two buffers are separate so that
both the decoded name and decoded value are available simultaneously when the
`on_header` callback fires. Return `HIVE_ERR_COMPRESSION` if the decoded
length would exceed `opt_max_header_string_size` or if the encoded string
length field claims more bytes than remain in the header block (truncated
string). These are HPACK decoding errors and must surface as connection errors
of type COMPRESSION_ERROR per RFC 9113 §4.3 — not stream errors. On success,
`hive_buf_t.data` points to the appropriate scratch buffer.

**Non-Huffman strings**: return a direct pointer into the source buffer
(`reassembly_buf` or the static table). No copy. Return `HIVE_ERR_COMPRESSION`
if the claimed string length exceeds the remaining bytes in the block
(truncation). On success, `hive_buf_t.data` points into the source buffer.

In both cases: if the string is to be inserted into the dynamic table, the
table insertion always copies the bytes into arena memory regardless of how
they were decoded (§8.1).

### 4.7 Integer Decoding (RFC 7541 §5.1)

All HPACK integer encodings use the same varint scheme. The first byte
provides N prefix bits; if the prefix value is `2^N - 1`, subsequent bytes
encode the remainder in groups of 7 bits with a continuation bit.

```c
#define HPACK_INT_OVERFLOW  UINT32_MAX   /* sentinel: varint overflowed */

static uint32_t
hpack_decode_int(const uint8_t *src, size_t len, int prefix_bits,
    size_t *consumed)
{
    uint32_t prefix_max = (1u << prefix_bits) - 1;
    uint32_t val;

    if (len == 0)
        return HPACK_INT_OVERFLOW;   /* truncated */

    val       = src[0] & prefix_max;
    *consumed = 1;

    if (val < prefix_max)
        return val;

    /* multi-byte continuation */
    uint32_t m = 0;
    while (*consumed < len) {
        uint8_t b = src[(*consumed)++];
        /* Use 64-bit intermediate to detect overflow at m=28 */
        uint64_t tmp = (uint64_t)val + ((uint64_t)(b & 0x7F) << m);
        if (tmp > UINT32_MAX)
            return HPACK_INT_OVERFLOW;
        val = (uint32_t)tmp;
        m  += 7;
        if (!(b & 0x80))
            return val;             /* complete */
        if (m > 28)
            return HPACK_INT_OVERFLOW; /* overflow guard */
    }
    return HPACK_INT_OVERFLOW;      /* truncated — ran out of input bytes */
}
```

Callers must check the return value against `HPACK_INT_OVERFLOW` and return
`HIVE_ERR_COMPRESSION` on overflow or truncation.

### 4.8 Encoder

The encoder writes directly into `send_buf + encode_start` (see §6.4 for
the exact offset calculation). No separate encoder scratch buffer.

```c
static int
hpack_encode_block(hpack_table_t *table,
    const hive_nv_t *nva, size_t nvlen,
    uint8_t *out, size_t out_cap, size_t *out_len);
```

If `enc_table.has_pending == 1`, the encoder emits dynamic table size update
representation(s) as the first bytes of the output block before any header
fields. If `enc_table.pending_min != enc_table.pending_max`, emit
`pending_min` first then `pending_max` (RFC 7541 §6.3 — the lowest value
reached must be emitted when an intermediate decrease occurred). If
`pending_min == pending_max`, emit a single size update. After emitting:

1. Evict to `pending_min` first: `hpack_table_evict_to(&enc_table, pending_min)`.
   This synchronizes the encoder's table with the decoder's state at the
   intermediate minimum — the decoder already evicted to this limit when it
   processed the corresponding size update from our encoder output.
2. Set `enc_table.max_size = pending_min`.
3. If `pending_max != pending_min`: evict to `pending_max`:
   `hpack_table_evict_to(&enc_table, pending_max)`, then set
   `enc_table.max_size = pending_max`.
4. Reset `has_pending = 0`, `pending_min = pending_max`.

For each header, lookup order: (1) static table exact match → indexed;
(2) static table name-only match → literal with index; (3) dynamic table
lookup (linear or hash per §4.9); (4) literal without indexing.

For dynamic table lookup, the search must prefer an exact match (name + value)
over a name-only match, and must prefer the newest matching entry. When using
the hash index (§4.9), the lookup must correctly handle multiple entries with
the same name but different values.

Huffman encoding is applied to string literals when the encoded length
is strictly shorter than the raw length. The Huffman encode table
(RFC 7541 Appendix B) is a compile-time lookup of code + bit-length.

### 4.9 Hash Index (Large Tables)

When `max_size > HPACK_LINEAR_THRESHOLD (16384)`:

```c
typedef struct {
    uint32_t name_hash;   /* FNV-1a 32-bit hash of name bytes */
    uint32_t value_hash;  /* FNV-1a 32-bit hash of value bytes */
    uint32_t ring_idx;    /* index into ring[] — HPACK_HASH_EMPTY or HPACK_HASH_TOMBSTONE */
} hpack_hash_slot_t;
```

8+4 = 12 bytes per slot. Hash table size = `next_power_of_two(ring_cap * 2)`.
Allocated from session arena when the table is first configured above the
threshold. Hash function: FNV-1a 32-bit on name bytes for the probe position.
Collision resolution: linear probing.

Sentinels:
```c
#define HPACK_HASH_EMPTY      0xFFFFFFFF  /* slot has never been used */
#define HPACK_HASH_TOMBSTONE  0xFFFFFFFE  /* slot held an evicted entry */
```

**Insertion**: hash entry added alongside ring entry. Probe from
`name_hash & hash_mask` until EMPTY or TOMBSTONE slot found; write entry.
Tombstone slots can be reclaimed on insertion.

**Eviction**: set `ring_idx = HPACK_HASH_TOMBSTONE`. Do NOT set to
HPACK_HASH_EMPTY. Clearing to EMPTY breaks linear probe chains: any entry
that collided with this slot and was placed further along the probe sequence
becomes unreachable. Tombstones allow probing to continue past evicted slots.

**Lookup** (exact match preferred, then name-only):
```
probe from name_hash & hash_mask:
  if slot.ring_idx == HPACK_HASH_EMPTY: stop (not found)
  if slot.ring_idx == HPACK_HASH_TOMBSTONE: continue probing
  if slot.name_hash == query_name_hash:
    compare full name bytes for collision safety
    if name matches:
      if slot.value_hash == query_value_hash:
        compare full value bytes — exact match candidate
      else:
        name-only match candidate (lower priority)
  continue probing until EMPTY or all slots scanned
return best candidate (exact match over name-only; newest by ring position)
```

The `value_hash` field is required to efficiently distinguish exact matches
from name-only matches during the probe. Without it, every name match would
require a full value comparison, and finding the best match among multiple
same-name entries would require enumerating the entire probe cluster.

---

## 5. Stream Table

### 5.1 Two-Layer Layout

```
stream_hash:       stream_hash_entry_t[hash_table_size]      — Layer 1
stream_slots:      hive_stream_t[opt_max_concurrent_streams]  — Layer 2
stream_free_stack: uint32_t[opt_max_concurrent_streams]       — free slot indices
```

Both layers are pre-allocated from the session arena at creation.
Zero runtime allocation per stream open or close.

`hash_table_size = next_power_of_two(opt_max_concurrent_streams * 2)`.
At default `opt_max_concurrent_streams = 100`:
- `hash_table_size = 256`
- `stream_hash`: 256 × 8 = 2 KB
- `stream_slots`: 100 × 64 = 6.4 KB
- `stream_free_stack`: 100 × 4 = 400 bytes
- Total: ~8.8 KB, L1 cache resident

### 5.2 Hash Entry

```c
typedef struct {
    uint32_t stream_id;   /* 0xFFFFFFFF = empty, 0xFFFFFFFE = tombstone */
    uint32_t slot_index;  /* index into stream_slots[] */
} stream_hash_entry_t;   /* 8 bytes */
```

### 5.3 Stream State Object

```c
typedef struct {
    uint32_t             stream_id;                 /* 0 = slot not in use */
    uint8_t              state;                     /* hive_stream_state_t */
    uint8_t              flags;                     /* HIVE_STREAM_FLAG_* */
    uint8_t              weight;                    /* priority weight — retained, not used */
    uint8_t              _pad;
    int32_t              send_window;               /* stream-level send window */
    int32_t              recv_window;               /* stream-level recv window */
    uint32_t             recv_consumed;             /* unacked bytes for WINDOW_UPDATE coalescing */
    uint32_t             _pad2;                     /* alignment for int64_t fields */
    int64_t              content_length_expected;   /* from Content-Length header; -1 = absent */
    uint64_t             content_length_received;   /* DATA bytes received so far */
    void                *user_data;                 /* per-stream caller context */
    hive_data_source_t   data_source;               /* body source while stream is open */
} hive_stream_t;  /* 4+1+1+1+1+4+4+4+4+8+8+8+16 = 64 bytes */
```

`content_length_expected` is set when a `Content-Length` header is decoded and
validated during `hpack_decode_block()`. Value -1 indicates no Content-Length
header was present. When messaging validation is enabled
(`opt_no_http_messaging == 0`), each DATA frame increments
`content_length_received`, and on stream close the library verifies that
`content_length_received == (uint64_t)content_length_expected` when expected
is not -1. A mismatch is a stream error PROTOCOL_ERROR.

Stream states:

```c
typedef enum {
    HIVE_STREAM_IDLE              = 0,
    HIVE_STREAM_OPEN              = 1,
    HIVE_STREAM_HALF_CLOSED_LOCAL  = 2,  /* we sent END_STREAM */
    HIVE_STREAM_HALF_CLOSED_REMOTE = 3,  /* peer sent END_STREAM */
    HIVE_STREAM_CLOSED             = 4,
    HIVE_STREAM_RESERVED_LOCAL     = 5,  /* server push promised */
    HIVE_STREAM_RESERVED_REMOTE    = 6,  /* client received PUSH_PROMISE */
} hive_stream_state_t;
```

### 5.4 Hash Function

```c
static uint32_t
stream_hash_fn(uint32_t stream_id, uint32_t hash_mask)
{
    return (stream_id * 2654435761u) >> (32 - __builtin_popcount(hash_mask));
}
```

Knuth multiplicative hash. Single multiply + shift. Distributes
monotonically increasing odd stream IDs uniformly across the table.

### 5.5 Open, Lookup, Close

**Open** (new stream arrives):
```
pop slot_index from stream_free_stack (stream_free_top--)
initialise stream_slots[slot_index]: stream_id, state, windows, etc.
  content_length_expected = -1
  content_length_received = 0
h = stream_hash_fn(stream_id, stream_hash_mask)
linear probe from h until EMPTY slot found
write {stream_id, slot_index} into that slot
stream_open_count++
if stream is peer-initiated: peer_stream_open_count++
```

**Lookup**:
```
h = stream_hash_fn(stream_id, stream_hash_mask)
linear probe from h:
  if entry.stream_id == stream_id: return &stream_slots[entry.slot_index]
  if entry.stream_id == EMPTY (0xFFFFFFFF): return NULL (not found)
  if entry.stream_id == TOMBSTONE (0xFFFFFFFE): continue probing
```

**Close** (RST_STREAM, END_STREAM both sides, GOAWAY):
```
h = stream_hash_fn(stream_id, stream_hash_mask)
find hash entry (same probe as lookup)
set hash entry to TOMBSTONE (0xFFFFFFFE)
set stream_slots[slot_index].stream_id = 0
push slot_index onto stream_free_stack (stream_free_stack[stream_free_top++])
stream_open_count--
if stream was peer-initiated: peer_stream_open_count--
tombstone_count++
closes_since_compact++
if tombstone_count > hash_table_size / 4 AND closes_since_compact >= 64:
  compact hash table (see §5.6)
```

`stream_close()` is a table management function only. It does NOT fire the
`on_stream_close` callback. Callers (§3.3 receive path, §6.5 send path) are
responsible for firing `on_stream_close` before calling `stream_close()`. This
ensures the callback fires exactly once and that the stream is still findable
via `stream_lookup()` at the time the callback runs.

### 5.6 Hash Table Compaction

Triggered when tombstone ratio exceeds 25% and at least 64 closes have
occurred since the last compaction. O(hash_table_size):

```
memset stream_hash to EMPTY (0xFFFFFFFF)
for each live entry in stream_slots (stream_id != 0):
  h = stream_hash_fn(stream_id, stream_hash_mask)
  linear probe from h until EMPTY found
  write entry
tombstone_count    = 0
closes_since_compact = 0
```

For 256 hash entries this scans 256 × 8 = 2048 bytes. Nanosecond scale.

### 5.7 Stream State Legality Table

RFC 9113 §5.1 defines which frames are valid in each stream state. The
library enforces these rules when processing incoming frames. Violations
are stream errors (RST_STREAM) unless otherwise noted.

| Received frame | idle | open | half-closed (remote) | half-closed (local) | closed | reserved (local) | reserved (remote) |
|---|---|---|---|---|---|---|---|
| HEADERS | opens stream ✓ | ✓ (trailers) | error | ✓ | error† | error | → half-closed (local) |
| DATA | error | ✓ | error | ✓ | error† | error | error |
| RST_STREAM | error | ✓ | ✓ | ✓ | ignore† | ✓ | ✓ |
| WINDOW_UPDATE | error | ✓ | ✓ | ✓ | error† | error | ✓ |
| PRIORITY | ignore | ignore | ignore | ignore | ignore | ignore | ignore |
| PUSH_PROMISE | n/a | opens reserved | n/a | ✓† | error | n/a | n/a |
| CONTINUATION | error | follows HEADERS | error | follows HEADERS | error | follows PP | error |

† Closed stream: if the stream was recently closed and we have a grace period
(implementation-defined, typically one round-trip), silently ignore certain
frames (RST_STREAM, WINDOW_UPDATE) that may be in-flight. Otherwise stream
error STREAM_CLOSED. Connection error for HEADERS/DATA on a fully closed
stream where `stream_id <= last_stream_id_remote` (closed after having been
opened) is a stream error STREAM_CLOSED; for `stream_id > last_stream_id_remote`
(never opened / idle) it is a connection error PROTOCOL_ERROR.

†† PUSH_PROMISE on half-closed(local): valid per RFC 9113 §6.6. The stream is
half-closed from the receiver's (client's) perspective — the client has sent
END_STREAM but the server may still send PUSH_PROMISE on the associated stream
before the response completes. RECV_PUSH_PROMISE_PAYLOAD explicitly allows
both OPEN and HALF_CLOSED_LOCAL carrying streams.

`idle` streams receiving frames other than HEADERS, PRIORITY, or PUSH_PROMISE
(on a referenced stream) are a connection error PROTOCOL_ERROR per RFC 9113
§5.1. (PRIORITY is also permitted on idle streams without causing a state
transition or error.)

Notes on corrected cells:
- `reserved (remote) | HEADERS → half-closed (local)`: receiving HEADERS on
  a reserved (remote) stream is the promised push response arriving; it
  transitions to half-closed (local) per RFC 9113 §5.1.1.
- `reserved (local) | HEADERS received → error`: the table shows received
  frames. Receiving HEADERS on a reserved (local) stream is a stream error
  PROTOCOL_ERROR. Sending HEADERS on reserved (local) — which is not shown
  here — transitions to half-closed (remote).
- `half-closed (remote) | WINDOW_UPDATE → valid`: RFC 9113 §5.1 explicitly
  lists WINDOW_UPDATE as valid in this state.
- `reserved (local) | WINDOW_UPDATE → valid`: the peer in reserved (remote)
  state may send WINDOW_UPDATE per RFC 9113 §5.1.1.

---

## 6. Send Queue

### 6.1 Buffers

```c
struct iovec *send_iov;           /* iovec[opt_max_send_iov], pre-allocated */
int           send_iov_count;     /* entries currently queued */
uint8_t      *send_buf;           /* frame serialisation buffer */
size_t        send_buf_used;      /* bytes written into send_buf */
size_t        send_buf_cap;       /* sized for headers + CONTINUATION overhead */
size_t        send_partial_offset;/* bytes already sent from current batch */
uint8_t       send_partial;       /* 1 = unsent tail from previous send call */
```

`send_iov` default: 512 entries × 16 bytes = 8 KB.
`send_buf_cap` default: 65536 + 4×9 + 2048 = 67620 bytes.

Both pre-allocated at session creation. No allocation during operation.

### 6.2 Frame Header Serialisation

Every outgoing frame writes its 9-byte header into `send_buf`:

```c
static void
frame_hdr_write_at(uint8_t *dst, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id)
{
    dst[0] = (length >> 16) & 0xFF;
    dst[1] = (length >> 8)  & 0xFF;
    dst[2] =  length        & 0xFF;
    dst[3] = type;
    dst[4] = flags;
    dst[5] = (stream_id >> 24) & 0x7F;  /* R bit cleared */
    dst[6] = (stream_id >> 16) & 0xFF;
    dst[7] = (stream_id >> 8)  & 0xFF;
    dst[8] =  stream_id        & 0xFF;
}

/* Convenience: write at send_buf + send_buf_used and advance */
static uint8_t *
frame_hdr_write(hive_session_t *s, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id)
{
    uint8_t *p = s->send_buf + s->send_buf_used;
    frame_hdr_write_at(p, length, type, flags, stream_id);
    s->send_buf_used += 9;
    return p;
}
```

### 6.3 Control Frame Queuing

SETTINGS, SETTINGS ACK, PING, PING ACK, RST_STREAM, WINDOW_UPDATE, GOAWAY:

```
frame_start = send_buf_used
frame_hdr_write(session, payload_len, type, flags, stream_id)
write payload bytes into send_buf + send_buf_used
send_iov[send_iov_count++] = {send_buf + frame_start, 9 + payload_len}
send_buf_used += payload_len
```

One iovec entry per control frame. The 9-byte header and payload are
contiguous in `send_buf`.

**SETTINGS frame directionality**: when queuing a SETTINGS frame for a
server-role session, the SETTINGS_ENABLE_PUSH parameter must not be included
regardless of the `opt_enable_push` value. RFC 9113 §6.5.2 prohibits servers
from sending this parameter with value 1.

### 6.4 HEADERS Frame Queuing

HEADERS may produce multiple frames if the HPACK-encoded output exceeds
`remote_settings.max_frame_size` (outgoing frames must respect the
**peer's** advertised max_frame_size). The encoded payload is laid down
contiguously, then frame headers are written after it. iovecs interleave
frame headers and payload chunks so the network sees them in the correct
order without any byte-shifting.

```
/* Reserve 9 bytes for the first frame header (simple case: contiguous with payload) */
first_hdr_offset = send_buf_used
send_buf_used   += 9
encode_start     = send_buf_used

/* Encode HPACK block into send_buf starting at encode_start.
 * Cap output at opt_max_continuation_size to ensure the continuation
 * header area (after the payload) is always available. */
hpack_encode_block(enc_table, nva, nvlen,
    send_buf + encode_start,
    min(send_buf_cap - encode_start, opt_max_continuation_size),
    &encoded_len)

if encoded_len <= remote_settings.max_frame_size:
    /* Simple case: single HEADERS frame, header and payload contiguous */
    flags = END_HEADERS
    if end_stream: flags |= END_STREAM
    frame_hdr_write_at(send_buf + first_hdr_offset, encoded_len,
        HIVE_FRAME_HEADERS, flags, stream_id)
    send_iov[send_iov_count++] = {send_buf + first_hdr_offset, 9 + encoded_len}
    send_buf_used = encode_start + encoded_len

else:
    /* Split case: HEADERS + one or more CONTINUATION frames */
    /* Frame headers go AFTER the payload to avoid shifting bytes */
    n_frames = ceil(encoded_len / remote_settings.max_frame_size)
    cont_hdr_area = encode_start + encoded_len
    /* cont_hdr_area holds n_frames-1 CONTINUATION frame headers (9 bytes each) */

    pos = 0; frame_idx = 0
    while pos < encoded_len:
        chunk_len = min(remote_settings.max_frame_size, encoded_len - pos)
        is_last   = (pos + chunk_len >= encoded_len)
        hdr_flags = is_last ? END_HEADERS : 0

        if frame_idx == 0:
            /* Use the pre-reserved space at first_hdr_offset */
            /* END_STREAM goes on the HEADERS frame regardless of is_last */
            if end_stream: hdr_flags |= END_STREAM
            frame_hdr_write_at(send_buf + first_hdr_offset, chunk_len,
                HIVE_FRAME_HEADERS, hdr_flags, stream_id)
            send_iov[send_iov_count++] = {send_buf + first_hdr_offset, 9}
        else:
            /* CONTINUATION header goes at cont_hdr_area + (frame_idx-1)*9 */
            cont_offset = cont_hdr_area + (frame_idx - 1) * 9
            frame_hdr_write_at(send_buf + cont_offset, chunk_len,
                HIVE_FRAME_CONTINUATION, hdr_flags, stream_id)
            send_iov[send_iov_count++] = {send_buf + cont_offset, 9}

        send_iov[send_iov_count++] = {send_buf + encode_start + pos, chunk_len}

        pos += chunk_len; frame_idx++

    send_buf_used = cont_hdr_area + (n_frames - 1) * 9
```

The key invariant: the encoded payload is laid down contiguously at
`encode_start`, frame headers are written after it, and the iovecs reference
them in the correct transmission order. No byte-shifting required.

When `end_stream == 1`, perform stream state transition and fire
`on_stream_close` immediately after queuing (queue-time, before transmission),
matching the §6.5 DATA path and the §9.11 contract:

```
if end_stream:
    stream = lookup(stream_id)
    if stream != NULL:
        if stream->state == OPEN:
            stream->state = HALF_CLOSED_LOCAL
        else if stream->state == HALF_CLOSED_REMOTE:
            stream->state = CLOSED
            fire on_stream_close(session, stream_id, HIVE_H2_NO_ERROR, user_data)
            stream_close(session, stream)
        /* RESERVED_LOCAL: HEADERS+END_STREAM transitions to closed.
         * This is the push response final HEADERS path (no DATA). */
        else if stream->state == RESERVED_LOCAL:
            stream->state = CLOSED
            fire on_stream_close(session, stream_id, HIVE_H2_NO_ERROR, user_data)
            stream_close(session, stream)
else:
    /* No END_STREAM — handle RESERVED_LOCAL non-final push response HEADERS.
     * RFC 9113 §5.1.1: sending HEADERS on RESERVED_LOCAL (push response
     * headers with body to follow) transitions to HALF_CLOSED_REMOTE. */
    stream = lookup(stream_id)
    if stream != NULL && stream->state == RESERVED_LOCAL:
        stream->state = HALF_CLOSED_REMOTE
```

### 6.5 DATA Frame Queuing

Called from `send_queue_flush_data()` for streams with a pending data_source.
Outgoing DATA frames must honor `remote_settings.max_frame_size`.

```
/* Check flow control: skip if either window is 0 or negative */
if session->send_window <= 0 || stream->send_window <= 0: return

max_len = min(remote_settings.max_frame_size,
              (uint32_t)session->send_window,
              (uint32_t)stream->send_window)

hdr_offset    = send_buf_used
send_buf_used += 9   /* reserve for DATA frame header */
send_iov[send_iov_count++] = {send_buf + hdr_offset, 9}

/* read_fn signature: callback receives uint8_t **buf (pointer to pointer).
 * Copy path: callback fills *buf (library-provided buffer).
 * NO_COPY path: callback redirects *buf to its own memory. */
body_ptr = send_buf + send_buf_used   /* default: library buffer */
data_source.read_callback(session, stream_id,
    &body_ptr, max_len, &data_flags, &data_source, user_data)
  → returns bytes_written, sets data_flags, may redirect body_ptr

/* 0-byte return without EOF: body exhausted, trailers pending.
 * Discard the reserved iov entry (undo the 9-byte header reservation),
 * clear stream->data_source.read_callback = NULL so send_queue_flush_data
 * skips this stream on subsequent calls (data_source is an embedded struct,
 * not a pointer — clear the callback field to mark it inactive), and return
 * without queuing any DATA frame. The caller must then call
 * hive_submit_trailers() to send the trailer HEADERS frame with END_STREAM. */
if bytes_written == 0 && !(data_flags & HIVE_DATA_FLAG_EOF):
    send_iov_count--                          /* undo reserved header iov entry */
    send_buf_used -= 9                        /* undo reserved header space */
    stream->data_source.read_callback = NULL  /* mark source exhausted */
    return

if data_flags & HIVE_DATA_FLAG_NO_COPY:
    /* Zero-copy: iovec points to caller's redirected memory */
    send_iov[send_iov_count++] = {body_ptr, bytes_written}
    /* body_ptr must remain valid until this entire batch is fully drained */
else:
    /* Copy path: data is already in send_buf */
    send_iov[send_iov_count++] = {send_buf + send_buf_used, bytes_written}
    send_buf_used += bytes_written

/* Write DATA frame header — length now known */
hdr_flags = 0
if data_flags & HIVE_DATA_FLAG_EOF:
    hdr_flags |= 0x01   /* END_STREAM */
    /* Correct state transition depends on current stream state */
    if stream->state == OPEN:
        stream->state = HALF_CLOSED_LOCAL
        /* Stream stays live — decrement windows after frame_hdr_write_at below */
    else if stream->state == HALF_CLOSED_REMOTE:
        stream->state = CLOSED
        /* Decrement send windows BEFORE stream_close() — stream_close() returns
         * the slot to the free pool. Any field access after stream_close() is a
         * stale-slot read regardless of what the memory contains. */
        session->send_window -= bytes_written
        stream->send_window  -= bytes_written
        /* Fire on_stream_close immediately — the stream is logically closed
         * once END_STREAM is queued locally. The callback fires before
         * transmission; callers must not interpret it as "peer confirmed".
         * See §9.11 for the documented timing contract. */
        fire on_stream_close(session, stream_id, HIVE_H2_NO_ERROR, user_data)
        stream_close(session, stream)
        /* stream is freed; do NOT touch stream-> after this point */
        stream = NULL
frame_hdr_write_at(send_buf + hdr_offset, bytes_written,
    HIVE_FRAME_DATA, hdr_flags, stream_id)

/* For OPEN→HALF_CLOSED_LOCAL and all non-EOF frames, decrement send windows
 * here while stream is still live. stream == NULL means it was already
 * decremented and closed above. */
if stream != NULL:
    session->send_window -= bytes_written
    stream->send_window  -= bytes_written
```

### 6.6 Drain and Partial Send

The send callback returns `ssize_t` — the number of bytes it actually wrote.
The library retains any unsent portion and resumes on the next call.

```c
int
hive_session_send(hive_session_t *session)
{
    ssize_t  written;
    size_t   total, skip, i;
    /* eff_iov must be sized to the session's configured opt_max_send_iov, not
     * a fixed compile-time constant. HIVE_SEND_IOV_MAX is the compile-time cap
     * that opt_max_send_iov must not exceed (enforced in hive_options_set_max_send_iov).
     * The implementation must either:
     * (a) allocate eff_iov from the session arena at session creation
     *     (opt_max_send_iov entries), or
     * (b) declare eff_iov as a VLA sized to session->opt_max_send_iov, or
     * (c) cap opt_max_send_iov to HIVE_SEND_IOV_MAX and document this in §9.5.
     * A fixed-size stack array eff_iov[HIVE_SEND_IOV_MAX] is only safe if
     * opt_max_send_iov is strictly validated to never exceed HIVE_SEND_IOV_MAX. */
    struct iovec eff_iov[HIVE_SEND_IOV_MAX];  /* see note above */
    int      eff_cnt = 0;

    /* Only flush pending DATA sources when there is no partial batch outstanding.
     * While send_partial == 1, the existing iov batch must be fully drained
     * before new DATA frames are appended; appending to a partially-sent batch
     * corrupts the send_partial_offset accounting. */
    if (!session->send_partial)
        send_queue_flush_data(session);

    if (session->send_iov_count == 0)
        return HIVE_OK;

    /* Build effective iov by skipping send_partial_offset bytes */
    skip = session->send_partial_offset;
    total = 0;
    for (i = 0; i < (size_t)session->send_iov_count; i++) {
        size_t entry_len = session->send_iov[i].iov_len;
        total += entry_len;
        if (skip >= entry_len) {
            skip -= entry_len;   /* this entry already sent */
        } else {
            eff_iov[eff_cnt].iov_base =
                (char *)session->send_iov[i].iov_base + skip;
            eff_iov[eff_cnt].iov_len  = entry_len - skip;
            eff_cnt++;
            skip = 0;
        }
    }

    written = session->callbacks.send(session, eff_iov, eff_cnt,
        session->user_data);

    if (written < 0) {
        /* Fatal error — session is dead */
        session->session_state = HIVE_SESSION_CLOSED;
        return HIVE_ERR_PROTOCOL;
    }

    session->send_partial_offset += (size_t)written;

    if (session->send_partial_offset >= total) {
        /* All bytes sent — reset queue */
        session->send_iov_count      = 0;
        session->send_buf_used       = 0;
        session->send_partial_offset = 0;
        session->send_partial        = 0;
    } else {
        /* Partial write — retain unsent tail */
        session->send_partial = 1;
        /* hive_session_want_write() will return 1 */
    }
    return HIVE_OK;
}
```

The send callback fires exactly once per `hive_session_send()` call. On a
partial write, the next call to `hive_session_send()` rebuilds the effective
iov from `send_partial_offset`, skipping bytes already sent.

`send_buf_used` resets to zero only on a complete send, not on partial. While
partial state is active, `send_buf` memory is valid and iovec entries pointing
into it remain correct.

### 6.7 iovec Overflow

If appending a frame would exceed `opt_max_send_iov`, the internal append
function flushes the current batch before continuing:

```
if send_iov_count + needed_entries > opt_max_send_iov:
  hive_session_send(session)   /* may be a partial flush */
  if send_partial:             /* previous send was itself partial */
    return HIVE_ERR_WOULDBLOCK /* cannot queue more until fully drained */
  /* Queue is now empty — proceed */
  continue queuing
```

Note: the overflow flush may trigger the send callback during a `hive_submit_*()`
call, before the caller's explicit `hive_session_send()` call. This is the
only path where the send callback fires outside of an explicit send call. It
is documented in the send callback comments as a rare but possible occurrence.
In practice with `opt_max_send_iov = 512` this is not expected during normal
operation.

---

## 7. Performance Architecture

### 7.1 Receive Path: In-Place Parsing

The library parses frames directly from the caller's input buffer. For the
common case where a complete frame fits within one input chunk, no internal
reassembly buffer is needed. The receive function processes however many bytes
are available and returns the count consumed.

Copy count on the receive hot path:

| Frame / Header type | Copies |
|---|---|
| DATA frame body | 0 — callback receives pointer into caller's buffer |
| Indexed HPACK header | 0 — pointer into static or dynamic table |
| Huffman-encoded header name | 1 — decode into hpack_scratch_name |
| Huffman-encoded header value | 1 — decode into hpack_scratch_value |
| HPACK dynamic table entry | 1 — copy into arena on insert |
| HEADERS reassembly | 1 — copy into reassembly_buf |

### 7.2 hive_buf_t: Explicit Retain (by-pointer delivery)

Header name/value pairs are delivered via `hive_buf_t *` pointers to the
`on_header` callback. The callback receives pointers to library-owned handles,
not copies. The library clears `HIVE_BUF_VALID` in the actual handles
immediately after the callback returns, making any stale pointer
immediately detectable in debug builds.

The caller calls `hive_buf_retain()` only for headers it needs to keep —
typically 3–4 per request for a server (`:method`, `:path`, `host`,
`content-type`). In a debug build, ASan poisons the data regions after
callback return, catching accidental retain-after-return immediately.

### 7.3 Send Path: Scatter-Gather iovec

All outgoing frames accumulate in `send_iov` during `hive_session_recv()`
processing and `hive_submit_*()` calls. `hive_session_send()` fires the
`send` callback once with the full iovec. One `writev` or TLS write per event
loop iteration regardless of frame count. Control frames, HEADERS, and DATA
all share the same iovec; no intermediate buffering or copying between them.

### 7.4 HPACK Table Lookup

Linear scan for `max_size ≤ 16384`: scan from `ring_head - 1` backward
through `count` live entries. Most-recently-inserted first — common headers
repeat across requests and are found early. L1 cache resident. 5–30 ns at
default 4096-byte table.

Hash index for `max_size > 16384`: FNV-1a name hash + value hash, linear
probing with tombstone handling, 5–15 ns regardless of table size.

| Table size | Entries | Metadata | Scan cost |
|---|---|---|---|
| 4096 (default) | ~50 active | ~1.2 KB | 5–30 ns |
| 16384 | ~512 | ~12 KB | 20–100 ns |
| 65536 | ~2048 | ~48 KB | 5–15 ns (hash) |

### 7.5 Stream Lookup

Knuth multiplicative hash: `(stream_id * 2654435761u) >> (32 - table_bits)`.
One multiply and one shift. O(1) expected. Pre-allocated, L1 resident. Hash
table and slot array together occupy < 10 KB at 100 concurrent streams.

### 7.6 Huffman Decode

256-entry lookup table with iterative bit accumulator. L1 cache resident after
first HEADERS frame. 20–50× faster than tree traversal. Handles all HPACK code
lengths (5–30 bits) through iterative 8-bit lookups. Pure C11, no SIMD.

### 7.7 WINDOW_UPDATE Coalescing

Each stream and the connection track received-but-unacknowledged bytes in
`recv_consumed`. A WINDOW_UPDATE is queued only when the accumulated total
exceeds half the current receive window size.

```c
/* in RECV_DATA_PAYLOAD, after enforcing flow control and decrementing recv_window */
stream->recv_consumed  += n;
session->recv_consumed += n;

if (stream->recv_consumed > stream->recv_window / 2) {
    /* SECURITY: verify restored window does not exceed 2^31-1 per RFC 9113 §6.9.2.
     * In practice this cannot overflow given our controlled accounting, but
     * the check is mandatory. */
    if ((int64_t)stream->recv_window + stream->recv_consumed > 0x7FFFFFFF):
      return session_error(s, HIVE_ERR_FLOW_CONTROL, HIVE_H2_FLOW_CONTROL_ERROR, 0)
    send_queue_window_update(session, stream->stream_id,
        stream->recv_consumed);
    stream->recv_window   += stream->recv_consumed;  /* restore window */
    stream->recv_consumed  = 0;
}
if (session->recv_consumed > session->recv_window / 2) {
    if ((int64_t)session->recv_window + session->recv_consumed > 0x7FFFFFFF):
      return session_error(s, HIVE_ERR_FLOW_CONTROL, HIVE_H2_FLOW_CONTROL_ERROR, 0)
    send_queue_window_update(session, 0 /* connection */,
        session->recv_consumed);
    session->recv_window   += session->recv_consumed;
    session->recv_consumed  = 0;
}
```

**Flow-control accounting note**: The pseudocode above decrements `recv_window`
and increments `recv_consumed` by `cur_frame.length` (the full frame payload
including padding) when the first bytes of the DATA frame are processed, before
any partial delivery to the callback. This is correct — the window is consumed
by the frame's arrival, not by callback delivery. `recv_window` is restored
when the WINDOW_UPDATE is queued via `send_queue_window_update()`.

The queue-time restoration is a deliberate simplification. The peer cannot
exploit the brief interval between when we restore `recv_window` locally and
when the WINDOW_UPDATE is transmitted, because the peer does not know the
credit has been granted until it receives the WINDOW_UPDATE frame. In practice
the window is always restored before the peer can send more DATA, since the
peer is constrained by the window it last received from us. The simplification
avoids tracking a separate "pending credit" field in the session struct.

---

## 8. Security Architecture

### 8.1 HPACK Dynamic Table: Always Copy

Header strings entering the HPACK dynamic table are always copied into
library-owned arena memory. This is a correctness and security requirement —
the dynamic table persists for the connection lifetime. Storing pointers into
the caller's input buffer would cause silent corruption on buffer reuse. The
copy uses the session allocator — near-zero cost with an arena.

This is enforced unconditionally. There is no option to disable it.

### 8.2 HPACK Bomb Protection

Two independent limits, enforced incrementally in `hpack_decode_block()`:

**Limit 1 — Decoded header list size**: configurable via
`hive_options_set_max_header_list_size()`, default 65536 bytes. Running sum
of `name_len + value_len + 32` for each decoded header (RFC 7541 §4.1 defines
the size of a header field as name_len + value_len + 32). Exceeded: RST_STREAM
PROTOCOL_ERROR. Decoding continues for the remainder of the block to maintain
dynamic table consistency.

**Limit 2 — Header count**: configurable via
`hive_options_set_max_header_count()`, default 100. Running count of decoded
headers. Exceeded: RST_STREAM PROTOCOL_ERROR. Same continued-decoding
requirement.

Neither limit requires buffering the entire decoded header list.

### 8.3 CONTINUATION Flood Protection

Hard cap on total compressed bytes in any HEADERS + CONTINUATION reassembly
sequence. Configurable via `hive_options_set_max_continuation_size()`,
default `opt_max_frame_size × 4 = 65536` bytes.

Enforced during `RECV_HEADERS_PAYLOAD` and `RECV_CONTINUATION_PAYLOAD`
before any bytes are written into `reassembly_buf`. Exceeding the cap is a
**connection error** (GOAWAY PROTOCOL_ERROR), not a stream error. This
classification follows RFC 9113 §4.3: any frame-sequence violation that
affects the entire connection is a connection error. The check prevents
flood attacks before HPACK decoding begins.

```c
if (session->reassembly_len + n > session->opt_max_continuation_size) {
    /*
     * SECURITY: CONTINUATION flood — connection error per RFC 9113 §4.3.
     * This is intentionally a GOAWAY, not RST_STREAM.
     */
    return session_error(session, HIVE_ERR_PROTOCOL,
        HIVE_H2_PROTOCOL_ERROR, 0);
}
```

### 8.4 SETTINGS Flood Protection

Hive maintains two separate SETTINGS counters:

`pending_settings` ring + `pending_count`: tracks **outbound** SETTINGS we
have sent and are awaiting peer ACK for. Used to match incoming SETTINGS ACK
frames to the correct sent SETTINGS. Size: `opt_max_settings_pending` entries.

`inbound_settings_count`: tracks **inbound** SETTINGS frames received but not
yet ACK'd. This is the flood protection counter. When a SETTINGS frame is
received:

```c
session->inbound_settings_count++;
if (session->inbound_settings_count > session->opt_max_settings_pending)
    return session_error(session, HIVE_ERR_PROTOCOL,
        HIVE_H2_PROTOCOL_ERROR, 0);   /* GOAWAY */
```

`inbound_settings_count` is decremented when the SETTINGS ACK is queued into
the send buffer (not when received, not when transmitted — when queued). The
fixed-size design prevents unbounded accumulation. With the default of
`opt_max_settings_pending = 3`, at most 3 inbound SETTINGS may be awaiting
ACK at any time.

### 8.5 RST_STREAM Flood Detection

Rolling rate counter in the session struct:

```c
uint32_t rst_flood_count;         /* RST_STREAM frames in current window */
uint64_t rst_flood_window_start;  /* monotonic seconds (CLOCK_MONOTONIC) */
```

On each received RST_STREAM:

```c
uint64_t now = (uint64_t)monotonic_secs();  /* CLOCK_MONOTONIC */
if (now - session->rst_flood_window_start >= session->opt_rst_flood_window_secs) {
    session->rst_flood_count       = 0;
    session->rst_flood_window_start = now;
}
session->rst_flood_count++;
if (session->rst_flood_count > session->opt_rst_flood_threshold) {
    /*
     * SECURITY: RST_STREAM flood threshold exceeded.
     * Library does not act unilaterally — caller decides response.
     */
    if (session->callbacks.on_rst_stream_flood != NULL)
        session->callbacks.on_rst_stream_flood(session,
            session->rst_flood_count, session->user_data);
}
```

Default: 100 RST_STREAM frames in a 10-second window triggers the callback.

### 8.6 Stream ID Exhaustion

When the highest seen remote stream ID exceeds `0x7FFFFFFFu - 1000u`:

```c
if (stream_id > (0x7FFFFFFFu - 1000u) && !session->goaway_sent)
    hive_submit_goaway_prepare(session);
```

Threshold of 1000 allows in-flight requests to complete before shutdown.
The caller is notified via `on_goaway` when the session closes fully.

### 8.7 Receive-Side Flow Control Enforcement

Hive enforces the receive window at both connection and stream level. Flow
control accounting covers the **full** DATA frame payload including padding
bytes (RFC 9113 §6.1, §6.9.1). The `fc_accounted` flag ensures enforcement
and window decrement happen exactly once per frame against `cur_frame.length`,
regardless of how many `hive_session_recv()` calls are needed to deliver the
frame. Padding bytes do not escape flow-control accounting even though they
are not delivered to `on_data_chunk`.

If the peer sends DATA in excess of the receive window, the violation is caught
on the first entry to `RECV_DATA_PAYLOAD` for that frame:

```c
/* SECURITY: receive-side flow control enforcement — once per frame */
if (!fc_accounted) {
    if ((uint32_t)cur_frame.length > (uint32_t)stream->recv_window)
        return stream_error(session, stream_id, HIVE_ERR_FLOW_CONTROL,
            HIVE_H2_FLOW_CONTROL_ERROR);
    if ((uint32_t)cur_frame.length > (uint32_t)session->recv_window)
        return session_error(session, HIVE_ERR_FLOW_CONTROL,
            HIVE_H2_FLOW_CONTROL_ERROR, 0);
    stream->recv_window  -= cur_frame.length;
    session->recv_window -= cur_frame.length;
    fc_accounted = 1;
}
```

This prevents a peer from forcing unbounded DATA processing regardless of
flow control limits.

### 8.8 Buffer Lifetime Enforcement

**`on_header` callbacks**: `hive_buf_t` handles are passed **by pointer**.
The callback receives `hive_buf_t *name` and `hive_buf_t *value` pointing
to library-owned handles. On callback return, the library clears
`HIVE_BUF_VALID` in the actual handle objects. In ASan debug builds, the
underlying data regions (`hpack_scratch_name`, `hpack_scratch_value`,
`reassembly_buf`) are poisoned via `HIVE_ASAN_POISON`. Any access to a stale
pointer after callback return produces an immediate ASan abort.

`hive_buf_retain()` must be called only within the callback — it copies
the data to arena memory and sets `HIVE_BUF_OWNED`. The copy is not poisoned.
Attempting to call `hive_buf_retain()` after callback return asserts in debug
builds.

**`on_data_chunk` callbacks**: the `data` pointer is directly into the
caller's input buffer. Valid only within the callback. This is a documented
lifetime contract; it is not mechanically enforced by ASan because the
library cannot safely poison caller-owned memory. In non-debug production
builds, accessing `data` after callback return is undefined behaviour that
will not be caught automatically. The contract must be followed by the caller.

Both contracts are stated prominently inline in the callback struct
documentation in `hive.h`.

---

## 9. Public API

### 9.1 Allocator Interface

```c
typedef void *(*hive_malloc_fn) (size_t size, void *ctx);
typedef void  (*hive_free_fn)   (void *ptr, void *ctx);
typedef void *(*hive_calloc_fn) (size_t nmemb, size_t size, void *ctx);
typedef void *(*hive_realloc_fn)(void *ptr, size_t size, void *ctx);

typedef struct {
    hive_malloc_fn  malloc;
    hive_free_fn    free;
    hive_calloc_fn  calloc;
    hive_realloc_fn realloc;
    void           *ctx;   /* passed to every allocator call */
} hive_mem_t;
```

Pass NULL to `hive_session_*_new()` to use system malloc/free/calloc/realloc
with ctx = NULL. If a non-NULL `hive_mem_t` is passed, all four function
pointers must be non-NULL.

The allocator is called in two contexts after session creation: HPACK dynamic
table entry insertion (one allocation per inserted header) and
`hive_buf_retain()` calls. Both are expected and go through the session
allocator. With an arena allocator they are effectively free-list pops.

### 9.2 Buffer Handle

```c
typedef struct {
    const uint8_t *data;
    size_t         len;
    uint32_t       flags;
} hive_buf_t;

#define HIVE_BUF_VALID  0x01u  /* set when handle is live; cleared on callback return */
#define HIVE_BUF_OWNED  0x02u  /* data was retained; caller must call hive_buf_free() */

/*
 * Retain the buffer contents beyond the callback lifetime.
 * Allocates a copy via the session allocator; sets HIVE_BUF_OWNED.
 * Must be called only within the on_header callback.
 * Asserts in debug builds if called after callback return.
 * Returns HIVE_OK on success, HIVE_ERR_NOMEM on allocation failure.
 */
int  hive_buf_retain(hive_session_t *session, hive_buf_t *buf);

/*
 * Free a retained buffer (HIVE_BUF_OWNED set).
 * No-op on non-owned or already-freed buffers.
 */
void hive_buf_free(hive_session_t *session, hive_buf_t *buf);
```

### 9.3 Name/Value Pair

```c
typedef struct {
    const uint8_t *name;
    const uint8_t *value;
    size_t         namelen;
    size_t         valuelen;
    uint8_t        flags;
} hive_nv_t;

#define HIVE_NV_FLAG_NO_INDEX  0x01u  /* never-indexed — do not store in HPACK table */
```

### 9.4 Data Source

```c
typedef struct hive_data_source hive_data_source_t;

typedef ssize_t (*hive_data_source_read_fn)(
    hive_session_t     *session,
    uint32_t            stream_id,
    uint8_t           **buf,        /* in/out: library provides *buf; callback may redirect */
    size_t              length,
    uint32_t           *data_flags,
    hive_data_source_t *source,
    void               *user_data);

struct hive_data_source {
    hive_data_source_read_fn  read_callback;
    void                     *ptr;   /* caller context: fd, mmap ptr, etc. */
};

#define HIVE_DATA_FLAG_EOF      0x01u  /* no more body data; library sets END_STREAM */
#define HIVE_DATA_FLAG_NO_COPY  0x02u  /* callback redirected *buf to caller-owned memory */
```

The `read_callback` receives `uint8_t **buf` — a pointer to a pointer.

**Copy path** (default): the library sets `*buf` to point into `send_buf`.
The callback writes body bytes into `**buf` and returns bytes written. The
library uses the data already in `send_buf`.

**Zero-copy path** (`HIVE_DATA_FLAG_NO_COPY`): the callback redirects `*buf`
to point to its own memory (mmap region, read buffer, etc.) and sets the
`NO_COPY` flag. The library uses the redirected pointer directly in the iovec
without copying.

**NO_COPY lifetime**: the caller's memory must remain valid until
`hive_session_want_write()` returns 0 after a `hive_session_send()` call
that fully drains the batch containing this NO_COPY entry. The memory must
remain valid across an arbitrary number of partial-send cycles. It is NOT
sufficient to release the buffer after the first send callback invocation —
partial sends leave the iovec referenced across multiple `hive_session_send()`
calls. Releasing the buffer after a partial send causes use-after-free when
the library resends the unsent tail.

**Contract**: the data_source is submitted once via `hive_submit_response()`
or `hive_submit_request()` and is fixed for the lifetime of that stream. The
`read_callback` will be called during `hive_session_send()` whenever the
send window allows more DATA frames. The caller cannot replace or modify the
data_source after submission. To abort body transmission, call
`hive_submit_rst_stream()`.

### 9.5 Options

```c
hive_options_t *hive_options_new(void);   /* uses system malloc */
void            hive_options_free(hive_options_t *);
```

`hive_options_new()` always uses system malloc regardless of any custom
allocator — options are created once at startup, not per connection.

Option ranges marked with † are deliberate implementation limits narrower than
the protocol allows; the rationale is noted.

| Function | Default | Valid range | Notes |
|---|---|---|---|
| `hive_options_set_header_table_size(opt, v)` | 4096 | 0–65536 | SETTINGS_HEADER_TABLE_SIZE advertised |
| `hive_options_set_enable_push(opt, v)` | 1 | 0–1 | SETTINGS_ENABLE_PUSH; client sessions only† †Servers must never include this in their outbound SETTINGS per RFC 9113 §6.5.2 |
| `hive_options_set_max_concurrent_streams(opt, v)` | 100 | 1–65535† | †Protocol allows up to 2^31-1; 65535 is sufficient for all known use cases |
| `hive_options_set_initial_window_size(opt, v)` | 65535 | 1–2147483647 | SETTINGS_INITIAL_WINDOW_SIZE; 0 is excluded† †RFC allows 0 but it means no DATA can be sent until a WINDOW_UPDATE arrives |
| `hive_options_set_max_frame_size(opt, v)` | 16384 | 16384–16777215 | SETTINGS_MAX_FRAME_SIZE |
| `hive_options_set_max_header_list_size(opt, v)` | 65536 | 1–16777215 | decoded header list limit |
| `hive_options_set_max_header_count(opt, v)` | 100 | 1–65535 | headers per HEADERS block |
| `hive_options_set_max_continuation_size(opt, v)` | 65536 | 16384–16777215 | compressed reassembly cap |
| `hive_options_set_max_settings_pending(opt, v)` | 3 | 1–255 | max unACK'd outbound SETTINGS; also used as inbound flood threshold |
| `hive_options_set_rst_stream_flood_threshold(opt, v)` | 100 | 1–65535 | RST_STREAM rate limit |
| `hive_options_set_rst_stream_flood_window_secs(opt, v)` | 10 | 1–3600 | RST_STREAM window |
| `hive_options_set_max_send_iov(opt, v)` | 512 | 64–HIVE_SEND_IOV_MAX | iovec array size. Upper bound is the compile-time constant HIVE_SEND_IOV_MAX (default 1024). Must not exceed HIVE_SEND_IOV_MAX — enforced by hive_options_set_max_send_iov() returning HIVE_ERR_INVALID. |
| `hive_options_set_max_header_string_size(opt, v)` | 8192 | 256–65536 | Huffman scratch buffer size (per buffer; two buffers allocated) |
| `hive_options_set_no_http_messaging(opt, v)` | 0 | 0–1 | 1 = skip RFC 9113 §8 validation |
| `hive_options_set_no_auto_ping_ack(opt, v)` | 0 | 0–1 | 1 = suppress automatic PING ACK; on_ping fires instead |

### 9.6 Callbacks

```c
typedef struct {
    /*
     * A HEADERS block is beginning on stream_id.
     * Called once before any on_header callbacks for this block.
     * Return HIVE_OK to continue; return HIVE_ERR_* to signal error.
     *
     * NOTE on stream errors during HPACK decode: returning an error from
     * on_begin_headers (or from on_header) causes the library to suppress
     * further on_header delivery for this block, but decoding continues to
     * maintain dynamic table consistency (RFC 7541 §2.3.2). A RST_STREAM is
     * sent for the stream after the full block is consumed. Only genuine
     * HPACK encoding errors (returning HIVE_ERR_COMPRESSION) terminate the
     * connection immediately.
     */
    int (*on_begin_headers)(hive_session_t *session,
            uint32_t stream_id, void *user_data);

    /*
     * One decoded header name/value pair.
     *
     * name and value are pointers to hive_buf_t handles owned by the library.
     * LIFETIME: valid only within this callback. Call hive_buf_retain() to
     * copy the data to session allocator memory. After this callback returns,
     * HIVE_BUF_VALID is cleared in the handle and the data regions are
     * poisoned in ASan debug builds. Do not store name or value pointers
     * beyond the callback — retain or copy.
     *
     * flags: HIVE_NV_FLAG_NO_INDEX if the header is never-indexed.
     *
     * Return HIVE_OK to continue; return HIVE_ERR_* to signal error.
     */
    int (*on_header)(hive_session_t *session,
            uint32_t stream_id,
            hive_buf_t *name, hive_buf_t *value,
            uint8_t flags, void *user_data);

    /*
     * All headers for stream_id have been delivered.
     * end_stream: 1 if no DATA frames follow (e.g. GET request or HEAD).
     * Return HIVE_OK to continue; return HIVE_ERR_* to signal error.
     */
    int (*on_headers_complete)(hive_session_t *session,
            uint32_t stream_id, int end_stream, void *user_data);

    /*
     * DATA bytes received on stream_id.
     *
     * data: pointer directly into the caller's buffer passed to
     *       hive_session_recv(). Zero copy — no internal DATA staging.
     *
     * LIFETIME: valid only within this callback. Copy if needed beyond
     * callback return. This lifetime is a documented contract only; it is
     * not enforced by ASan because the pointer is into caller-owned memory
     * which the library cannot safely poison.
     *
     * NOTE: fires for available bytes, not per complete frame. A single DATA
     * frame may produce multiple on_data_chunk calls if its payload spans
     * multiple hive_session_recv() calls. Callers must handle partial delivery.
     *
     * Return HIVE_OK to continue; return HIVE_ERR_* to signal error.
     */
    int (*on_data_chunk)(hive_session_t *session,
            uint32_t stream_id,
            const uint8_t *data, size_t len, void *user_data);

    /*
     * Stream stream_id has been closed.
     * error_code: HIVE_H2_NO_ERROR for clean close; RFC 9113 error code otherwise.
     * Return value ignored.
     */
    int (*on_stream_close)(hive_session_t *session,
            uint32_t stream_id, uint32_t error_code, void *user_data);

    /*
     * Peer SETTINGS frame received and applied.
     * Fired after the SETTINGS ACK is queued.
     * Return value ignored.
     */
    int (*on_settings)(hive_session_t *session, void *user_data);

    /*
     * Our outbound SETTINGS was acknowledged by the peer.
     * Fired when a SETTINGS ACK is received and matched to a pending entry.
     * Return value ignored.
     */
    void (*on_settings_ack)(hive_session_t *session, void *user_data);

    /*
     * Server push promised on stream_id (client role only).
     * promised_stream_id: the reserved stream ID for the push response.
     *
     * Header delivery: the promised request headers (:method, :path, etc.)
     * are delivered via on_begin_headers and on_header callbacks BEFORE this
     * callback fires, attributed to stream_id (the carrying stream). The
     * caller must accumulate those headers and correlate them with the
     * promised_stream_id supplied here. A future API revision may add a
     * dedicated push-header callback to avoid this attribution ambiguity.
     *
     * Return HIVE_OK to accept the push.
     * Return HIVE_ERR_REFUSED_STREAM to reject — the library sends
     * RST_STREAM REFUSED_STREAM on the promised stream and closes it.
     * Any other return value is treated as HIVE_OK (push accepted).
     */
    int (*on_push_promise)(hive_session_t *session,
            uint32_t stream_id, uint32_t promised_stream_id,
            void *user_data);

    /*
     * GOAWAY received from peer.
     * last_stream_id: highest stream ID peer will process.
     * error_code:     RFC 9113 wire error code.
     * debug_data:     optional opaque debug data from the GOAWAY frame.
     *                 Pointer is into reassembly_buf — valid only within callback.
     *                 NULL only if the GOAWAY frame contained no debug data
     *                 (debug_len == 0 in the wire frame).
     *                 If debug data was present but exceeded reassembly_buf
     *                 capacity, a truncated prefix is delivered (debug_len is
     *                 the number of bytes actually delivered, not the full
     *                 wire length). Callers must not assume debug_data is the
     *                 complete debug payload when the GOAWAY frame is large.
     * debug_len:      byte length of debug_data delivered (0 if absent).
     * Return value ignored.
     */
    void (*on_goaway)(hive_session_t *session,
            uint32_t last_stream_id, uint32_t error_code,
            const uint8_t *debug_data, size_t debug_len,
            void *user_data);

    /*
     * PING received from peer (only fired when opt_no_auto_ping_ack == 1).
     * opaque_data: exactly 8 bytes from the PING frame.
     * When this callback fires, the library has NOT sent a PING ACK.
     * The caller should call hive_submit_ping_ack() to respond.
     * Return value ignored.
     */
    void (*on_ping)(hive_session_t *session,
            const uint8_t opaque_data[8], void *user_data);

    /*
     * PING ACK received from peer.
     * opaque_data: the 8 bytes echoed from our original PING frame.
     * Use to measure RTT by matching against the value passed to hive_submit_ping().
     * Return value ignored.
     */
    void (*on_ping_ack)(hive_session_t *session,
            const uint8_t opaque_data[8], void *user_data);

    /*
     * RST_STREAM rate threshold exceeded.
     * rate: RST_STREAM frames received in the current time window.
     * The library does not take unilateral action. The caller may
     * call hive_submit_goaway_prepare() or close the connection.
     * Return value ignored.
     */
    void (*on_rst_stream_flood)(hive_session_t *session,
            uint32_t rate, void *user_data);

    /*
     * The library has detected a connection-level protocol error and is
     * about to send GOAWAY. Fired before GOAWAY is queued.
     * hive_err:    library error code (HIVE_ERR_PROTOCOL, etc.)
     * h2_error:    HTTP/2 wire error code (HIVE_H2_PROTOCOL_ERROR, etc.)
     * Use for logging. Do not call any submit functions from this callback.
     * Return value ignored.
     */
    void (*on_connection_error)(hive_session_t *session,
            int hive_err, uint32_t h2_error_code, void *user_data);

    /*
     * Library has frames ready to send. Called by hive_session_send().
     *
     * May also be called during hive_submit_*() in the rare iovec overflow
     * case (§6.7). This is the only path where send fires outside an
     * explicit hive_session_send() call.
     *
     * iov: scatter-gather array of iovcnt entries.
     * Entries may point into library arena memory (headers, control frames)
     * or into caller memory (DATA body with HIVE_DATA_FLAG_NO_COPY).
     *
     * Returns ssize_t: number of bytes written (0..total).
     * Return -1 on fatal error — session is marked CLOSED.
     * The library retains any unsent portion for the next call.
     *
     * The caller is responsible for calling hive_session_send() again
     * if hive_session_want_write() returns 1 after a partial send.
     */
    ssize_t (*send)(hive_session_t *session,
            const struct iovec *iov, int iovcnt, void *user_data);

} hive_callbacks_t;
```

Any callback field set to NULL is silently skipped. The callbacks struct is
copied by value into session memory at session creation.

### 9.7 Session Lifecycle

```c
/*
 * Create a server-role session.
 *
 * mem:       allocator; NULL for system malloc.
 * options:   session options; NULL for defaults.
 * callbacks: event callbacks; must not be NULL; send field must not be NULL.
 * user_data: passed unchanged to every callback.
 *
 * Returns session pointer on success.
 * Returns NULL on allocation failure.
 * On return, the server SETTINGS preface is queued. Call hive_session_send().
 */
hive_session_t *hive_session_server_new(
    const hive_mem_t       *mem,
    const hive_options_t   *options,
    const hive_callbacks_t *callbacks,
    void                   *user_data);

/*
 * Create a client-role session.
 *
 * On return, the client connection preface (24-byte PRI * magic + SETTINGS)
 * is queued. Call hive_session_send() to flush.
 */
hive_session_t *hive_session_client_new(
    const hive_mem_t       *mem,
    const hive_options_t   *options,
    const hive_callbacks_t *callbacks,
    void                   *user_data);

/*
 * Create a server-role session from an HTTP/1.1 Upgrade context.
 *
 * settings_payload: base64url-decoded value of the HTTP2-Settings header
 *   from the client's Upgrade request. Must not be NULL — a valid h2c
 *   upgrade MUST include exactly one HTTP2-Settings header field per RFC
 *   9113 §3.2.1. If the header was absent, the Upgrade request is not a
 *   valid h2c upgrade; reject it at the HTTP/1.1 layer and do not call
 *   this function.
 * settings_len: byte length of settings_payload. Must be > 0 if
 *   settings_payload is non-NULL. Pass a zero-length payload (empty
 *   SETTINGS) if the header was present but empty.
 *
 * Returns session pointer on success. NULL on allocation failure or
 * malformed settings_payload.
 *
 * After this call:
 *   - Stream 1 exists in HALF_CLOSED_REMOTE state.
 *   - The server SETTINGS preface is queued; call hive_session_send().
 *   - Call hive_session_feed_upgrade_headers() to inject the HTTP/1.1
 *     request as stream 1's opening HEADERS.
 */
hive_session_t *hive_session_server_upgrade(
    const hive_mem_t       *mem,
    const hive_options_t   *options,
    const hive_callbacks_t *callbacks,
    void                   *user_data,
    const uint8_t          *settings_payload,
    size_t                  settings_len);

/*
 * Inject the original HTTP/1.1 upgrade request as stream 1 HEADERS.
 *
 * Must be called after hive_session_server_upgrade() before any
 * hive_session_recv() calls. Fires on_begin_headers, on_header × nvlen,
 * and on_headers_complete through the normal callback path, ensuring all
 * session invariants (bomb limits, HTTP messaging validation) are enforced.
 *
 * nva:        the HTTP/1.1 request headers translated to HTTP/2 pseudo-headers
 *             and regular headers. Must include :method, :path, :scheme.
 * nvlen:      number of entries in nva.
 * end_stream: must always be 1. Stream 1 for an h2c upgrade is implicitly
 *             half-closed from the client per RFC 9113 §3.2 — the request
 *             body (if any) was sent over HTTP/1.1 before the connection
 *             upgrade and cannot arrive as HTTP/2 DATA frames. Passing 0 is
 *             a caller error.
 *
 * Returns HIVE_OK on success.
 * Returns HIVE_ERR_PROTOCOL if messaging validation fails.
 */
int hive_session_feed_upgrade_headers(hive_session_t *session,
    const hive_nv_t *nva, size_t nvlen, int end_stream);

/*
 * Destroy session and free all associated memory via the session's allocator.
 * With an arena allocator this is one free() call.
 * After this call the session pointer is invalid.
 */
void hive_session_free(hive_session_t *session);
```

### 9.8 Receive and Send

```c
/*
 * Feed incoming bytes to the session.
 *
 * data: bytes read from the network (after TLS decryption if applicable).
 * len:  number of bytes available.
 *
 * The library processes bytes in-place from data, firing callbacks as
 * frames are parsed. Do not modify or free data until this function returns.
 *
 * Returns bytes consumed (>= 0) on success.
 * Returns HIVE_ERR_* (< 0) on fatal session error. On fatal error, a GOAWAY
 * has been queued in the send buffer. The caller MUST call hive_session_send()
 * once more to attempt to flush the queued GOAWAY to the peer, then call
 * hive_session_free(). No other calls (recv, submit) are valid after a fatal
 * error return.
 *
 * Partial consumption is normal when a frame spans multiple calls.
 * Call again with the remaining bytes on the next recv event.
 *
 * Does NOT send anything. Call hive_session_send() after this to flush
 * any queued responses (SETTINGS ACK, WINDOW_UPDATE, error frames, etc.)
 */
ssize_t hive_session_recv(hive_session_t *session,
    const uint8_t *data, size_t len);

/*
 * Drain all pending outgoing frames.
 *
 * Drives pending DATA sources (calls data_source.read_callback for streams
 * with pending body data, within flow control limits), then fires the send
 * callback once with the full batched iovec.
 *
 * The send callback returns the number of bytes written. If fewer than the
 * total queued bytes were written, the library retains the unsent tail and
 * hive_session_want_write() returns 1. The caller must register for
 * write-readiness (EPOLLOUT / EVFILT_WRITE) and call hive_session_send()
 * again when the socket is writable.
 *
 * Returns HIVE_OK on success (including partial send).
 * Returns HIVE_ERR_* on fatal error (session must be freed).
 */
int hive_session_send(hive_session_t *session);

/*
 * Returns 1 if there are bytes to send (either queued frames or unsent
 * tail from a partial write). Returns 0 if the send queue is fully drained.
 *
 * Use to register EPOLLOUT / EVFILT_WRITE interest after a partial send.
 * Advisory — the caller may choose to send more aggressively.
 */
int hive_session_want_write(hive_session_t *session);

/*
 * Returns 1 if the session should continue receiving data.
 * Returns 0 after GOAWAY is received from the peer.
 *
 * This is advisory. After GOAWAY recv, streams with ID <= last_stream_id
 * may still have in-flight frames. The caller may choose to continue
 * reading to process those frames. Once all in-flight streams are closed,
 * stop reading and call hive_session_free().
 */
int hive_session_want_read(hive_session_t *session);
```

### 9.9 Submit — Server Role

```c
/*
 * Submit a response for stream_id.
 *
 * nva:         response headers. Must include :status.
 * nvlen:        number of entries.
 * data_source:  if non-NULL, DATA frames follow via read_callback.
 *               NULL for header-only responses (204, 304, etc.).
 *
 * CONTRACT: data_source is fixed for the lifetime of this stream.
 * read_callback is called during hive_session_send() when the send window
 * allows more DATA frames. To abort body transmission: hive_submit_rst_stream().
 *
 * Returns HIVE_OK on success.
 * Returns HIVE_ERR_STREAM_CLOSED if stream_id is not open.
 * Returns HIVE_ERR_INVALID_ARG if nva is NULL or nvlen is 0.
 */
int hive_submit_response(hive_session_t *session,
    uint32_t stream_id,
    const hive_nv_t *nva, size_t nvlen,
    const hive_data_source_t *data_source);

/*
 * Submit trailers for stream_id (HEADERS with END_STREAM, no DATA follows).
 *
 * Trailers must not contain pseudo-headers.
 *
 * Call sequence for a response with trailers:
 * 1. hive_submit_response() with a data_source — body DATA frames follow
 * 2. data_source.read_callback returns data WITHOUT HIVE_DATA_FLAG_EOF
 *    for all body chunks except the last
 * 3. When the body is exhausted, the read_callback returns 0 bytes (no EOF
 *    flag) — this signals no more DATA, leaving the stream open for trailers
 * 4. Call hive_submit_trailers() — sends HEADERS with END_STREAM
 *
 * Do NOT set HIVE_DATA_FLAG_EOF on the final DATA frame when trailers follow.
 * HIVE_DATA_FLAG_EOF closes the local side immediately with END_STREAM on the
 * DATA frame, leaving no opportunity to send trailers.
 *
 * Returns HIVE_OK on success.
 * Returns HIVE_ERR_STREAM_CLOSED if stream is not in appropriate state.
 */
int hive_submit_trailers(hive_session_t *session,
    uint32_t stream_id,
    const hive_nv_t *nva, size_t nvlen);

/*
 * Submit an interim (1xx) response for stream_id.
 *
 * nva must contain :status with a 1xx value. No DATA follows.
 * Multiple interim responses may be submitted before the final response.
 *
 * Returns HIVE_OK on success.
 * Returns HIVE_ERR_STREAM_CLOSED if stream is not open.
 */
int hive_submit_interim_response(hive_session_t *session,
    uint32_t stream_id,
    const hive_nv_t *nva, size_t nvlen);

/*
 * Submit a PUSH_PROMISE on stream_id (server role only).
 *
 * promise_nva:  headers of the synthetic push request.
 *               Must include :method, :path, :scheme, :authority.
 * Returns the reserved promised stream ID (even, > 0) on success.
 * Returns HIVE_ERR_PROTOCOL if SETTINGS_ENABLE_PUSH = 0.
 * Returns HIVE_ERR_STREAM_CLOSED if stream_id is not open.
 * Returns HIVE_ERR_REFUSED_STREAM if the total stream slot pool is
 *   exhausted (stream_open_count >= opt_max_concurrent_streams). This
 *   is independent of the peer's SETTINGS_MAX_CONCURRENT_STREAMS.
 *   See §2.9 for the slot-pool total-cap constraint.
 */
int32_t hive_submit_push_promise(hive_session_t *session,
    uint32_t stream_id,
    const hive_nv_t *promise_nva, size_t promise_nvlen);

/*
 * Submit RST_STREAM for stream_id.
 * error_code: HIVE_H2_NO_ERROR for clean cancellation.
 * Returns HIVE_OK. Returns HIVE_ERR_STREAM_CLOSED if already closed.
 */
int hive_submit_rst_stream(hive_session_t *session,
    uint32_t stream_id, uint32_t error_code);

/*
 * Initiate two-phase graceful shutdown — phase 1.
 *
 * Sends GOAWAY with last_stream_id = 0x7FFFFFFF and error code NO_ERROR.
 * This signals the peer to stop opening new streams while allowing existing
 * ones to complete. After this call, hive_session_want_read() still returns 1
 * for in-flight streams.
 *
 * Call hive_submit_goaway_final() after all in-flight streams are closed.
 *
 * Returns HIVE_OK. Returns HIVE_ERR_SESSION_CLOSED if already in GOAWAY state.
 */
int hive_submit_goaway_prepare(hive_session_t *session);

/*
 * Complete two-phase graceful shutdown — phase 2.
 *
 * Sends GOAWAY with the actual last_stream_id_remote (highest stream ID
 * the server has processed) and the given error_code.
 *
 * Call after hive_submit_goaway_prepare() and after all in-flight streams
 * have completed. In-flight streams on the caller's side should have been
 * responded to or RST'd before this call.
 *
 * opaque_data: optional debug data. NULL for none. Maximum 256 bytes.
 * Returns HIVE_OK. Returns HIVE_ERR_SESSION_CLOSED if session already closed.
 */
int hive_submit_goaway_final(hive_session_t *session,
    uint32_t error_code,
    const uint8_t *opaque_data, size_t opaque_data_len);

/*
 * Submit a PING frame.
 * opaque_data: exactly 8 bytes. Echoed back in on_ping_ack.
 * Returns HIVE_OK on success.
 */
int hive_submit_ping(hive_session_t *session,
    const uint8_t opaque_data[8]);

/*
 * Submit a PING ACK explicitly.
 * Used when opt_no_auto_ping_ack == 1 and on_ping has fired.
 * opaque_data: the 8 bytes from the received PING (from on_ping callback).
 * Returns HIVE_OK on success.
 */
int hive_submit_ping_ack(hive_session_t *session,
    const uint8_t opaque_data[8]);
```

### 9.10 Submit — Client Role

```c
/*
 * Submit a request (client role only).
 *
 * nva:          request headers. Must include :method, :path, :scheme.
 * nvlen:         number of entries in nva.
 * data_source:  if non-NULL, request body follows. NULL for GET and HEAD.
 *
 * Honors peer SETTINGS_MAX_CONCURRENT_STREAMS: returns HIVE_ERR_REFUSED_STREAM
 * if the peer's concurrent stream limit is already reached.
 * Also returns HIVE_ERR_REFUSED_STREAM if the total stream slot pool is
 * exhausted (stream_open_count >= opt_max_concurrent_streams), regardless
 * of the peer's limit. See §2.9 for the slot-pool total-cap constraint.
 *
 * Returns the new stream ID (odd, > 0) on success.
 * Returns HIVE_ERR_SESSION_CLOSED if session is in GOAWAY state.
 * Returns HIVE_ERR_REFUSED_STREAM if peer's max_concurrent_streams reached
 *   or if the local slot pool is exhausted.
 * Returns HIVE_ERR_NOMEM on allocation failure.
 */
int32_t hive_submit_request(hive_session_t *session,
    const hive_nv_t *nva, size_t nvlen,
    const hive_data_source_t *data_source);
```

### 9.11 Introspection

```c
/* Current connection-level send window (peer's recv window). */
int32_t hive_session_get_remote_window_size(hive_session_t *session);

/* Current stream-level send window for stream_id. */
int32_t hive_session_get_stream_remote_window_size(
    hive_session_t *session, uint32_t stream_id);

/* Local SETTINGS currently in effect (what we advertised). */
int hive_session_get_local_settings(hive_session_t *session,
    hive_settings_t *settings_out);

/* Remote SETTINGS currently in effect (what peer advertised). */
int hive_session_get_remote_settings(hive_session_t *session,
    hive_settings_t *settings_out);

/*
 * Get the current state of stream_id.
 * Returns HIVE_STREAM_IDLE if stream_id has never been opened or is not found.
 *
 * Timing note: when a locally-sent END_STREAM closes a stream (e.g.
 * HALF_CLOSED_REMOTE → CLOSED in the send path), the stream state
 * transitions to CLOSED and on_stream_close fires in the same call that
 * queues the DATA/HEADERS frame — before the frame is transmitted. Callers
 * should not interpret on_stream_close as confirmation of peer receipt; it
 * means the stream is logically closed from the local perspective and the
 * close frame is queued. The stream slot is freed immediately; subsequent
 * calls to hive_stream_get_state() for that stream_id return HIVE_STREAM_IDLE.
 */
hive_stream_state_t hive_stream_get_state(hive_session_t *session,
    uint32_t stream_id);

/*
 * Set per-stream caller context pointer.
 * Returns HIVE_OK on success. Returns HIVE_ERR_STREAM_CLOSED if not found.
 */
int hive_stream_set_user_data(hive_session_t *session,
    uint32_t stream_id, void *user_data);

/*
 * Get per-stream caller context pointer.
 * Returns NULL if stream_id not found.
 */
void *hive_stream_get_user_data(hive_session_t *session,
    uint32_t stream_id);
```

### 9.12 HPACK Standalone API

HPACK encoder and decoder exposed independently for callers who need header
compression without a full session.

```c
/* Encoder */
int  hive_hpack_encoder_new(hive_hpack_encoder_t **enc, size_t max_table_size);
void hive_hpack_encoder_free(hive_hpack_encoder_t *enc);

/*
 * Encode nvlen header pairs into out.
 * outlen: on entry, capacity of out; on success, bytes written.
 * Returns HIVE_OK on success, HIVE_ERR_* on error.
 */
int  hive_hpack_encode(hive_hpack_encoder_t *enc,
         const hive_nv_t *nva, size_t nvlen,
         uint8_t *out, size_t *outlen);

/* Decoder */
int  hive_hpack_decoder_new(hive_hpack_decoder_t **dec, size_t max_table_size);
void hive_hpack_decoder_free(hive_hpack_decoder_t *dec);

/*
 * Decode one header field from in[0..inlen].
 * consumed: bytes read from in on return.
 * nv_out: populated on HIVE_HPACK_DECODE_EMIT.
 *
 * Returns HIVE_HPACK_DECODE_EMIT: one header decoded; call again.
 * Returns HIVE_HPACK_DECODE_DONE: input exhausted; block complete.
 * Returns HIVE_ERR_COMPRESSION:   fatal HPACK error.
 */
int  hive_hpack_decode(hive_hpack_decoder_t *dec,
         const uint8_t *in, size_t inlen, size_t *consumed,
         hive_nv_t *nv_out);
```

### 9.13 Error Codes

```c
/* Library return codes */
#define HIVE_OK                    0    /* success */
#define HIVE_ERR_NOMEM            -1   /* allocation failure */
#define HIVE_ERR_PROTOCOL         -2   /* HTTP/2 protocol error */
#define HIVE_ERR_INVALID_ARG      -3   /* bad argument to API function */
#define HIVE_ERR_STREAM_CLOSED    -4   /* stream_id not open */
#define HIVE_ERR_SESSION_CLOSED   -5   /* session in GOAWAY or CLOSED state */
#define HIVE_ERR_WOULDBLOCK       -6   /* flow-controlled; retry after WINDOW_UPDATE */
#define HIVE_ERR_COMPRESSION      -7   /* HPACK encoding/decoding error — fatal */
#define HIVE_ERR_FLOW_CONTROL     -8   /* flow control window overflow */
#define HIVE_ERR_FRAME_SIZE       -9   /* frame exceeds advertised max frame size */
#define HIVE_ERR_REFUSED_STREAM   -10  /* stream refused (push rejected, max concurrent) */
#define HIVE_ERR_CANCEL           -11  /* stream cancelled by RST_STREAM */
#define HIVE_ERR_SETTINGS_TIMEOUT -12  /* SETTINGS ACK not received within caller's timeout */
```

Note on `HIVE_ERR_SETTINGS_TIMEOUT`: Hive does not implement timeouts
internally (see TECH_STACK.md §4.2). This error code is reserved for
caller-driven timeout detection: if the caller determines that a SETTINGS ACK
has not been received within an acceptable period, it may call a library
function to signal the timeout condition and receive this error. The mechanism
for caller-driven timeout triggering will be specified in a future revision.

```c
/* HPACK standalone decoder return values (positive — not error codes) */
#define HIVE_HPACK_DECODE_EMIT     1   /* header decoded; call again */
#define HIVE_HPACK_DECODE_DONE     2   /* block complete */

/* HTTP/2 wire error codes (used in RST_STREAM and GOAWAY frames) */
/* Complete set per RFC 9113 §7 */
#define HIVE_H2_NO_ERROR            0x0
#define HIVE_H2_PROTOCOL_ERROR      0x1
#define HIVE_H2_INTERNAL_ERROR      0x2
#define HIVE_H2_FLOW_CONTROL_ERROR  0x3
#define HIVE_H2_SETTINGS_TIMEOUT    0x4
#define HIVE_H2_STREAM_CLOSED       0x5
#define HIVE_H2_FRAME_SIZE_ERROR    0x6
#define HIVE_H2_REFUSED_STREAM      0x7
#define HIVE_H2_CANCEL              0x8
#define HIVE_H2_COMPRESSION_ERROR   0x9
#define HIVE_H2_CONNECT_ERROR       0xa
#define HIVE_H2_ENHANCE_YOUR_CALM   0xb
#define HIVE_H2_INADEQUATE_SECURITY 0xc
#define HIVE_H2_HTTP_1_1_REQUIRED   0xd
```

`HIVE_OK` (value 0) is the library success return code. `HIVE_H2_NO_ERROR`
(value 0x0) is the HTTP/2 wire error code used in RST_STREAM and GOAWAY frames
to indicate a clean close. They share the same numeric value but are used in
different contexts.

---

## 10. Event Loop Walkthrough

A complete single iteration for a server receiving a GET request and
responding with a static file body.

**Setup**: max_concurrent=100, opt_max_send_iov=512, arena allocator in use.
Stream 1 is new. Previous SETTINGS exchange is complete.

```
Step 1 — epoll_wait fires; socket is readable

  n = tls_read(tls_ctx, read_buf, 4096);
  /* read_buf contains: [9 frame header bytes][N compressed header bytes] */

Step 2 — hive_session_recv(session, read_buf, n)

  recv_state = RECV_FRAME_HEADER
  · consume 9 bytes from read_buf → frame_hdr_buf
  · parse cur_frame: type=HEADERS, flags=END_HEADERS|END_STREAM,
                     stream_id=1, length=N
  · validate: inbound length N <= local_settings.max_frame_size ✓
  · validate: stream_id=1 is new, odd, > last_stream_id_remote (0)
  · validate: peer_stream_open_count (0) < opt_max_concurrent_streams (100)
  · check stream ID exhaustion: 1 < (0x7FFFFFFFu - 1000u) — no GOAWAY needed
  · pop slot 0 from stream_free_stack
  · initialise stream_slots[0]: stream_id=1, state=OPEN,
      send_window=65535, recv_window=65535, recv_consumed=0,
      content_length_expected=-1, content_length_received=0
  · insert {stream_id=1, slot_index=0} into stream_hash
  · last_stream_id_remote = 1; stream_open_count = 1; peer_stream_open_count = 1

  recv_state = RECV_HEADERS_PAYLOAD
  · copy N bytes from read_buf+9 → reassembly_buf[0..N-1]
  · reassembly_len = N; payload_remaining = 0
  · END_HEADERS flag set → hpack_decode_block(session, reassembly_buf, N)
    · fire on_begin_headers(session, 1, user_data)
    · decode :method GET  → on_header(session, 1, &name, &value, 0, user_data)
    · decode :path /index.html → on_header(...)
    · decode :scheme https → on_header(...)
    · decode host example.com → on_header(...)
    · decoded_size, decoded_count within limits ✓
    · presence check: has_method=1, has_scheme=1, has_path=1 ✓
    · fire on_headers_complete(session, 1, end_stream=1, user_data)
  · reassembly_len = 0; reassembly_active = 0
  · END_STREAM → stream 1 state = HALF_CLOSED_REMOTE
  · return n (all bytes consumed)

Step 3 — inside on_headers_complete, caller calls:
  hive_submit_response(session, 1, nva, nvlen, &data_source)

  · verify stream 1 is HALF_CLOSED_REMOTE ✓
  · encode_start headers using multi-iov approach (§6.4)
    · first_hdr_offset = 0; encode_start = 9
    · hpack_encode produces 12 bytes: [:status 200, content-type text/html]
    · encoded_len=12 <= remote_settings.max_frame_size — simple case
    · write HEADERS frame header at send_buf[0..8]:
        length=12, type=0x1, flags=0x04 (END_HEADERS), stream_id=1
    · send_iov[0] = {send_buf+0, 9+12=21}
    · send_buf_used = 21
  · store data_source in stream_slots[0].data_source

Step 4 — hive_session_recv() returns; event loop calls hive_session_send()

  · send_partial == 0: call send_queue_flush_data(session)
  · stream 1 has pending data_source
    · send_window check: session=65535, stream=65535 — flow control allows DATA
    · max_len = min(16384, 65535, 65535) = 16384
    · hdr_offset = 21; send_buf_used → 30
    · send_iov[1] = {send_buf+21, 9}   (DATA frame header)
    · data_source.read_callback(session, 1, &body_ptr, 16384, &flags, &src, ud)
      → callback redirects body_ptr = mmap_ptr, bytes_written=8192, flags=NO_COPY|EOF
    · send_iov[2] = {mmap_ptr, 8192}   (zero-copy body)
    · write DATA frame header at send_buf[21..29]:
        length=8192, type=0x0, flags=0x01 (END_STREAM), stream_id=1
    · stream was HALF_CLOSED_REMOTE + send END_STREAM → state = CLOSED
    · session->send_window -= 8192; stream->send_window -= 8192  /* before slot freed */
    · fire on_stream_close(session, 1, HIVE_H2_NO_ERROR, user_data)  /* queue time */
    · stream_close(session, stream)  /* slot freed immediately */
    · send_iov_count = 3

  · fire send(session, send_iov, 3, user_data)
      iov[0] = {send_buf+0,  21}     HEADERS frame (header+payload, contiguous)
      iov[1] = {send_buf+21,  9}     DATA frame header
      iov[2] = {mmap_ptr,  8192}     file body — caller memory, zero copy

  · caller: tls_write(tls_ctx, iov, 3)
    → returns 8230 (all bytes written)

  · send_partial_offset = 8230 >= total(8230): full send
  · send_iov_count = 0; send_buf_used = 0; send_partial = 0

Step 5 — return to event loop
  hive_session_want_write(session) = 0  (queue empty, no partial)
  hive_session_want_read(session)  = 1  (session still open)
```

---

## 11. Memory Budget

Default options, `opt_max_concurrent_streams = 100`.

| Component | Formula | Bytes |
|---|---|---|
| `hive_session_t` struct | measured (increased for new fields) | ~500 |
| `stream_hash` | 256 × 8 | 2,048 |
| `stream_slots` | 100 × 64 | 6,400 |
| `stream_free_stack` | 100 × 4 | 400 |
| `send_iov` | 512 × 16 | 8,192 |
| `send_buf` | 65536 + 36 + 2048 | 67,620 |
| `reassembly_buf` | opt_max_continuation_size | 65,536 |
| `hpack_scratch_name` | opt_max_header_string_size | 8,192 |
| `hpack_scratch_value` | opt_max_header_string_size | 8,192 |
| `enc_table.ring` | 128 × 8 (4096/32 entries) | 1,024 |
| `dec_table.ring` | 128 × 8 | 1,024 |
| `pending_settings` | 3 × 24 (outbound ring) | 72 |
| **Total** | | **~168 KB** |

Dominated by `send_buf` and `reassembly_buf`. Both are configurable.

Setting `opt_max_continuation_size = 16384` (one max-frame-size) reduces
per-session footprint to ~103 KB. At 1000 concurrent HTTP/2 connections in
Wraith with arena allocators: ~103 MB — well within budget for a production
server.

With a Wraith connection-scoped arena: one `malloc(~168 KB)` at connection
accept, zero structural allocation during the connection lifetime
(HPACK entries and retained headers use the same arena as free-list pops),
one `free()` at connection close.

**See Also**: PROJECT.md, TECH_STACK.md, CODING_STANDARDS.md, DEVELOPMENT.md, TESTING.md
