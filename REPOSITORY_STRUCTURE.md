# Repository Structure

## 1. Top-Level Layout

```
hive/
├── Makefile                    # Build orchestration: dev, release, test, lint, install
├── .clang-format               # Code formatting rules (KNF-based)
├── .clang-tidy                 # Static analysis configuration
├── README.md                   # Embedder-facing introduction and quickstart
├── PROJECT.md                  # Overview, goals, scope, security model
├── ARCHITECTURE.md             # Internal design, memory layout, state machines, API
├── TECH_STACK.md               # Build system, compiler flags, tools
├── CODING_STANDARDS.md         # C style, allocator discipline, security patterns
├── REPOSITORY_STRUCTURE.md     # This file
├── DEVELOPMENT.md              # Phased build plan, milestones, task breakdown
├── TESTING.md                  # Test strategy, conformance suite (h2spec)
├── include/                    # Public header
├── src/                        # Library source files
├── tests/                      # Unit tests and conformance helpers
└── tools/                      # Developer tools (frame decoder)
```

The build output is `libhive.a`. No binary is installed. `make install`
installs two files: `libhive.a` into `$(LIBDIR)` and `include/hive.h`
into `$(INCLUDEDIR)`.

---

## 2. include/ - Public Header

```
include/
│
└── hive.h              # The only header an embedder includes.
                        # Self-contained: pulls in only <stddef.h>,
                        #   <stdint.h>, <sys/uio.h>.
                        # Declares all public types, constants, and functions.
                        # Does NOT expose any internal types or struct layouts.
                        # See ARCHITECTURE.md §9 for full API documentation.
                        # See README.md for a quickstart integration example.
```

`hive.h` is the entire public surface of the library. An embedder adds
`-I/usr/local/include` (or wherever it is installed) and writes
`#include <hive.h>`. Nothing else is needed.

---

## 3. src/ - Library Source Files

All library source lives here. No subdirectories. Each module is a `.c` file
with a corresponding internal `.h` file. The public API is in `include/hive.h`
only - internal headers are never installed and never included by embedders.

```
src/
│
│   - Session and public API -
│
├── hive.c              # Session lifecycle and public API entry points.
│                       # hive_session_server_new(), hive_session_client_new(),
│                       #   hive_session_server_upgrade(),
│                       #   hive_session_feed_upgrade_headers(),
│                       #   hive_session_free().
│                       # session_prealloc(): allocates all sub-buffers from the
│                       #   session allocator using goto cleanup. See ARCHITECTURE.md §2.
│                       # hive_session_recv(): entry point - drives hive_frame.c
│                       #   receive state machine.
│                       # hive_session_send(): fires send callback with batched
│                       #   iovec; returns int (HIVE_OK or HIVE_ERR_*); retains
│                       #   unsent tail via send_partial_offset for partial writes.
│                       #   (Note: the send *callback* returns ssize_t bytes written;
│                       #   hive_session_send() itself returns int.)
│                       #   Calls send_queue_flush_data() first.
│                       # hive_buf_retain(), hive_buf_free().
│                       # hive_session_want_read(), hive_session_want_write().
│                       # Submit functions: hive_submit_response(),
│                       #   hive_submit_trailers(), hive_submit_interim_response(),
│                       #   hive_submit_push_promise(), hive_submit_rst_stream(),
│                       #   hive_submit_goaway_prepare(), hive_submit_goaway_final(),
│                       #   hive_submit_ping(), hive_submit_ping_ack().
│                       # hive_submit_request() (client role).
│                       # Introspection: hive_session_get_remote_window_size(),
│                       #   hive_session_get_local_settings(),
│                       #   hive_session_get_remote_settings(),
│                       #   hive_stream_get_state(),
│                       #   hive_stream_set_user_data(),
│                       #   hive_stream_get_user_data().
│                       # NULL-allocator shim: null_alloc_malloc/free/calloc/realloc.
│                       #   This is the ONLY file in src/ that may call malloc/free
│                       #   directly. All other src/ files use s->mem.*.
│                       # See ARCHITECTURE.md §1, §9.7–§9.11.
│                       # See CODING_STANDARDS.md §2.1 for the allocator rule.
│
├── hive_internal.h     # Internal shared types and forward declarations.
│                       # hive_session_t complete struct definition (all 10 regions).
│                       #   Includes all v1.1 fields: preface_count, tombstone_count,
│                       #   closes_since_compact, inbound_settings_count,
│                       #   goaway_last_stream_id_sent/recv, goaway_prepare_sent,
│                       #   send_partial, send_partial_offset.
│                       #   v1.2 additions: reassembly_active, priority_payload_len,
│                       #   reassembly_end_stream (Region G); peer_stream_open_count
│                       #   (Region I); content_length_expected, content_length_received
│                       #   (hive_stream_t). hive_stream_t is now 64 bytes.
│                       #   v1.5 additions: fc_accounted (flow-control once-per-frame
│                       #   flag), pad_length_received (PADDED frame split-delivery
│                       #   flag), reassembly_promised_stream_id (fragmented
│                       #   PUSH_PROMISE promised stream ID), reassembly_type
│                       #   (0=HEADERS, 1=PUSH_PROMISE - controls CONTINUATION
│                       #   completion path), goaway_error_code_recv (Region C).
│                       #   v1.8/v1.9 additions: pad_validated (deferred padding
│                       #   check flag - validated after fixed-prefix fields consumed);
│                       #   pending_min in hpack_table_t (lowest HEADER_TABLE_SIZE
│                       #   reached since last encode - required for RFC 7541 §6.3
│                       #   two-update sequence).
│                       #   v1.13/v1.14 additions: error_stream_id parameter
│                       #   added to hpack_decode_block() - for HEADERS errors
│                       #   target reassembly_stream_id; for PUSH_PROMISE errors
│                       #   target reassembly_promised_stream_id.
│                       # hpack_table_t, hpack_entry_t, hpack_hash_slot_t.
│                       # stream_hash_entry_t, hive_stream_t.
│                       # frame_hdr_t. hive_recv_state_t. hive_session_state_t.
│                       # hive_stream_state_t.
│                       # All _Static_assert size checks (CODING_STANDARDS.md §1.3).
│                       # session_error() and stream_error() static helpers.
│                       #   session_error() fires on_connection_error before GOAWAY.
│                       # HPACK_ENTRY_NAME / HPACK_ENTRY_VALUE / HPACK_ENTRY_RFC_SIZE
│                       #   accessor macros.
│                       # HIVE_ASAN_POISON / HIVE_ASAN_UNPOISON macros.
│                       # HIVE_DEBUG build guards.
│                       # NOT included by embedders - internal only.
│                       # See ARCHITECTURE.md §2.
│
│   - Frame parser, serialiser, and standalone header parser -
│
├── hive_frame_bare.h   # Standalone frame header parser interface.
│                       # No dependency on hive_session_t, callbacks, or stream state.
│                       # Used by both libhive.a (via hive_frame.c) and the
│                       #   tools/hive_decode binary.
│                       # frame_hdr_write_at(): write a 9-byte frame header at
│                       #   an arbitrary buffer offset (type, flags, stream_id,
│                       #   length in network byte order).
│                       # frame_hdr_parse(): parse 9 bytes into a frame_hdr_t.
│
├── hive_frame_bare.c   # Standalone frame header serialisation and parsing.
│                       # Implements frame_hdr_write_at() and frame_hdr_parse().
│                       # Zero session coupling - safe to link into any binary.
│                       # See ARCHITECTURE.md §6.2.
│                       # See TECH_STACK.md §7.8 (frame decoder tool usage).
│
├── hive_frame.h        # Internal frame parser interface.
│                       # Includes hive_frame_bare.h.
│                       # Frame type constants: HIVE_FRAME_DATA (0x0) through
│                       #   HIVE_FRAME_CONTINUATION (0x9).
│                       # Frame flag constants: HIVE_FLAG_END_STREAM,
│                       #   HIVE_FLAG_END_HEADERS, HIVE_FLAG_PADDED,
│                       #   HIVE_FLAG_ACK, HIVE_FLAG_PRIORITY.
│                       # frame_recv_init(), frame_recv_process() declarations.
│
├── hive_frame.c        # Incremental frame parser (receive path).
│                       # All 18 hive_recv_state_t states (ARCHITECTURE.md §3.1),
│                       #   including RECV_SERVER_PREFACE (client role: first frame
│                       #   from server must be SETTINGS non-ACK),
│                       #   RECV_HEADERS_PAD and RECV_PUSH_PROMISE_PAD (consume
│                       #   padding after header payloads), and RECV_GOAWAY_DEBUG
│                       #   (collect optional GOAWAY debug data before on_goaway).
│                       # frame_hdr_buf[9] / frame_hdr_count staging:
│                       #   accumulates 9 bytes across any number of recv calls
│                       #   before parsing cur_frame.
│                       # preface_count: counts bytes of client or server preface;
│                       #   also used as flag (value 1) for first-frame validation.
│                       # ctrl_staging[8] / ctrl_staging_count:
│                       #   accumulates PING (8), GOAWAY header (8),
│                       #   SETTINGS params (6), RST_STREAM/WINDOW_UPDATE (4),
│                       #   PRIORITY (5) bytes.
│                       # reassembly_active: CONTINUATION lockout flag. Set when
│                       #   HEADERS/PUSH_PROMISE without END_HEADERS received;
│                       #   cleared on final CONTINUATION with END_HEADERS.
│                       #   Used in preference to reassembly_len to correctly
│                       #   handle zero-length compressed header blocks.
│                       # reassembly_type: distinguishes HEADERS (0) from
│                       #   PUSH_PROMISE (1) reassembly. Controls CONTINUATION
│                       #   END_HEADERS completion: HEADERS fires on_headers_complete;
│                       #   PUSH_PROMISE fires on_push_promise with
│                       #   reassembly_promised_stream_id.
│                       # reassembly_stream_id: set before every call to
│                       #   hpack_decode_block(), including single-frame (unfragmented)
│                       #   HEADERS and PUSH_PROMISE paths, so callbacks always fire
│                       #   with the correct stream ID.
│                       # fc_accounted: ensures flow-control window decrement against
│                       #   cur_frame.length happens exactly once per DATA frame
│                       #   regardless of split delivery across multiple recv calls.
│                       # pad_validated: deferred padding check flag. pad_length is
│                       #   extracted as the first payload byte but validated only after
│                       #   all fixed-prefix fields (PRIORITY prefix in HEADERS, promised
│                       #   stream ID in PUSH_PROMISE) are consumed. Reset at each
│                       #   RECV_FRAME_HEADER entry.
│                       # Idle/closed stream classification is parity-aware for DATA,
│                       #   RST_STREAM, and WINDOW_UPDATE: uses last_stream_id_local
│                       #   for locally-initiated streams and last_stream_id_remote for
│                       #   peer-initiated, determined by stream ID parity and role.
│                       # content_length_received: incremented by application data
│                       #   bytes (excluding padding) in RECV_DATA_PAYLOAD; checked
│                       #   against content_length_expected on END_STREAM in both
│                       #   DATA frames and HEADERS+END_STREAM (no DATA) messages.
│                       # RESERVED_REMOTE state transitions: HEADERS received on a
│                       #   reserved(remote) stream (push response) transitions to
│                       #   half-closed(local); with END_STREAM transitions to closed.
│                       #   Handled in both unfragmented (RECV_HEADERS_PAYLOAD) and
│                       #   fragmented (RECV_CONTINUATION_PAYLOAD) paths.
│                       # DATA on reserved stream (RESERVED_LOCAL or RESERVED_REMOTE):
│                       #   connection error PROTOCOL_ERROR, not RST_STREAM STREAM_CLOSED.
│                       # GOAWAY receive closes only locally-initiated streams above
│                       #   last_stream_id (REFUSED_STREAM); peer-initiated streams
│                       #   above the limit do not exist in the stream table.
│                       # reassembly_stream_error_code: non-zero when a HEADERS
│                       #   block arrives on an illegal-state stream. Full reassembly
│                       #   and decode proceed with callbacks suppressed (for HPACK
│                       #   table synchronization per RFC 7541 §2.3.2). RST_STREAM
│                       #   and stream_close fire on END_HEADERS. Cleared after use.
│                       # RST_STREAM error paths: all send RST_STREAM paths now call
│                       #   on_stream_close and stream_close before queuing the RST -
│                       #   RST_STREAM is terminal for the stream per RFC 9113 §6.4.
│                       # WINDOW_UPDATE zero-increment: returns explicitly after error;
│                       #   does not fall through to normal window update logic.
│                       # RST_STREAM flood counter updated before stream lookup so
│                       #   recently-closed stream RSTs are counted toward rate limit.
│                       # HEADERS + CONTINUATION + PUSH_PROMISE reassembly
│                       #   into reassembly_buf.
│                       # DATA delivery: on_data_chunk fires with pointer
│                       #   directly into caller's buffer - zero copy.
│                       #   Receive-side flow control enforced before delivery:
│                       #   FLOW_CONTROL_ERROR on stream or connection window exceeded.
│                       # CONTINUATION lockout: connection error (GOAWAY) per
│                       #   RFC 9113 §4.3 - NOT RST_STREAM.
│                       #   Checked in RECV_FRAME_HEADER before any other processing.
│                       #   See ARCHITECTURE.md §3.5.
│                       # Fixed-length frame validation: SETTINGS-ACK=0, PING=8,
│                       #   RST_STREAM=4, WINDOW_UPDATE=4, PRIORITY=5, GOAWAY>=8;
│                       #   SETTINGS non-ACK: length must be multiple of 6;
│                       #   PUSH_PROMISE: length >= 4 (or >= 5 if PADDED);
│                       #   PRIORITY stream_id=0: PROTOCOL_ERROR connection error.
│                       # Inbound frame length validated against
│                       #   local_settings.max_frame_size (what we advertised).
│                       # Unknown frame types: RECV_SKIP_PAYLOAD.
│                       # PUSH_PROMISE: promised stream opening checks
│                       #   peer_stream_open_count and stream_open_count against
│                       #   opt_max_concurrent_streams before opening slot;
│                       #   exceeded limits → RST_STREAM REFUSED_STREAM on
│                       #   promised stream (not carrying stream).
│                       # HPACK decode errors on PUSH_PROMISE target the promised
│                       #   stream via error_stream_id parameter - carrying stream
│                       #   is unaffected per RFC 9113 §6.6.
│                       # SETTINGS flood: increments inbound_settings_count
│                       #   (separate from the outbound pending_settings ring).
│                       # New callbacks fired: on_settings_ack, on_goaway,
│                       #   on_ping, on_ping_ack.
│                       # See ARCHITECTURE.md §3, §6.2.
│                       # See CODING_STANDARDS.md §3.1 (SETTINGS directionality).
│
│   - HPACK -
│
├── hive_hpack.h        # Internal HPACK interface.
│                       # hpack_table_init(), hpack_table_free().
│                       # hpack_decode_block(): decode a full HEADERS block,
│                       #   fire on_header callbacks (by pointer), enforce
│                       #   bomb limits, enforce size-update position rule.
│                       # hpack_encode_block(): encode header list into send_buf.
│                       # hpack_decode_int() - returns HPACK_INT_OVERFLOW on error.
│                       # hpack_encode_int().
│                       # Standalone API glue: hive_hpack_encoder_t,
│                       #   hive_hpack_decoder_t (thin wrappers around
│                       #   hpack_table_t with their own allocator).
│
├── hive_hpack.c        # Full RFC 7541 HPACK implementation.
│                       # Static table: compile-time array of 61 hive_nv_t
│                       #   entries (RFC 7541 Appendix A). No allocation.
│                       # Huffman decode table: 256-entry huff_entry_t array
│                       #   (RFC 7541 Appendix B). 1KB, L1 resident.
│                       #   Iterative bit-accumulator loop handles codes up
│                       #   to 30 bits. See ARCHITECTURE.md §4.4.
│                       # Huffman encode table: 257-entry code+length array.
│                       # Dynamic table: ring buffer of hpack_entry_t* pointers.
│                       #   Each entry is a single allocation:
│                       #   [hpack_entry_t header | name bytes | value bytes].
│                       #   SECURITY: every insertion copies into arena memory -
│                       #   never stores a pointer into the caller's buffer.
│                       #   Oversized entry (rfc_size > max_size): evict table
│                       #   fully but do not insert. See ARCHITECTURE.md §4.2.
│                       #   See ARCHITECTURE.md §8.1.
│                       # Encoder pending size updates: pending_min and pending_max
│                       #   track the minimum and final HEADER_TABLE_SIZE reached
│                       #   since the last encode. On next encode, if pending_min
│                       #   != pending_max, emit pending_min first then pending_max
│                       #   (RFC 7541 §6.3 two-update requirement). Eviction applied
│                       #   to pending_min first, then pending_max, to stay
│                       #   synchronized with the decoder's table state.
│                       # hpack_decode_block(): Added error_stream_id parameter.
│                       #   For HEADERS blocks: error_stream_id == reassembly_stream_id.
│                       #   For PUSH_PROMISE blocks: error_stream_id ==
│                       #   reassembly_promised_stream_id - HPACK decode errors on
│                       #   PUSH_PROMISE target the promised stream; the carrying
│                       #   stream is unaffected per RFC 9113 §6.6.
│                       #   RECV_CONTINUATION_PAYLOAD derives err_target from
│                       #   reassembly_type (0=HEADERS, 1=PUSH_PROMISE).
│                       #   index-0 rejection; out-of-range index rejection;
│                       #   HPACK bomb limits enforced incrementally as stream
│                       #   errors before each on_header callback.
│                       #   on_header called with hive_buf_t *name, hive_buf_t *value
│                       #   (by pointer). HIVE_BUF_VALID cleared after callback.
│                       #   Callback return values checked: HIVE_ERR_COMPRESSION
│                       #   from any callback → immediate connection error;
│                       #   other non-OK → stream_error_pending, continue decoding.
│                       #   In HIVE_DEBUG: ASan-poison reassembly_buf,
│                       #   hpack_scratch_name, hpack_scratch_value after callback
│                       #   (not static/dynamic table data).
│                       #   See ARCHITECTURE.md §4.5, §8.8.
│                       # String decode: Huffman path decodes into hpack_scratch_name
│                       #   (for names) or hpack_scratch_value (for values) - two
│                       #   separate scratch buffers so both decoded strings are
│                       #   simultaneously valid when on_header fires;
│                       #   literal path is a direct pointer into reassembly_buf.
│                       #   Truncated string or string-too-long: HIVE_ERR_COMPRESSION
│                       #   (connection error COMPRESSION_ERROR) - not PROTOCOL_ERROR.
│                       #   See ARCHITECTURE.md §4.6.
│                       # Integer varint: returns HPACK_INT_OVERFLOW on truncation
│                       #   or overflow. Callers check before use.
│                       #   See ARCHITECTURE.md §4.7.
│                       # Hash index activated when max_size > 16384.
│                       #   See ARCHITECTURE.md §4.9.
│                       # See ARCHITECTURE.md §4.
│                       # See CODING_STANDARDS.md §3.2 (SECURITY comments).
│
│   - Stream table -
│
├── hive_stream.h       # Internal stream table interface.
│                       # stream_open(), stream_lookup(), stream_close().
│                       #   stream_close() increments both tombstone_count
│                       #   and closes_since_compact.
│                       # stream_free_pop(), stream_free_push().
│                       # stream_hash_compact().
│
├── hive_stream.c       # Two-layer stream state store.
│                       # Layer 1: stream_hash (open-addressed hash table).
│                       #   stream_hash_entry_t: 8 bytes each.
│                       #   Knuth multiplicative hash: (id * 2654435761u) >> shift.
│                       #   Linear probing. Sentinels: EMPTY=0xFFFFFFFF,
│                       #   TOMBSTONE=0xFFFFFFFE.
│                       # Layer 2: stream_slots (hive_stream_t array, 64 bytes each).
│                       # Free stack: stream_free_stack (uint32_t array).
│                       #   O(1) slot alloc and free - no allocation per stream.
│                       # Compaction: when tombstone ratio > 25% and closes_since_compact
│                       #   >= 64. tombstone_count and closes_since_compact reset on
│                       #   compaction. See ARCHITECTURE.md §5.6.
│                       # See ARCHITECTURE.md §5.
│
│   - Flow control -
│
├── hive_flow.h         # Internal flow control interface.
│                       # flow_recv_data(): enforce recv_window, update
│                       #   recv_consumed, coalesce WINDOW_UPDATE, restore
│                       #   recv_window when WINDOW_UPDATE is queued.
│                       #   Called from RECV_DATA_PAYLOAD.
│                       # flow_apply_window_update(): apply received
│                       #   WINDOW_UPDATE to send_window; validate overflow.
│                       # flow_adjust_streams(): retroactively adjust all
│                       #   stream send windows after SETTINGS_INITIAL_WINDOW_SIZE
│                       #   change.
│
├── hive_flow.c         # Connection and stream flow control.
│                       # Receive-side enforcement: stream->recv_window and
│                       #   session->recv_window decremented on DATA receipt;
│                       #   FLOW_CONTROL_ERROR (stream) or GOAWAY (connection)
│                       #   if peer exceeds window.
│                       # recv_consumed tracking per stream and per connection.
│                       # WINDOW_UPDATE coalescing: queue only when
│                       #   recv_consumed > recv_window / 2.
│                       #   recv_window restored by the update increment on send.
│                       # send_window enforcement: DATA frames skipped when
│                       #   session->send_window == 0 or stream->send_window == 0.
│                       # Outbound DATA capped at remote_settings.max_frame_size
│                       #   (peer's advertised limit).
│                       # Retroactive stream window adjustment on
│                       #   SETTINGS_INITIAL_WINDOW_SIZE change.
│                       # See ARCHITECTURE.md §7.7, §8.7.
│
│   - Send queue -
│
├── hive_send.h         # Internal send queue interface.
│                       # send_queue_append_ctrl(): queue control frame.
│                       # send_queue_append_headers(): queue HEADERS frame,
│                       #   split into CONTINUATION if needed.
│                       # send_queue_flush_data(): drive data_sources within
│                       #   flow control limits.
│
├── hive_send.c         # Scatter-gather iovec accumulation and drain.
│                       # frame_hdr_write(): write 9-byte header into send_buf
│                       #   at send_buf_used and advance. Wrapper around
│                       #   frame_hdr_write_at() from hive_frame_bare.c.
│                       # Control frames (SETTINGS, SETTINGS ACK, PING, PING ACK,
│                       #   RST_STREAM, WINDOW_UPDATE, GOAWAY): frame header
│                       #   + payload contiguous in send_buf, one iovec entry.
│                       # HEADERS frames: no byte-shifting algorithm -
│                       #   encoded payload placed contiguously at encode_start,
│                       #   frame headers written after it, iovecs interleave
│                       #   headers and payload chunks. Outbound frame sizing
│                       #   uses remote_settings.max_frame_size.
│                       # DATA frames: callback receives uint8_t **buf
│                       #   (pointer to pointer). Copy path: library buffer.
│                       #   NO_COPY path: callback redirects *buf to caller memory;
│                       #   iovec points directly to caller's memory.
│                       #   END_STREAM flag included in frame_hdr_write_at() call
│                       #   when HIVE_DATA_FLAG_EOF is set - not written retroactively.
│                       #   State transition on EOF: OPEN→HALF_CLOSED_LOCAL,
│                       #   HALF_CLOSED_REMOTE→CLOSED; on_stream_close fires at
│                       #   queue time (before transmission).
│                       #   Send windows decremented BEFORE stream_close() to avoid
│                       #   use-after-free of freed slot.
│                       #   0-byte/no-EOF read_callback return: trailer handoff -
│                       #   reserved iov and header space undone, data_source.read_callback
│                       #   cleared to NULL (embedded struct, not pointer), returns
│                       #   without queuing DATA frame; caller then calls
│                       #   hive_submit_trailers().
│                       # eff_iov in hive_session_send: sized to HIVE_SEND_IOV_MAX;
│                       #   opt_max_send_iov must not exceed HIVE_SEND_IOV_MAX
│                       #   (enforced by hive_options_set_max_send_iov).
│                       # Partial send: send_partial_offset tracks bytes already
│                       #   sent from current batch; effective iov built by
│                       #   skipping send_partial_offset bytes on next call.
│                       # iovec overflow: internal flush if send_iov_count
│                       #   would exceed opt_max_send_iov.
│                       # See ARCHITECTURE.md §6.
│
│   - Security -
│
├── hive_security.h     # Internal security module interface.
│                       # security_check_rst_flood(): update rate counter using
│                       #   clock abstraction, fire on_rst_stream_flood if
│                       #   threshold exceeded.
│                       # security_check_stream_exhaustion(): compare stream_id
│                       #   against 2^31-1000 threshold; call
│                       #   hive_submit_goaway_prepare() if exceeded.
│
├── hive_security.c     # RST_STREAM flood detection and stream ID exhaustion.
│                       # RST_STREAM flood: rolling rate counter using
│                       #   hive_monotonic_secs() clock abstraction (see
│                       #   hive_clock.h). Window resets on expiry.
│                       #   Fires on_rst_stream_flood - library does not
│                       #   take unilateral action.
│                       # Stream ID exhaustion: triggers prepare-phase GOAWAY
│                       #   (last_stream_id=0x7FFFFFFF) when stream_id exceeds
│                       #   0x7FFFFFFFu - 1000u. Allows in-flight streams to
│                       #   drain before final shutdown.
│                       # Note: HPACK bomb (§8.2) is enforced in hive_hpack.c.
│                       #   CONTINUATION flood (§8.3) is enforced in hive_frame.c
│                       #   as a connection error (GOAWAY), not RST_STREAM.
│                       #   SETTINGS flood (§8.4) is enforced in hive_frame.c
│                       #   via inbound_settings_count.
│                       #   Receive-side flow control (§8.7) enforced in hive_flow.c.
│                       #   hive_security.c owns only RST_STREAM flood and
│                       #   stream exhaustion.
│                       # See ARCHITECTURE.md §8.5, §8.6.
│
│   - Clock abstraction -
│
├── hive_clock.h        # Monotonic clock abstraction.
│                       # hive_monotonic_secs(): returns uint64_t seconds
│                       #   via CLOCK_MONOTONIC on production builds.
│                       # In test builds (HIVE_TEST_CLOCK=1 compile flag):
│                       #   reads from the global hive_test_clock_secs
│                       #   (uint64_t) which tests can advance directly.
│                       #   This eliminates timing dependencies from
│                       #   RST_STREAM flood tests.
│                       # Used only by hive_security.c for rate window tracking.
│                       # See ARCHITECTURE.md §8.5.
│                       # See DEVELOPMENT.md Phase 7 §7.3.
│
│   - Platform compat (Linux only) -
│
├── compat_str.h        # strlcpy and strlcat declarations for Linux.
│                       # On OpenBSD: not compiled (libc provides them).
│                       # On Linux: compiled in, no link flags needed.
│                       # See TECH_STACK.md §4.3.
│
└── compat_str.c        # Portable strlcpy() and strlcat() implementations.
                        # Compiled on Linux only via $(COMPAT_SRC) in Makefile.
                        # Not compiled on OpenBSD - libc versions used instead.
```

---

## 4. tests/ - Unit Tests and Conformance Helpers

All test source lives here. Tests link against `libhive.a`, not directly
against source files. This verifies that the library's exported symbols
are complete and correct.

```
tests/
│
│   - Test harness -
│
├── test_harness.h      # RUN() macro, tests_run / tests_passed counters.
│                       # Contract: test functions return 1 on pass, 0 on fail.
│                       # See TECH_STACK.md §6.1.
│
├── run_tests.c         # Test binary entry point. Calls RUN() for every
│                       #   test case across all test files. Returns 0 if
│                       #   all pass, 1 if any fail.
│
├── run_tests.sh        # Shell runner. Executes run_tests binary, fails
│                       #   on non-zero exit. Invoked by `make test`.
│
│   - Unit tests by module -
│
├── test_compat.c       # Platform compat layer tests.
│                       # strlcpy: basic copy, truncation, empty source.
│                       # strlcat: basic append, full dst, overflow guard.
│                       # See DEVELOPMENT.md Phase 1 §1.3.
│
├── test_hpack.c        # HPACK encoder and decoder tests.
│                       # Static table: size, index 1, index 2, index 61.
│                       # Huffman: empty, known vectors, round-trip, EOS rejection,
│                       #   invalid padding rejection.
│                       # Dynamic table: insert, evict, RFC size accounting,
│                       #   oversized entry eviction (table empties, no insert).
│                       # Always-copy: insert, overwrite original, verify correct.
│                       # Integer varint: 1-byte, multi-byte, truncated (overflow),
│                       #   RFC §C.1 example.
│                       # String decode: literal, Huffman, scratch limit,
│                       #   truncated string rejection.
│                       # Negative cases: index-0, out-of-range, size-update
│                       #   after first header field, size-update > pending_max.
│                       # Full decode: RFC 7541 §C.3, §C.4, §C.6 test vectors.
│                       # HPACK bomb: both limits as stream errors
│                       #   (session continues after RST_STREAM).
│                       # on_header by-pointer: HIVE_BUF_VALID cleared after
│                       #   callback in the library's handle objects.
│                       # Standalone API: encoder/decoder new/free/encode/decode.
│                       # See TESTING.md §6 key test cases.
│
├── test_frame.c        # Frame parser, serialiser, and bare header parser tests.
│                       # Frame header serialisation: DATA, SETTINGS, HEADERS.
│                       # hive_frame_bare: standalone parse/write with no session.
│                       # Receive coverage: all 10 frame types + unknown type.
│                       # Split delivery: 1-byte feed for all frame types.
│                       # CONTINUATION lockout: connection error (GOAWAY),
│                       #   not RST_STREAM - verified explicitly.
│                       # Fixed-length frame matrix: SETTINGS-ACK=0, PING=8,
│                       #   RST_STREAM=4, WINDOW_UPDATE=4, PRIORITY=5, GOAWAY>=8.
│                       # Inbound length validated against local_settings
│                       #   (not remote_settings).
│                       # Validation: DATA on stream 0, SETTINGS non-zero stream,
│                       #   stream ID parity, bad SETTINGS length.
│                       # See TESTING.md §2.2 (split delivery), §5.2.
│
├── test_stream.c       # Stream table tests.
│                       # open / lookup / close round-trip.
│                       # Hash collision handling.
│                       # Free stack: open max_concurrent, close one, reuse.
│                       # Compaction: tombstone_count and closes_since_compact
│                       #   tracked correctly; triggers at correct ratio.
│                       # Stream ID monotonicity violation.
│
├── test_flow.c         # Flow control tests.
│                       # WINDOW_UPDATE: connection and stream, overflow, zero.
│                       # Receive-side enforcement: stream window violation
│                       #   (RST_STREAM, session continues); connection window
│                       #   violation (GOAWAY, session closes).
│                       # DATA zero-copy: pointer in caller's buffer.
│                       # DATA partial delivery: two chunks, total bytes correct.
│                       # WINDOW_UPDATE coalescing: recv_window correctly restored.
│                       # Send window enforcement: DATA blocked at window=0.
│                       # Outbound frame capped at remote_settings.max_frame_size.
│                       # See TESTING.md §5.5 (receive-side flow control tests).
│
├── test_session.c      # Session lifecycle and end-to-end tests.
│                       # Session create/free: NULL allocator and custom allocator.
│                       # Tracking allocator: zero outstanding allocations after free.
│                       # Allocation failure: zero leaks when create fails mid-alloc.
│                       # Partial send: resume on next hive_session_send() call.
│                       # SETTINGS exchange: apply, ACK, retroactive window adjust.
│                       #   SETTINGS_HEADER_TABLE_SIZE constrains encoder (not decoder).
│                       # SETTINGS directionality: inbound uses local_settings;
│                       #   outbound respects remote_settings.
│                       # Connection preface: client and server, valid and invalid.
│                       #   Client role: first server frame must be SETTINGS non-ACK.
│                       # Full GET request-response round-trip.
│                       # send callback fires once per hive_session_send() call.
│                       # NO_COPY: *buf redirect points to caller memory.
│                       # All five new callbacks verified: on_settings_ack,
│                       #   on_goaway, on_ping, on_ping_ack, on_connection_error.
│                       # Two-phase GOAWAY: prepare then final.
│                       # Client role: stream ID assignment (1, 3, 5);
│                       #   MAX_CONCURRENT_STREAMS enforcement.
│                       # Stream introspection: get_state(), user_data set/get.
│                       # h2c Upgrade: feed_upgrade_headers fires callbacks.
│                       # Server push: PUSH_PROMISE, reserved stream, push response.
│                       # GOAWAY recv: want_read() advisory, no crash if caller
│                       #   continues reading.
│
├── test_security.c     # Security limit and flood protection tests.
│                       # CONTINUATION flood: GOAWAY (connection error) before
│                       #   HPACK decode - explicitly NOT RST_STREAM.
│                       # PUSH_PROMISE flood: same connection error path.
│                       # SETTINGS flood: inbound_settings_count counter (separate
│                       #   from the outbound pending_settings ring).
│                       # Unsolicited SETTINGS ACK: GOAWAY PROTOCOL_ERROR.
│                       # RST_STREAM flood: callback at threshold using clock mock
│                       #   (hive_test_clock_secs - no sleep() needed).
│                       # RST_STREAM window reset: advance clock mock, verify reset.
│                       # Stream ID exhaustion: prepare-phase GOAWAY
│                       #   (last_stream_id=0x7FFFFFFF, session stays OPEN).
│                       # HTTP messaging: pseudo-header order, duplicate pseudo,
│                       #   uppercase field names, forbidden Connection header,
│                       #   invalid TE value, Content-Length mismatch.
│                       # Malformed frames: oversized (uses local_settings),
│                       #   fixed-length matrix (all 6 frame types), bad padding,
│                       #   HPACK negative cases.
│                       # SETTINGS directionality: inbound vs outbound checks.
│                       # hive_buf_t HIVE_BUF_VALID cleared after callback
│                       #   (automated, all builds).
│                       # Callback error returns: stream error, session continues.
│                       # ASan poisoning of reassembly_buf/hpack_scratch data
│                       #   verified manually in HIVE_DEBUG build (not static table).
│                       # See TESTING.md §5.
│
│   - Conformance test server -
│
└── h2spec_server.c     # Minimal HTTP/2 server built on libhive.a.
                        # Uses libtls (LibreSSL) for TLS - the only file in
                        #   tests/ that depends on libtls. Not linked into
                        #   libhive.a itself.
                        # Handles all h2spec test scenarios: responds 200 to
                        #   valid requests, handles RST_STREAM, GOAWAY, PING,
                        #   SETTINGS exchanges, server push.
                        # Implements partial-send correctly: registers for
                        #   write-readiness when want_write() returns 1 and
                        #   retries hive_session_send() when writable again.
                        # Built by `make h2spec-server`.
                        # Used by `make h2spec` (runs h2spec against it).
                        # See TECH_STACK.md §7.7, TESTING.md §4.
```

---

## 5. tools/ - Developer Tools

```
tools/
│
└── hive_decode.c       # Offline HTTP/2 frame decoder.
                        # Reads raw HTTP/2 bytes (post-TLS-decryption) from
                        #   a file argument or stdin.
                        # Pretty-prints each frame: type name, flags, stream ID,
                        #   length, and known payload fields (SETTINGS params,
                        #   WINDOW_UPDATE increment, RST_STREAM error code,
                        #   GOAWAY last_stream_id and error code, etc.).
                        # Does NOT decode HPACK - shows raw compressed byte
                        #   count for HEADERS frames.
                        # Built from tools/hive_decode.c + src/hive_frame_bare.c
                        #   only. No dependency on hive_session_t, HPACK, stream
                        #   table, allocator, or any other src/ module.
                        #   This is why hive_frame_bare.c is factored out.
                        # Built by `make tools`.
                        # See TECH_STACK.md §7.8 for usage and expected output.
```

---

## 6. Top-Level Files

### Makefile

Single top-level Makefile. No per-directory Makefiles.

```makefile
# Key targets
make              # same as make release - builds libhive.a
make dev          # debug build with ASan/UBSan and HIVE_DEBUG=1
make release      # optimised build with -fPIC and hardening flags
make test         # build and run tests/run_tests.sh
make test-tsan    # run tests with ThreadSanitizer (Linux only)
make valgrind     # run tests under Valgrind (Linux only)
make h2spec-server # build tests/h2spec_server
make h2spec       # run full h2spec suite against h2spec_server
make tools        # build tools/hive_decode (links hive_frame_bare.c only)
make lint         # clang-tidy + cppcheck on src/
make clean        # remove build artifacts
make install      # install libhive.a and include/hive.h to PREFIX
```

Platform detected via `$(shell uname)`. Compiles `src/compat_str.c` only
on Linux. The `make install` target installs exactly two files and nothing
else - no man pages, no pkg-config, no service scripts.

The `make dev` target defines `-DHIVE_DEBUG=1` which enables ASan poisoning
of `hive_buf_t` data after callbacks. The `HIVE_TEST_CLOCK=1` flag may be
added to a debug build to enable the clock abstraction mock for flood tests:
`make dev EXTRA_CFLAGS=-DHIVE_TEST_CLOCK=1`.

### .clang-format

KNF-based formatting rules:

```yaml
BasedOnStyle: LLVM
IndentWidth: 8
UseTab: ForIndentation
BreakBeforeBraces: Linux     # functions: own line; control: same line
ColumnLimit: 80
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
```

### .clang-tidy

Static analysis checks:

```yaml
Checks: >
  clang-analyzer-*,
  cert-*,
  bugprone-*,
  performance-*,
  portability-*,
  -cert-err33-c,
  -bugprone-easily-swappable-parameters
```

---

## 7. Naming Conventions Across Files

Function names are prefixed by their module. This makes `grep` and
navigation unambiguous across the codebase.

| Module | File | Function prefix | Example |
|---|---|---|---|
| Session / public API | src/hive.c | `hive_` | `hive_session_recv()` |
| Frame bare (standalone) | src/hive_frame_bare.c | `frame_hdr_` | `frame_hdr_write_at()` |
| Frame parser | src/hive_frame.c | `frame_` | `frame_recv_process()` |
| HPACK | src/hive_hpack.c | `hpack_` | `hpack_decode_block()` |
| Stream table | src/hive_stream.c | `stream_` | `stream_open()` |
| Flow control | src/hive_flow.c | `flow_` | `flow_recv_data()` |
| Send queue | src/hive_send.c | `send_queue_` | `send_queue_append_ctrl()` |
| Security | src/hive_security.c | `security_` | `security_check_rst_flood()` |
| Clock | src/hive_clock.h | `hive_monotonic_` | `hive_monotonic_secs()` |
| Compat | src/compat_str.c | *(none)* | `strlcpy()`, `strlcat()` |

Public API functions (declared in `include/hive.h`) are grouped by namespace:

| Namespace | Functions |
|---|---|
| `hive_session_` | `hive_session_recv()`, `hive_session_send()`, `hive_session_want_read()`, `hive_session_want_write()`, `hive_session_*_new()`, `hive_session_free()`, `hive_session_feed_upgrade_headers()`, `hive_session_get_*()` |
| `hive_submit_` | `hive_submit_response()`, `hive_submit_trailers()`, `hive_submit_interim_response()`, `hive_submit_push_promise()`, `hive_submit_rst_stream()`, `hive_submit_goaway_prepare()`, `hive_submit_goaway_final()`, `hive_submit_ping()`, `hive_submit_ping_ack()`, `hive_submit_request()` |
| `hive_stream_` | `hive_stream_get_state()`, `hive_stream_set_user_data()`, `hive_stream_get_user_data()` |
| `hive_buf_` | `hive_buf_retain()`, `hive_buf_free()` |
| `hive_options_` | `hive_options_new()`, `hive_options_free()`, `hive_options_set_*()` |
| `hive_hpack_` | `hive_hpack_encoder_new()`, `hive_hpack_encoder_free()`, `hive_hpack_encode()`, `hive_hpack_decoder_new()`, `hive_hpack_decoder_free()`, `hive_hpack_decode()` |

Internal functions (in `src/`) use the module prefix listed in the table
above and are declared `static` if file-scoped or in the internal `.h` if
shared across files. No internal function name begins with `hive_` - that
prefix is reserved for the public API.

**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md, TESTING.md
