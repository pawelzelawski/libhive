# Development Plan

## Status Overview

**Last Updated**: 2026-04-22
**Current Phase**: Phase 1 — Foundation
**Next Task**: 1.2 — Test harness (already complete, combined with 1.1)

### Phase Summary

| Phase | Name | Status | Tests | Notes |
|---|---|---|---|---|
| 1 | Foundation | IN PROGRESS | — | Build system, test harness, compat, static tables |
| 2 | Frame Parser | NOT STARTED | — | Receive state machine, all 10 frame types, hive_frame_bare.c |
| 3 | HPACK | NOT STARTED | — | Encoder, decoder, Huffman, standalone API |
| 4 | Session Core | NOT STARTED | — | Minimal send queue, session struct, stream table, SETTINGS, preface |
| 5 | Flow Control and DATA | NOT STARTED | — | Windows, recv-side enforcement, WINDOW_UPDATE, DATA delivery |
| 6 | Submit and Send | NOT STARTED | — | Full send queue, partial-send, response/request submit, new callbacks |
| 7 | Security Hardening | NOT STARTED | — | Flood protection, exhaustion, limits, Content-Length, clock abstraction |
| 8 | h2c and Server Push | NOT STARTED | — | Upgrade path, PUSH_PROMISE, two-phase GOAWAY, client role |
| 9 | Conformance and Polish | NOT STARTED | — | h2spec, README, API reference, tools |

### Quality Milestones

| ID | Milestone | Status |
|---|---|---|
| M1 | Build system works on Linux and OpenBSD | DONE |
| M2 | All unit tests pass on Linux | NOT STARTED |
| M3 | All unit tests pass on OpenBSD | NOT STARTED |
| M4 | Valgrind clean on Linux | NOT STARTED |
| M5 | ASan/UBSan clean on both platforms | NOT STARTED |
| M6 | clang-format clean | NOT STARTED |
| M7 | clang-tidy clean | NOT STARTED |
| M8 | No direct malloc/free calls in src/ except the NULL-allocator shim | NOT STARTED |
| M9 | Full HPACK test suite passes (encode-decode round-trip) | NOT STARTED |
| M10 | Frame state machine handles split delivery (1 byte at a time) | NOT STARTED |
| M11 | h2spec full suite passes — zero failures | NOT STARTED |
| M12 | Wraith integration: HTTP/2 GET over TLS with arena allocator | NOT STARTED |

---

## Development Principles

Before starting any phase, read and follow these documents:

- **CODING_STANDARDS.md** — KNF style, allocator discipline, security
  practices, error handling. Every function written must comply.
- **ARCHITECTURE.md** — The specification. Every implementation decision
  must match what is documented there. If there is a conflict, the
  architecture document wins — raise it before deviating.
- **TECH_STACK.md** — Compiler flags, build system, test harness structure.

**Bottom-up approach**: Each phase builds on the previous. Do not start a
phase until the prior phase is complete and its tests pass.

**Test as you go**: Write tests for each component before moving to the
next. A component without passing tests is not done. For a zero-I/O library,
tests are straightforward: construct raw HTTP/2 byte sequences and feed them
directly to `hive_session_recv()`. No sockets, no threads, no timing.

**Platform parity**: Test on both Linux and OpenBSD at each phase. Platform
divergence caught late is much harder to fix than divergence caught at the
phase boundary.

**Allocator discipline**: Every allocation in `src/` goes through
`s->mem.*`. No direct malloc calls anywhere except the NULL-allocator shim
in `hive.c`. This is checked at each phase — a `grep malloc src/` that finds
anything other than the shim is a bug. See CODING_STANDARDS.md §2.

**Phase ordering**: The send queue (control frames) must exist before Phase 4
can queue SETTINGS ACKs. Phase 4 therefore includes a minimal send queue
implementation (task 4.0) covering control frames and basic drain. Phase 6
extends this to the full HEADERS and DATA send path with the partial-send
model. Phase 5 (flow control) requires this minimal send queue to test
WINDOW_UPDATE queuing.

**Incremental integration**: After Phase 4 the session can handle a basic
SETTINGS exchange. After Phase 5 it can receive DATA and enforce flow control.
After Phase 6 it can send a complete response. Each phase milestone is a
working, testable slice.

---

## Phase 1 — Foundation

**Goal**: Build system works on both platforms. Repository structure is in
place. Test harness compiles and runs. Platform compat layer is tested.
Static tables (HPACK static table, Huffman code table) are implemented and
verified against known vectors. `_Static_assert` checks are in place. Code
compiles clean with zero warnings.

**Reference documents**:
- TECH_STACK.md §5 — Makefile structure, compiler flags, build targets
- TECH_STACK.md §6 — test harness structure and assertion macro
- TECH_STACK.md §4.3 — strlcpy/strlcat compat for Linux
- CODING_STANDARDS.md §1.2 — file organisation and include order
- CODING_STANDARDS.md §1.3 — `_Static_assert` placement
- REPOSITORY_STRUCTURE.md §1 — top-level directory layout

**Prerequisite**: None.

### Tasks

**1.1 — Repository skeleton** ✓ DONE
- Create directory structure per REPOSITORY_STRUCTURE.md §1:
  `src/`, `include/`, `tests/`, `tools/`
- Create `include/hive.h` with skeleton (include guards, system includes,
  empty typedefs as forward declarations — no function bodies yet)
- Create `src/hive_internal.h` (internal shared types, forward declarations)
- Create top-level `Makefile` with stubs for all targets and working
  platform detection (`!=` shell assignment, BSD/GNU portable)
- Create `.clang-format` (KNF-based, per TECH_STACK.md §7.6)
- Create `.clang-tidy` (per TECH_STACK.md §7.4)
- Verify `make dev` and `make release` compile a trivial `src/hive.c`
  with zero warnings on Linux and OpenBSD

**1.2 — Test harness** ✓ DONE (combined with 1.1)
- Create `tests/test_harness.h` with the `RUN()` macro per TECH_STACK.md §6.1
- Create `tests/run_tests.c` as the test binary entry point
- Create `tests/run_tests.sh` — runs the binary, fails on non-zero exit
- `make test` runs empty suite, prints `0/0 tests passed`, exits 0
- `make valgrind` runs test binary under Valgrind, exits clean on Linux

**1.3 — Platform compat layer**
- Create `src/compat_str.c` and `src/compat_str.h`:
  portable `strlcpy(dst, src, dstsize)` and `strlcat(dst, src, dstsize)`
  implementations
- On OpenBSD: these files must not be compiled (`$(COMPAT_SRC)` is empty)
- On Linux: compiled in, no link flags needed
- Tests (in `tests/test_compat.c`):
  - `test_strlcpy_basic`: copies up to dstsize-1 bytes, always null-terminates
  - `test_strlcpy_truncation`: source longer than dst → truncated, null-terminated,
    returns full source length
  - `test_strlcpy_empty_src`: empty source → dst = `""`, returns 0
  - `test_strlcat_basic`: appends up to remaining space
  - `test_strlcat_full_dst`: dst already full → no write, returns combined length

**1.4 — HPACK static table and Huffman tables**
- In `src/hive_hpack.c`: implement the compile-time static table array
  (RFC 7541 Appendix A, 61 entries) per ARCHITECTURE.md §4.3
- Implement the Huffman decode table (256-entry `huff_entry_t` array,
  RFC 7541 Appendix B) per ARCHITECTURE.md §4.4 — the iterative
  bit-accumulator decode loop handles codes longer than 8 bits
- Implement the Huffman encode table (symbol → code + bit-length,
  RFC 7541 Appendix B, 257 entries including EOS)
- All three tables are compile-time constants. No runtime initialisation.
- Tests (in `tests/test_hpack.c` — static table and Huffman only):
  - `test_static_table_size`: 61 entries, index 1–61 all non-NULL
  - `test_static_table_index1`: index 1 = `:authority` with empty value
  - `test_static_table_index2`: index 2 = `:method` `GET`
  - `test_static_table_index61`: index 61 = `www-authenticate` with empty value
  - `test_huffman_decode_empty`: zero-length input → zero-length output
  - `test_huffman_decode_www`: RFC 7541 §C.4 example — decode `www.example.com`
  - `test_huffman_encode_decode_roundtrip`: encode then decode `no-cache` → same
  - `test_huffman_eos_rejected`: EOS symbol in non-terminal position → error
  - `test_huffman_invalid_padding`: trailing bits not all-ones → error

**1.5 — _Static_assert checks**
- Add all struct size assertions from CODING_STANDARDS.md §1.3 to
  `src/hive_internal.h`. They require the complete struct definitions,
  so they will expand and become active as structs are defined in later phases.
  Placeholder `_Static_assert(1 == 1, "placeholder")` in Phase 1.
  The real assertions activate as structs land in Phase 2 onward.

### Tests for Phase 1

Files: `tests/test_compat.c`, `tests/test_hpack.c` (static/Huffman only)

### Phase 1 Completion Criteria

- [ ] `make dev` succeeds with zero warnings on Linux
- [ ] `make dev` succeeds with zero warnings on OpenBSD
- [ ] `make test` runs and all Phase 1 tests pass on both platforms
- [ ] `make valgrind` clean on Linux
- [ ] ASan/UBSan clean on both platforms
- [ ] `make lint` produces zero warnings
- [ ] Quality milestone M1 confirmed

---

## Phase 2 — Frame Parser

**Goal**: The incremental frame parser correctly handles all 10 HTTP/2 frame
types from a byte stream. The state machine transitions correctly on split
delivery (bytes arriving in chunks of any size, including 1 byte at a time).
Frame header staging, `ctrl_staging` accumulation, and HEADERS reassembly
all work correctly. CONTINUATION lockout enforces a connection error (GOAWAY),
not a stream error. Unknown frame types are skipped correctly per RFC 9113 §4.1.
`hive_frame_bare.c` is factored out for the frame decoder tool.

No session, no HPACK, no stream table yet — this phase tests the frame
layer in isolation using minimal stubs.

**Reference documents**:
- ARCHITECTURE.md §3 — full receive state machine
- ARCHITECTURE.md §3.1 — all state enum values
- ARCHITECTURE.md §3.2 — frame header structure and frame type constants
- ARCHITECTURE.md §3.3 — processing loop pseudocode
- ARCHITECTURE.md §3.4 — new stream validation
- ARCHITECTURE.md §3.5 — CONTINUATION lockout (connection error)
- ARCHITECTURE.md §6.2 — `frame_hdr_write()` and `frame_hdr_write_at()`
- CODING_STANDARDS.md §3.1 — input validation: length before content,
  SETTINGS directionality (local_settings for inbound)
- CODING_STANDARDS.md §3.2 — SECURITY comments
- CODING_STANDARDS.md §4.2 — connection error vs stream error classification

**Prerequisite**: Phase 1 complete.

### Tasks

**2.1 — Frame type and flag constants**
- In `src/hive_frame.h`: define all `HIVE_FRAME_*` constants (0x0–0x9)
  per ARCHITECTURE.md §3.2
- Define all frame flag constants: `HIVE_FLAG_END_STREAM (0x01)`,
  `HIVE_FLAG_END_HEADERS (0x04)`, `HIVE_FLAG_PADDED (0x08)`,
  `HIVE_FLAG_PRIORITY (0x20)`, `HIVE_FLAG_ACK (0x01)` (for SETTINGS/PING)
- Define `frame_hdr_t` struct per ARCHITECTURE.md §3.2

**2.2 — Frame header serialisation (shared with hive_frame_bare.c)**
- In `src/hive_frame_bare.c` and `src/hive_frame_bare.h`:
  implement `frame_hdr_write_at()` and `frame_hdr_parse()` as standalone
  functions with no dependency on `hive_session_t`, callbacks, or stream state.
  This file is used by both `libhive.a` and the `tools/hive_decode` binary.
- In `src/hive_send.c`: implement `frame_hdr_write()` — a thin wrapper around
  `frame_hdr_write_at()` that writes into `send_buf + send_buf_used` and
  advances `send_buf_used`
- Tests verify the exact 9-byte output for known inputs

**2.3 — Receive state machine (hive_frame.c)**
- Implement all 18 receive states per ARCHITECTURE.md §3.1, including
  `RECV_SERVER_PREFACE` (client role: first frame must be SETTINGS non-ACK),
  `RECV_HEADERS_PAD` and `RECV_PUSH_PROMISE_PAD` (consume padding bytes after
  header payloads), and `RECV_GOAWAY_DEBUG` (collect optional GOAWAY debug data
  into `reassembly_buf` before firing `on_goaway`)
- `frame_hdr_buf[9]` / `frame_hdr_count` staging — accumulate until 9 bytes
  then parse into `cur_frame`
- `ctrl_staging[8]` / `ctrl_staging_count` — accumulate small payloads
  (SETTINGS params 6 bytes at a time, PING 8 bytes, RST_STREAM/WINDOW_UPDATE
  4 bytes, PRIORITY 5 bytes, GOAWAY first 8 bytes)
- HEADERS/CONTINUATION reassembly into `reassembly_buf`; set `reassembly_active = 1`
  and `reassembly_end_stream` when transitioning to RECV_CONTINUATION_PAYLOAD;
  clear `reassembly_active` on final END_HEADERS; route through RECV_HEADERS_PAD
  if `pad_remaining > 0` after payload is consumed
- DATA delivery: fire `on_data_chunk` with pointer directly into caller's
  buffer — no copy. Accumulate `payload_remaining` down to zero.
- `RECV_SKIP_PAYLOAD`: discard `payload_remaining` bytes without processing
- Padding: `pad_length` is a payload byte — it must be extracted inside the
  payload state (RECV_DATA_PAYLOAD, RECV_HEADERS_PAYLOAD, RECV_PUSH_PROMISE_PAYLOAD)
  as the first byte, NOT during the RECV_FRAME_HEADER → payload transition;
  account for HEADERS PRIORITY flag (5-byte prefix) via the persistent
  `priority_payload_len` field in Region G
- CONTINUATION lockout check in RECV_FRAME_HEADER (ARCHITECTURE.md §3.5):
  uses `reassembly_active` flag (not `reassembly_len`) to correctly handle
  zero-length HEADERS blocks; two checks required:
  - if `reassembly_active` and (frame is not CONTINUATION or stream_id mismatch)
    → `session_error()` — **connection error** (GOAWAY)
  - if `!reassembly_active` and frame is CONTINUATION → same connection error
- Unknown frame types: transition to RECV_SKIP_PAYLOAD per RFC 9113 §4.1

**2.4 — Frame validation**
- In RECV_FRAME_HEADER after parsing `cur_frame`:
  - Validate inbound `cur_frame.length` against **`local_settings.max_frame_size`**
    (what we advertised) — NOT `remote_settings`. See ARCHITECTURE.md §2.5
    and CODING_STANDARDS.md §3.1 for the SETTINGS directionality rule.
    → FRAME_SIZE_ERROR connection error on violation
  - Fixed-length frame validation per ARCHITECTURE.md §3.3:
    - SETTINGS with ACK flag: `length` must be 0 → FRAME_SIZE_ERROR
    - SETTINGS without ACK: `length` must be a multiple of 6 → FRAME_SIZE_ERROR
    - PING: `length` must be 8 → FRAME_SIZE_ERROR
    - RST_STREAM: `length` must be 4 → FRAME_SIZE_ERROR
    - WINDOW_UPDATE: `length` must be 4 → FRAME_SIZE_ERROR
    - PRIORITY: `length` must be 5 → FRAME_SIZE_ERROR
    - GOAWAY: `length` must be >= 8 → FRAME_SIZE_ERROR
    - PUSH_PROMISE (no PADDED flag): `length` must be >= 4 → FRAME_SIZE_ERROR
    - PUSH_PROMISE (PADDED flag set): `length` must be >= 5 → FRAME_SIZE_ERROR
  - `stream_id == 0` for DATA/HEADERS/RST_STREAM/CONTINUATION/PUSH_PROMISE
    → PROTOCOL_ERROR connection error
  - `stream_id == 0` for PRIORITY → PROTOCOL_ERROR connection error
    (RFC 9113 §6.3 — distinct from the above; PRIORITY on stream 0 is not
    in the same list but is equally prohibited)
  - `stream_id != 0` for SETTINGS/PING/GOAWAY → PROTOCOL_ERROR connection error

**2.5 — Stub session struct for testing**
- A minimal `hive_session_t` definition in `src/hive_internal.h` containing
  only the fields required by Phase 2: Region B options subset
  (`opt_max_frame_size`, `opt_max_continuation_size`), Region G
  (all frame receive state machine fields including `preface_count`),
  and `local_settings` with `max_frame_size` (for inbound validation).
  Remaining regions added in Phase 4.

### Tests for Phase 2

File: `tests/test_frame.c`

**Frame header serialisation**:
- `test_frame_hdr_write_data`: DATA frame, stream=1, length=100, flags=0x01
  → verify exact 9-byte output
- `test_frame_hdr_write_settings`: SETTINGS frame, stream=0, length=0,
  flags=0x01 (ACK) → verify exact 9-byte output
- `test_frame_hdr_write_headers`: HEADERS frame, length=20, flags=END_HEADERS
  → verify exact 9-byte output

**Receive state machine — frame type coverage**:
- `test_recv_settings_ack`: feed a 9-byte SETTINGS ACK frame → state machine
  processes it and returns to RECV_FRAME_HEADER
- `test_recv_ping`: feed a 17-byte PING frame (9 header + 8 payload) → fires
  no callback (stubs), transitions back to RECV_FRAME_HEADER
- `test_recv_window_update`: feed WINDOW_UPDATE for stream 0 and stream 1
- `test_recv_rst_stream`: feed RST_STREAM for stream 1
- `test_recv_data_full`: feed a complete DATA frame → `on_data_chunk` fires
  with correct pointer and length
- `test_recv_headers_end_headers`: feed a HEADERS+END_HEADERS frame →
  `reassembly_buf` populated, `reassembly_len` correct
- `test_recv_priority_ignored`: feed a PRIORITY frame → silently discarded
- `test_recv_unknown_type`: feed frame with type 0xFF → skipped, state
  machine continues

**Split delivery — core requirement**:
- `test_recv_split_frame_header`: feed the 9 frame header bytes one at a
  time (9 separate calls) → parsed correctly after the 9th byte
- `test_recv_split_data_payload`: feed a DATA frame in 1-byte chunks →
  `on_data_chunk` fires multiple times, total bytes correct
- `test_recv_split_settings_param`: feed SETTINGS payload in 1-byte chunks
  → all 6-byte parameters processed correctly
- `test_recv_headers_plus_continuation`: feed HEADERS without END_HEADERS
  then CONTINUATION with END_HEADERS → `reassembly_len` accumulates correctly

**Validation**:
- `test_recv_frame_too_large`: length > `local_settings.max_frame_size` →
  FRAME_SIZE_ERROR connection error (verifying inbound uses local_settings)
- `test_recv_data_on_stream_zero`: DATA with stream_id=0 → PROTOCOL_ERROR
- `test_recv_settings_nonzero_stream`: SETTINGS with stream_id=1 →
  PROTOCOL_ERROR
- `test_recv_continuation_lockout`: HEADERS without END_HEADERS then DATA
  (not CONTINUATION) → **connection error** PROTOCOL_ERROR (GOAWAY, not RST_STREAM)
- `test_recv_settings_bad_length_nonzero_ack`: SETTINGS ACK with length=6 →
  FRAME_SIZE_ERROR
- `test_recv_ping_wrong_length`: PING with length=4 → FRAME_SIZE_ERROR
- `test_recv_rst_stream_wrong_length`: RST_STREAM with length=5 → FRAME_SIZE_ERROR
- `test_recv_unknown_frame_mid_stream`: unknown frame type during normal
  operation → skipped, next frame parsed correctly

### Phase 2 Completion Criteria

- [ ] All Phase 2 tests pass on Linux and OpenBSD
- [ ] Split delivery test (1-byte feed) passes for all frame types
- [ ] CONTINUATION lockout test passes with connection error (GOAWAY), not RST_STREAM
- [ ] Frame length validation uses `local_settings.max_frame_size` (not remote)
- [ ] Fixed-length frame validation passes for all frame types
- [ ] `hive_frame_bare.c` builds standalone (no session dependency)
- [ ] Valgrind clean on test binary
- [ ] ASan/UBSan clean on both platforms
- [ ] Quality milestone M10 confirmed

---

## Phase 3 — HPACK

**Goal**: HPACK encoder and decoder are complete and correct. Static table
lookup, dynamic table insertion and eviction, Huffman encode/decode (with
correct bit-accumulator loop for codes > 8 bits), all four header field
representations, and dynamic table size updates work correctly. The standalone
`hive_hpack_encoder_t` and `hive_hpack_decoder_t` APIs work independently of
a session. `hpack_decode_block()` fires the correct callback sequence with
`hive_buf_t *` handles (by pointer). HPACK always-copy rule is enforced on
every dynamic table insertion. All RFC 7541 negative cases are rejected.

**Reference documents**:
- ARCHITECTURE.md §4 — full HPACK design
- ARCHITECTURE.md §4.1 — `hpack_table_t` structure; enc vs dec table ownership
- ARCHITECTURE.md §4.2 — entry layout, oversized entry eviction rule
- ARCHITECTURE.md §4.3 — static table
- ARCHITECTURE.md §4.4 — Huffman decode (iterative bit-accumulator loop)
- ARCHITECTURE.md §4.5 — `hpack_decode_block()` loop; size-update position rule;
  index-0 and out-of-range rejection
- ARCHITECTURE.md §4.6 — string decoding; truncated string rejection
- ARCHITECTURE.md §4.7 — integer varint decode; truncated varint rejection;
  `HPACK_INT_OVERFLOW` sentinel
- ARCHITECTURE.md §4.8 — encoder; multiple pending size-update rule
- ARCHITECTURE.md §4.9 — hash index for large tables
- ARCHITECTURE.md §8.1 — HPACK always-copy (security requirement)
- ARCHITECTURE.md §8.2 — HPACK bomb protection
- CODING_STANDARDS.md §2.3 — pre-allocation discipline
- CODING_STANDARDS.md §3.2 — SECURITY comments on bomb checks and always-copy

**Prerequisite**: Phase 1 complete. (Phase 3 is independent of Phase 2 —
HPACK is tested standalone before being integrated into the session.)

### Tasks

**3.1 — hpack_table_t: dynamic table**
- In `src/hive_hpack.c`: implement `hpack_table_init()`, `hpack_table_free()`
  using the session allocator interface (or standalone allocator for the
  standalone API)
- Implement ring buffer entry management:
  - `hpack_table_insert()`: allocate `sizeof(hpack_entry_t) + name_len + value_len`
    via allocator, copy name and value bytes in (ARCHITECTURE.md §4.2),
    store pointer in `ring[ring_head]`, advance `ring_head`,
    evict oldest entries until `size + rfc_size <= max_size`.
    Oversized entry rule: if `rfc_size > max_size` after full eviction,
    do not insert (table remains empty). See ARCHITECTURE.md §4.2.
  - `SECURITY:` comment on every dynamic table insertion confirming the copy
    (ARCHITECTURE.md §8.1)
  - `hpack_table_lookup()`: linear scan from `ring_head - 1` backward,
    return index and full/name-only match type
  - `hpack_table_evict_to()`: evict entries until `size <= new_max`

**3.2 — Integer varint encode/decode**
- `hpack_decode_int()` per ARCHITECTURE.md §4.7:
  handle multi-byte continuation, overflow guard at m > 28,
  return `HPACK_INT_OVERFLOW` on truncation or overflow;
  callers must check return value against `HPACK_INT_OVERFLOW`
- `hpack_encode_int()` — encode integer with N-bit prefix into output buffer

**3.3 — String encode/decode**
- `hpack_decode_string()`: detect Huffman flag, decode into `hpack_scratch_name`
  (for header names) or `hpack_scratch_value` (for header values) — two separate
  scratch buffers are required so both decoded strings remain valid simultaneously
  when the `on_header` callback fires (ARCHITECTURE.md §2.8, §4.6);
  return PROTOCOL_ERROR if `claimed_length > remaining_block_bytes`
  (truncated string rejection per ARCHITECTURE.md §4.6)
- `hpack_encode_string()`: Huffman-encode if shorter, otherwise literal;
  write length prefix and string bytes into output buffer

**3.4 — Full decoder: `hpack_decode_block()`**
- Implement the decode loop per ARCHITECTURE.md §4.5:
  - All four representations (indexed, literal with indexing, literal no
    index, literal never index, dynamic table size update)
  - Index-0 rejection: indexed representation with index == 0 →
    HIVE_ERR_COMPRESSION (fatal)
  - Out-of-range index rejection: index > 61 + dec_table.count →
    HIVE_ERR_COMPRESSION
  - Size-update position constraint: dynamic table size updates
    (0x20 pattern) only permitted before the first header field;
    after any header is decoded, a size update → HIVE_ERR_COMPRESSION
  - `on_header` is called with `hive_buf_t *name, hive_buf_t *value`
    (by pointer, not by value) per ARCHITECTURE.md §9.6
  - After `on_header` returns: clear `HIVE_BUF_VALID` in the actual handle
    objects; in HIVE_DEBUG builds, ASan-poison `reassembly_buf`,
    `hpack_scratch_name`, and `hpack_scratch_value` regions (not static/dynamic
    table data)
- Bomb protection: `decoded_size` and `decoded_count` running totals,
  checked after each header before `on_header` fires (ARCHITECTURE.md §8.2);
  `decoded_size` uses `name_len + value_len + 32` per entry (RFC 7541 §4.1)
- HTTP messaging validation (when `opt_no_http_messaging == 0`):
  pseudo-header rules, forbidden headers per RFC 9113 §8.3 — placeholder
  stub in Phase 3, full implementation in Phase 7

**3.5 — Full encoder: `hpack_encode_block()`**
- Implement per ARCHITECTURE.md §4.8 — static table exact match, static
  table name-only match, dynamic table lookup, literal fallback
- Emit dynamic table size update prefix when `has_pending == 1`
- Multiple pending updates: emit lowest reached value first, then final
  value per RFC 7541 §6.3
- Write directly into caller-provided output buffer

**3.6 — Hash index for large tables**
- Implement `hpack_hash_slot_t` index per ARCHITECTURE.md §4.9
- Hash index allocated only when `max_size > HPACK_LINEAR_THRESHOLD (16384)`
- FNV-1a on name bytes for probe position; `value_hash` field (FNV-1a on value
  bytes) stored per slot to support exact-match vs name-only distinction
- Two sentinels: `HPACK_HASH_EMPTY = 0xFFFFFFFF` (never used),
  `HPACK_HASH_TOMBSTONE = 0xFFFFFFFE` (evicted)
- On eviction: set slot to TOMBSTONE, **not** EMPTY — clearing to EMPTY breaks
  linear probe chains; tombstones allow probing to continue past evicted slots
- On lookup: probe skips tombstones, stops at EMPTY; prefer exact match (name
  + value) over name-only match; prefer newest entry among candidates

**3.7 — Standalone API**
- `hive_hpack_encoder_new()`, `hive_hpack_encoder_free()`,
  `hive_hpack_encode()` per ARCHITECTURE.md §9.12
- `hive_hpack_decoder_new()`, `hive_hpack_decoder_free()`,
  `hive_hpack_decode()` — incremental, single header per call

### Tests for Phase 3

File: `tests/test_hpack.c` (extended from Phase 1)

**Dynamic table**:
- `test_hpack_table_insert_basic`: insert one entry, verify lookup by index
- `test_hpack_table_evict_on_insert`: insert entries until eviction triggers,
  verify oldest entry is gone and `size` is correct
- `test_hpack_table_evict_to_zero`: `evict_to(0)` removes all entries
- `test_hpack_table_rfc_size`: `name+value+32` accounting correct
- `test_hpack_table_oversized_entry`: entry whose rfc_size > max_size → table
  empties and entry is NOT inserted; table remains valid and empty
- `test_hpack_always_copy`: after insert, overwrite original name buffer,
  verify table still returns correct name (confirming the copy, not a pointer)

**Integer encoding**:
- `test_hpack_int_decode_1byte`: value fits in prefix bits
- `test_hpack_int_decode_multibyte`: RFC 7541 §C.1 example (1337 with 5-bit prefix)
- `test_hpack_int_decode_truncated`: run out of input bytes mid-varint →
  returns `HPACK_INT_OVERFLOW`
- `test_hpack_int_decode_overflow`: varint that would exceed uint32 max →
  returns `HPACK_INT_OVERFLOW`
- `test_hpack_int_encode_decode_roundtrip`: encode then decode, verify value

**String encoding**:
- `test_hpack_string_decode_literal`: literal string `www.example.com`
- `test_hpack_string_decode_huffman`: RFC 7541 §C.4 Huffman example
- `test_hpack_string_encode_huffman`: encode `no-cache` → shorter than literal
- `test_hpack_string_scratch_limit`: string longer than `max_header_string_size`
  → HIVE_ERR_PROTOCOL
- `test_hpack_string_truncated`: claimed length > remaining block bytes →
  HIVE_ERR_COMPRESSION

**HPACK decode — RFC 7541 test vectors**:
- `test_hpack_decode_rfc_c3`: RFC 7541 §C.3 — first request (no Huffman,
  with indexing): `:method GET`, `:scheme http`, `:path /`, `:authority
  www.example.com`; verify decoded headers and dynamic table state
- `test_hpack_decode_rfc_c4`: RFC 7541 §C.4 — first request with Huffman
- `test_hpack_decode_rfc_c6`: RFC 7541 §C.6 — response with Huffman

**HPACK decode — negative cases**:
- `test_hpack_index_zero_rejected`: indexed representation with index=0 →
  HIVE_ERR_COMPRESSION
- `test_hpack_index_out_of_range`: index beyond static + dynamic table size →
  HIVE_ERR_COMPRESSION
- `test_hpack_size_update_after_header`: dynamic table size update appearing
  after first header field → HIVE_ERR_COMPRESSION
- `test_hpack_size_update_exceeds_pending_max`: size update with value >
  `dec_table.pending_max` → HIVE_ERR_COMPRESSION

**Full encode**:
- `test_hpack_encode_decode_roundtrip_no_huff`: encode a header list,
  decode the result, verify headers match
- `test_hpack_encode_decode_roundtrip_huff`: same with Huffman-encoded strings

**Bomb protection**:
- `test_hpack_bomb_size_limit`: construct a block that would decode to
  more than `max_header_list_size` bytes → HIVE_ERR_PROTOCOL after the
  limit is exceeded mid-block; verify stream error (RST_STREAM), not GOAWAY
- `test_hpack_bomb_count_limit`: construct a block with more than
  `max_header_count` headers → HIVE_ERR_PROTOCOL (stream error)

**on_header by-pointer delivery**:
- `test_hpack_header_callback_by_pointer`: verify that after `on_header`
  returns, `HIVE_BUF_VALID` is cleared in the library's handle objects
  (the ones whose addresses were passed to the callback); any read through
  a stale pointer sees the cleared flag

**Standalone API**:
- `test_hpack_standalone_encoder_decoder`: create encoder and decoder with
  `hive_hpack_encoder_new()` / `hive_hpack_decoder_new()`, encode a list,
  decode with the incremental decoder, verify round-trip

### Phase 3 Completion Criteria

- [ ] All RFC 7541 §C.3, §C.4, §C.6 test vectors pass exactly
- [ ] Always-copy test passes (SECURITY requirement M8 verified for HPACK)
- [ ] Oversized entry eviction test passes
- [ ] All negative-case tests pass (index-0, out-of-range, size-update position,
      truncated varint/string)
- [ ] Bomb protection tests pass; verified as stream errors (not connection errors)
- [ ] `on_header` by-pointer test passes
- [ ] All Phase 3 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms
- [ ] Quality milestone M9 confirmed

---

## Phase 4 — Session Core

**Goal**: A complete `hive_session_t` can be created and torn down correctly
with both the NULL allocator and a custom arena-style allocator. A minimal
send queue exists for control frames so that SETTINGS ACKs and WINDOW_UPDATEs
can be queued. The connection preface is generated correctly for client and
server roles. SETTINGS frames are received, applied, and ACK'd. The stream
hash table opens, looks up, and closes streams correctly. A simple HEADERS
frame (GET request) arriving on a new stream fires the correct callback
sequence. The session handles basic request-response at the protocol level,
without DATA frames (those come in Phase 5).

**Reference documents**:
- ARCHITECTURE.md §1.2 — session lifecycle
- ARCHITECTURE.md §2 — full session struct layout (all regions A–J)
- ARCHITECTURE.md §2.5 — SETTINGS state: pending_settings (outbound) vs
  inbound_settings_count (inbound flood protection); SETTINGS directionality
- ARCHITECTURE.md §5 — stream table: two-layer design, hash function, open/close
- ARCHITECTURE.md §5.4 — Knuth hash function
- ARCHITECTURE.md §5.5 — open, lookup, close sequences
- ARCHITECTURE.md §5.6 — hash table compaction
- ARCHITECTURE.md §6.3 — control frame queuing
- ARCHITECTURE.md §6.6 — minimal `hive_session_send()` drain
- ARCHITECTURE.md §9.5 — options table
- ARCHITECTURE.md §9.7 — session lifecycle functions
- CODING_STANDARDS.md §2 — allocator discipline, pre-allocation at creation
- CODING_STANDARDS.md §3.6 — `goto cleanup` for session creation

**Prerequisite**: Phases 2 and 3 complete.

### Tasks

**4.0 — Minimal send queue (prerequisite for all SETTINGS/ACK work)**
- Implement `send_queue_append_ctrl()` in `src/hive_send.c`:
  writes a 9-byte frame header + optional payload into `send_buf`,
  appends one iovec entry, advances `send_buf_used`
- Implement a minimal `hive_session_send()` in `src/hive.c`:
  - Call `send_queue_flush_data()` first (stub that does nothing in Phase 4)
  - If `send_iov_count == 0`: return `HIVE_OK`
  - Fire `callbacks.send(session, send_iov, send_iov_count, user_data)`
  - Handle partial write: update `send_partial_offset`; reset queue only
    on full send; set `send_partial = 1` on partial
  - On fatal error (-1 from callback): set session CLOSED, return error
- Implement `hive_session_want_write()`:
  returns 1 if `send_iov_count > 0 || send_partial == 1`; else 0
- This minimal implementation handles control frames only. Phase 6 extends
  it with HEADERS and DATA via `send_queue_flush_data()`.

**4.1 — Complete hive_session_t struct**
- Define the full `hive_session_t` in `src/hive_internal.h` with all 10
  regions (A through J) per ARCHITECTURE.md §2, including:
  - `preface_count` in Region G
  - `reassembly_active` and `priority_payload_len` in Region G
    (required for correct CONTINUATION lockout and split HEADERS handling)
  - `reassembly_end_stream` in Region G
  - `tombstone_count` and `closes_since_compact` in Region I
  - `peer_stream_open_count` in Region I (directional concurrent stream enforcement)
  - `inbound_settings_count` in Region E (separate from `pending_settings`)
  - `goaway_last_stream_id_sent` and `goaway_last_stream_id_recv` in Region C
  - `send_partial` and `send_partial_offset` in Region J
  - `goaway_prepare_sent` in Region C
  - `content_length_expected` (int64_t, -1 = absent) and
    `content_length_received` (uint64_t) in `hive_stream_t` Region
- Add all `_Static_assert` checks from CODING_STANDARDS.md §1.3 — these
  now have real struct definitions to check against
- Verify `sizeof(hive_stream_t) == 64` on both platforms

**4.2 — Options**
- Implement `hive_options_t` struct and all `hive_options_set_*()` functions
  using system malloc (options are not per-session)
- `hive_options_new()`: calloc a zeroed options struct, apply all defaults
- `hive_options_free()`: free with system free
- All option setters validate ranges per ARCHITECTURE.md §9.5 table

**4.3 — Session creation and teardown**
- Implement `hive_session_server_new()`, `hive_session_client_new()`,
  `hive_session_server_upgrade()`, `hive_session_feed_upgrade_headers()`
  per ARCHITECTURE.md §9.7
- `session_prealloc()` using `goto cleanup` pattern per CODING_STANDARDS.md §3.6:
  allocate all sub-buffers (stream_hash, stream_slots, stream_free_stack,
  send_iov, send_buf, reassembly_buf, hpack_scratch_name, hpack_scratch_value,
  enc_table.ring, dec_table.ring, pending_settings) in sequence; on any failure
  free all previously allocated sub-buffers and return NULL
- Initialise stream_free_stack with indices 0..max_concurrent-1
- Initialise stream_hash with EMPTY sentinels (0xFFFFFFFF)
- Server role: queue initial SETTINGS frame into send queue via
  `send_queue_append_ctrl()` (now available from task 4.0)
- Client role: queue 24-byte PRI * magic + SETTINGS into send queue
- `hive_session_free()`: free all sub-buffers via session allocator in
  reverse allocation order, then free the session struct itself

**4.4 — Stream table**
- Implement `stream_open()`, `stream_lookup()`, `stream_close()` per
  ARCHITECTURE.md §5.5 — `stream_close()` increments both `tombstone_count`
  and `closes_since_compact`
- Implement `stream_hash_fn()` (Knuth multiplicative hash) per
  ARCHITECTURE.md §5.4
- Implement hash table compaction per ARCHITECTURE.md §5.6
- Free stack: `stream_free_pop()` and `stream_free_push()`

**4.5 — SETTINGS receive and apply**
- In the RECV_SETTINGS_PAYLOAD state: process 6-byte parameter chunks via
  `ctrl_staging`, apply each known parameter to `remote_settings`
- SETTINGS directionality (ARCHITECTURE.md §2.5):
  - `SETTINGS_HEADER_TABLE_SIZE` received from peer → update `enc_table.pending_max`,
    set `enc_table.has_pending = 1` (constrains our encoder, not our decoder)
  - `SETTINGS_INITIAL_WINDOW_SIZE` → retroactively adjust all open stream
    `send_window` values
  - `SETTINGS_MAX_FRAME_SIZE` → update `remote_settings.max_frame_size`
    (governs our outbound frame sizing)
- Inbound SETTINGS flood check: increment `inbound_settings_count`; if
  `inbound_settings_count > opt_max_settings_pending` → GOAWAY PROTOCOL_ERROR
- Queue SETTINGS ACK via `send_queue_append_ctrl()` (from task 4.0) — 9-byte
  frame, no payload, flags=ACK; decrement `inbound_settings_count` when ACK queued
- Fire `on_settings` callback after ACK is queued
- On SETTINGS ACK received: validate `pending_count > 0`; pop from
  `pending_settings` ring; fire `on_settings_ack` callback

**4.6 — Connection preface validation**
- Server role: state = RECV_CLIENT_PREFACE; consume 24 bytes via `preface_count`,
  verify exact match against `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`;
  mismatch → `session_error()` PROTOCOL_ERROR
- Client role: state = RECV_SERVER_PREFACE; first frame must be a SETTINGS
  frame with ACK flag clear; anything else → `session_error()` PROTOCOL_ERROR

**4.7 — New stream creation from HEADERS**
- In RECV_FRAME_HEADER, when a HEADERS frame opens a new stream:
  validate ID parity, monotonicity, open count per ARCHITECTURE.md §3.4;
  call `stream_open()` with initial state OPEN

### Tests for Phase 4

File: `tests/test_session.c`

**Session creation**:
- `test_session_server_new_null_alloc`: create with NULL allocator, verify
  preface queued, `hive_session_want_write()` == 1
- `test_session_client_new_null_alloc`: create with NULL allocator, verify
  24-byte + SETTINGS preface queued
- `test_session_new_custom_alloc`: create with a tracking allocator that
  counts calls; verify exactly the expected number of allocations occur
  and no malloc calls after structural creation
- `test_session_free_all_allocations`: create and immediately free; verify
  tracking allocator shows zero outstanding allocations
- `test_session_new_alloc_failure`: tracking allocator fails on the 3rd
  allocation; verify session_new returns NULL and no memory is leaked

**Minimal send queue (task 4.0 verification)**:
- `test_send_control_frame_queued`: after session creation, `want_write()` == 1;
  after `hive_session_send()` with a full-write callback, `want_write()` == 0
- `test_send_partial_write`: send callback returns fewer bytes than queued;
  verify `want_write()` == 1 and next `hive_session_send()` resumes correctly
- `test_send_fatal_error`: send callback returns -1; verify session state
  is HIVE_SESSION_CLOSED

**Options**:
- `test_options_defaults`: `hive_options_new()` → all defaults match
  ARCHITECTURE.md §9.5 table
- `test_options_set_max_concurrent`: set to 50, create session, verify
  stream_slots array has 50 entries

**Stream table**:
- `test_stream_open_lookup_close`: open stream 1, lookup returns correct
  slot, close stream 1, lookup returns NULL
- `test_stream_hash_collision`: open streams 1, 257 (likely same hash bucket),
  verify both are found correctly
- `test_stream_free_stack`: open max_concurrent streams, verify open count
  equals max_concurrent; close one, verify open count decrements and slot
  is reusable
- `test_stream_compaction`: open and close 64 streams in sequence; verify
  `closes_since_compact` increments and compaction triggers when tombstone
  ratio exceeds 25%
- `test_stream_id_monotonicity`: attempt to open stream 3 after stream 5
  has been seen → PROTOCOL_ERROR connection error

**SETTINGS exchange**:
- `test_settings_recv_and_ack`: feed a SETTINGS frame with
  `INITIAL_WINDOW_SIZE = 131072`, verify `remote_settings` updated and
  SETTINGS ACK queued; verify `inbound_settings_count` decrements when ACK queued
- `test_settings_recv_ack`: feed a SETTINGS ACK, verify `pending_count`
  decrements and `on_settings_ack` fires
- `test_settings_unsolicited_ack`: feed SETTINGS ACK when `pending_count == 0`
  → PROTOCOL_ERROR connection error (GOAWAY)
- `test_settings_invalid_window_size`: `INITIAL_WINDOW_SIZE > 2^31-1` →
  FLOW_CONTROL_ERROR connection error
- `test_settings_invalid_frame_size`: `MAX_FRAME_SIZE < 16384` →
  PROTOCOL_ERROR connection error
- `test_settings_header_table_size_updates_encoder`: SETTINGS with
  `HEADER_TABLE_SIZE = 512` → `enc_table.pending_max = 512`,
  `enc_table.has_pending = 1` (encoder constrained, not decoder)

**Connection preface**:
- `test_server_preface_valid`: feed the 24-byte client magic + SETTINGS →
  passes, state = RECV_FRAME_HEADER
- `test_server_preface_invalid`: feed wrong bytes in preface → PROTOCOL_ERROR
- `test_client_preface_first_frame_not_settings`: client role; feed a PING
  frame as first server frame → PROTOCOL_ERROR
- `test_client_preface_settings_with_ack`: client role; feed SETTINGS with
  ACK flag as first server frame → PROTOCOL_ERROR

**End-to-end: HEADERS receive**:
- `test_recv_get_request_headers`: feed a complete HEADERS frame for stream 1
  (GET / HTTP/2) → `on_begin_headers`, `on_header` × 4, `on_headers_complete`
  fire in order with correct values; `on_header` receives `hive_buf_t *` handles

### Phase 4 Completion Criteria

- [ ] Session creates and frees with zero memory leaks (tracking allocator test)
- [ ] Allocation failure during creation leaves no leaks
- [ ] Partial send test passes (send_partial_offset tracking correct)
- [ ] SETTINGS exchange test passes; inbound/outbound counters are distinct
- [ ] SETTINGS_HEADER_TABLE_SIZE correctly updates encoder (not decoder)
- [ ] Client preface validation tests pass
- [ ] End-to-end HEADERS receive test passes (callbacks fire in order)
- [ ] All Phase 4 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms

---

## Phase 5 — Flow Control and DATA

**Goal**: Connection-level and stream-level flow control windows are tracked
correctly at both send and receive sides. The receive window is enforced —
DATA beyond the advertised receive window triggers FLOW_CONTROL_ERROR.
WINDOW_UPDATE frames are received and applied to `send_window`. DATA frames
are received and delivered via `on_data_chunk` with the correct zero-copy
pointer. `recv_consumed` accumulates correctly and WINDOW_UPDATE frames are
coalesced at the half-window threshold. `send_window` enforcement blocks DATA
frame emission when the window is zero.

**Reference documents**:
- ARCHITECTURE.md §7.7 — WINDOW_UPDATE coalescing logic
- ARCHITECTURE.md §3.3 — RECV_DATA_PAYLOAD with recv-side flow control enforcement
- ARCHITECTURE.md §2.4 — Region D: recv_window and enforcement semantics
- ARCHITECTURE.md §6.5 — DATA frame queuing (send-side)
- ARCHITECTURE.md §8.7 — receive-side flow control enforcement (SECURITY)
- ARCHITECTURE.md §9.6 — `on_data_chunk` callback lifetime contract
- CODING_STANDARDS.md §3.2 — SECURITY comments on flow control checks

**Prerequisite**: Phase 4 complete. (The minimal send queue from Phase 4
task 4.0 is required for WINDOW_UPDATE queuing.)

### Tasks

**5.1 — WINDOW_UPDATE receive**
- In RECV_WINDOW_UPDATE_PAYLOAD: accumulate 4 bytes via `ctrl_staging`, apply to
  `session->send_window` (stream_id == 0) or `stream->send_window`
- Validate increment > 0:
  - stream_id == 0: PROTOCOL_ERROR connection error
  - stream_id != 0: RST_STREAM PROTOCOL_ERROR (stream error)
- Validate no overflow > 0x7FFFFFFF:
  - stream_id == 0: FLOW_CONTROL_ERROR connection error
  - stream_id != 0: RST_STREAM FLOW_CONTROL_ERROR
- See ARCHITECTURE.md §3.3 for exact pseudocode

**5.2 — DATA receive with recv-side flow control enforcement**
- In RECV_DATA_PAYLOAD, before firing `on_data_chunk`:
  ```
  SECURITY: receive-side flow control enforcement
  if n > stream->recv_window: stream error FLOW_CONTROL_ERROR
  if n > session->recv_window: connection error FLOW_CONTROL_ERROR
  stream->recv_window  -= n
  session->recv_window -= n
  ```
- Fire `on_data_chunk` with pointer into caller's buffer (zero copy)
- Update `stream->recv_consumed` and `session->recv_consumed`
- WINDOW_UPDATE coalescing per ARCHITECTURE.md §7.7:
  when `recv_consumed > recv_window / 2`, queue WINDOW_UPDATE and restore
  `recv_window` by the update increment
- `SECURITY:` comment on the zero-copy pointer lifetime per
  CODING_STANDARDS.md §3.2
- Note: `data` points into caller-owned memory; the library cannot poison
  it. The on_data_chunk lifetime is a documented contract only, not ASan-enforced.

**5.3 — DATA send (flow control enforcement)**
- In `send_queue_flush_data()` (stub from Phase 4, now implemented):
  for each stream with a pending data_source, check both
  `session->send_window > 0` and `stream->send_window > 0` before calling
  `read_callback`; skip if either window is zero
- Compute `max_len = min(remote_settings.max_frame_size, send_window, stream_send_window)`
  (outbound frame sizing uses `remote_settings.max_frame_size` — what peer advertised)
- If `read_callback` returns 0 bytes: skip this stream, move to next

**5.4 — `hive_session_want_write()` update**
- Extends the Phase 4 implementation: also returns 1 when there are open
  streams with pending data_sources and `session->send_window > 0`

### Tests for Phase 5

File: `tests/test_flow.c`

- `test_window_update_connection`: feed WINDOW_UPDATE for stream 0,
  verify `session->send_window` increases
- `test_window_update_stream`: feed WINDOW_UPDATE for stream 1,
  verify `stream->send_window` increases
- `test_window_update_zero_increment_connection`: increment == 0 on stream 0
  → PROTOCOL_ERROR connection error
- `test_window_update_zero_increment_stream`: increment == 0 on stream 1
  → RST_STREAM PROTOCOL_ERROR (stream error, session continues)
- `test_window_update_overflow`: increment that would overflow int32 →
  FLOW_CONTROL_ERROR (stream or connection depending on stream_id)
- `test_data_recv_zero_copy`: feed a DATA frame, verify `on_data_chunk` pointer
  is within caller's input buffer (not a copy)
- `test_data_recv_partial`: feed a DATA frame in two chunks; verify
  `on_data_chunk` fires twice and total bytes correct
- `test_data_recv_exceeds_stream_window`: feed DATA totalling more than
  `stream->recv_window` → RST_STREAM FLOW_CONTROL_ERROR (stream error)
- `test_data_recv_exceeds_connection_window`: feed DATA totalling more than
  `session->recv_window` → FLOW_CONTROL_ERROR connection error (GOAWAY)
- `test_window_update_coalescing`: feed DATA frames accumulating to
  `recv_window / 2 + 1` bytes; verify exactly one WINDOW_UPDATE is queued,
  not one per DATA chunk; verify `recv_window` is restored
- `test_send_window_blocks_data`: set `stream->send_window = 0`; call
  `hive_session_send()` with pending data_source; verify no DATA frames
  emitted; then feed WINDOW_UPDATE; verify DATA flows on next send
- `test_send_max_len_respects_remote_max_frame_size`: set
  `remote_settings.max_frame_size = 100`; verify DATA frames are capped
  at 100 bytes (outbound uses peer's advertised max)

### Phase 5 Completion Criteria

- [ ] Zero-copy DATA test passes (pointer is in caller's buffer)
- [ ] Receive-side flow control enforcement tests pass (stream and connection)
- [ ] WINDOW_UPDATE coalescing test passes; recv_window correctly restored
- [ ] Flow control blocking test passes
- [ ] Outbound frame sizing respects `remote_settings.max_frame_size`
- [ ] All Phase 5 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms

---

## Phase 6 — Submit and Send

**Goal**: A complete request-response cycle works end-to-end. The server role
can receive a HEADERS frame (GET), call `hive_submit_response()` from within
the callback, and have `hive_session_send()` produce a correct batched iovec
containing the HEADERS response followed by DATA. Both copy and zero-copy
(`HIVE_DATA_FLAG_NO_COPY` with `uint8_t **buf` redirect) DATA paths work.
The partial-send model correctly retains unsent bytes and resumes on next call.
The client role can submit a request and receive a response. All new callbacks
(`on_goaway`, `on_ping`, `on_ping_ack`, `on_settings_ack`, `on_connection_error`)
are wired. Two-phase GOAWAY works. Stream introspection functions work.

**Reference documents**:
- ARCHITECTURE.md §6 — full send queue design
- ARCHITECTURE.md §6.3 — control frame queuing
- ARCHITECTURE.md §6.4 — HEADERS frame queuing and CONTINUATION splitting
  (new algorithm: contiguous payload, retroactive headers, interleaved iovecs)
- ARCHITECTURE.md §6.5 — DATA frame queuing (`uint8_t **buf` NO_COPY path)
- ARCHITECTURE.md §6.6 — partial-send drain model (ssize_t, send_partial_offset)
- ARCHITECTURE.md §6.7 — iovec overflow handling
- ARCHITECTURE.md §9.4 — data_source contract (`uint8_t **buf` signature)
- ARCHITECTURE.md §9.6 — new callbacks
- ARCHITECTURE.md §9.9 — submit functions including two-phase GOAWAY
- ARCHITECTURE.md §9.11 — stream introspection
- ARCHITECTURE.md §10 — full event loop walkthrough

**Prerequisite**: Phase 5 complete.

### Tasks

**6.1 — Full send queue: HEADERS frame queuing**
- In `send_queue_append_headers()`: implement the HEADERS splitting algorithm
  from ARCHITECTURE.md §6.4:
  - Reserve 9 bytes for first frame header
  - Encode HPACK block contiguously starting at `encode_start`
  - If `encoded_len <= remote_settings.max_frame_size`: single frame, fill
    pre-reserved header, single iovec entry
  - If `encoded_len > remote_settings.max_frame_size`: split — lay down
    CONTINUATION frame headers after the encoded payload in `send_buf`,
    then emit alternating iovecs (frame header, payload chunk, frame header,
    payload chunk...) so no bytes are shifted or copied
  - Outbound frame sizing always uses `remote_settings.max_frame_size`

**6.2 — Full send queue: DATA frame queuing (`uint8_t **buf`)**
- In `send_queue_flush_data()`: full implementation per ARCHITECTURE.md §6.5:
  - `body_ptr = send_buf + send_buf_used` (default: library buffer)
  - Call `data_source.read_callback(session, stream_id, &body_ptr, max_len,
    &data_flags, &data_source, user_data)` — note `&body_ptr` (pointer to pointer)
  - `HIVE_DATA_FLAG_NO_COPY`: callback redirected `body_ptr` to caller memory;
    iovec points to `body_ptr` (caller-owned)
  - Copy path: data already in `send_buf` at `send_buf + send_buf_used`;
    iovec points into `send_buf`
  - Determine `hdr_flags` based on `data_flags`: set END_STREAM bit if
    `HIVE_DATA_FLAG_EOF` is set; write the complete DATA frame header via
    `frame_hdr_write_at(send_buf + hdr_offset, bytes_written, HIVE_FRAME_DATA,
    hdr_flags, stream_id)` — do NOT use a retroactive `|=` on the reserved
    space before calling `frame_hdr_write_at()`; those bytes are uninitialised
    until `frame_hdr_write_at()` writes them
  - Update `session->send_window` and `stream->send_window`
  - State transition on EOF: OPEN → HALF_CLOSED_LOCAL,
    HALF_CLOSED_REMOTE → CLOSED (per ARCHITECTURE.md §6.5)

**6.3 — Full `hive_session_send()` (extends Phase 4 minimal)**
- Replace the Phase 4 minimal implementation with the full version per
  ARCHITECTURE.md §6.6:
  - `send_queue_flush_data()` called first (now fully implemented)
  - `send` callback returns `ssize_t` bytes written
  - Build effective iov by skipping `send_partial_offset` bytes
  - Update `send_partial_offset += written`
  - Full send: reset `send_iov_count = 0`, `send_buf_used = 0`,
    `send_partial_offset = 0`, `send_partial = 0`
  - Partial send: set `send_partial = 1`; retain queue for next call
- iovec overflow: if `send_iov_count + needed > opt_max_send_iov`,
  flush internally before continuing (ARCHITECTURE.md §6.7)

**6.4 — `hive_submit_response()`**
- Validate stream exists and is in HALF_CLOSED_REMOTE or OPEN state
- Queue HEADERS via `send_queue_append_headers()`
- Store `data_source` in `stream->data_source` — fixed for stream lifetime
- Stream state transitions per ARCHITECTURE.md §5.3

**6.5 — Additional submit functions**
- `hive_submit_trailers()` — HEADERS with END_STREAM, no pseudo-headers
- `hive_submit_interim_response()` — HEADERS with 1xx :status, no DATA follows
- `hive_submit_goaway_prepare()` — sends GOAWAY with last_stream_id=0x7FFFFFFF;
  sets `goaway_prepare_sent = 1`, `goaway_sent = 1`; does not close session yet
- `hive_submit_goaway_final()` — sends GOAWAY with `last_stream_id_remote` and
  given error_code; sets `session_state = HIVE_SESSION_GOAWAY_SENT`
- `hive_submit_rst_stream()` — RST_STREAM (uses `HIVE_H2_*` wire error codes,
  not `HIVE_ERR_*`)
- `hive_submit_ping()` — PING frame
- `hive_submit_ping_ack()` — PING ACK (used when `opt_no_auto_ping_ack == 1`)

**6.6 — `hive_submit_request()` (client role)**
- Allocate new stream ID (odd, incrementing), call `stream_open()`
- Check `stream_open_count < remote_settings.max_concurrent_streams`
  (peer's limit); return `HIVE_ERR_REFUSED_STREAM` if at limit
- Queue HEADERS via `send_queue_append_headers()`
- Store `data_source` in stream slot

**6.7 — New callbacks: wire up all five**
- `on_settings_ack`: fire in RECV_SETTINGS_PAYLOAD on ACK path (Phase 4
  stub should already call it; verify it is correct here)
- `on_goaway`: fire in RECV_GOAWAY_PAYLOAD after parsing last_stream_id
  and error_code, with debug_data pointer into reassembly_buf
- `on_ping`: fire in RECV_PING_PAYLOAD when ACK=0 and `opt_no_auto_ping_ack=1`
- `on_ping_ack`: fire in RECV_PING_PAYLOAD when ACK=1
- `on_connection_error`: fire in `session_error()` before GOAWAY is queued

**6.8 — Stream introspection**
- `hive_stream_get_state()` — return `hive_stream_state_t` for a stream_id;
  return `HIVE_STREAM_IDLE` if not found
- `hive_stream_set_user_data()` — set `stream->user_data`; return
  `HIVE_ERR_STREAM_CLOSED` if not found
- `hive_stream_get_user_data()` — return `stream->user_data`; NULL if not found

### Tests for Phase 6

File: `tests/test_session.c` (extended)

**Partial send model**:
- `test_send_partial_resume`: queue 3 frames; send callback returns half the
  bytes; verify `want_write()` == 1; call `hive_session_send()` again; verify
  remaining bytes sent and queue is empty
- `test_send_fires_once_per_call`: queue multiple frames; verify send callback
  fires exactly once per `hive_session_send()` call

**Submit response**:
- `test_submit_response_headers_only`: submit response with NULL data_source;
  send callback receives correct HEADERS frame bytes
- `test_submit_response_with_data_copy`: submit response with data_source
  returning small body (copy path); verify iovec has HEADERS + DATA header
  + DATA body; verify END_STREAM in DATA frame header flags
- `test_submit_response_no_copy`: submit with `HIVE_DATA_FLAG_NO_COPY`;
  callback redirects `*buf` to caller memory; verify DATA body iovec points
  to that caller memory, not into `send_buf`
- `test_submit_response_eof_flag`: verify END_STREAM bit set retroactively
  at `send_buf[hdr_offset + 4]` when `HIVE_DATA_FLAG_EOF` is set

**End-to-end round trip**:
- `test_full_get_request_response`: feed a HEADERS frame (GET /), submit
  response with a small body, call `hive_session_send()`, verify the
  captured iovec bytes decode to a valid HTTP/2 response; feed the iovec
  bytes back to a second session as if a client received them, verify
  client fires correct callbacks

**New callbacks**:
- `test_on_settings_ack_fires`: send SETTINGS, feed ACK → `on_settings_ack`
  fires exactly once
- `test_on_goaway_fires`: feed GOAWAY frame → `on_goaway` fires with correct
  last_stream_id, error_code, and debug_data pointer
- `test_on_ping_fires_when_no_auto_ack`: set `opt_no_auto_ping_ack=1`; feed
  PING → `on_ping` fires; no ACK queued automatically
- `test_on_ping_ack_fires`: feed PING ACK → `on_ping_ack` fires
- `test_on_connection_error_fires_before_goaway`: trigger a protocol error;
  verify `on_connection_error` fires before GOAWAY is queued

**Two-phase GOAWAY**:
- `test_goaway_prepare`: call `hive_submit_goaway_prepare()`; verify GOAWAY
  with last_stream_id=0x7FFFFFFF queued; verify new streams still accepted
  (prepare phase doesn't close session)
- `test_goaway_final`: call `hive_submit_goaway_final()`; verify GOAWAY with
  `last_stream_id_remote` queued; verify no new streams accepted

**Client role**:
- `test_submit_request_assigns_stream_id`: first request → stream ID 1,
  second request → stream ID 3
- `test_submit_request_max_concurrent_honored`: set `remote_settings.max_concurrent_streams = 2`;
  submit 3 requests → third returns `HIVE_ERR_REFUSED_STREAM`
- `test_submit_request_with_body`: submit request with data_source, verify
  HEADERS + DATA frames in send queue

**Stream introspection**:
- `test_stream_get_state`: open stream 1 → OPEN; send END_STREAM → HALF_CLOSED_LOCAL
- `test_stream_user_data`: set user_data on stream 1; get it back; close stream;
  get returns NULL

**iovec overflow**:
- `test_iovec_overflow`: set `opt_max_send_iov = 4`; queue 5 frames; verify
  send callback fires during queue overflow, then once more for the 5th frame

### Phase 6 Completion Criteria

- [ ] Full request-response round-trip test passes
- [ ] `NO_COPY` iovec pointer test passes (`*buf` redirect works correctly)
- [ ] Partial send resume test passes
- [ ] All five new callbacks fire correctly
- [ ] Two-phase GOAWAY tests pass
- [ ] MAX_CONCURRENT_STREAMS enforcement in `hive_submit_request()` passes
- [ ] All Phase 6 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms
- [ ] Quality milestones M2 and M3 confirmed (all unit tests pass)

---

## Phase 7 — Security Hardening

**Goal**: All security limits are implemented and enforced. HPACK bomb
protection (both limits, stream errors), CONTINUATION flood protection
(connection error / GOAWAY), SETTINGS flood protection (inbound counter),
RST_STREAM flood detection (rolling rate counter with clock abstraction),
and stream ID exhaustion handling all work correctly. HTTP messaging validation
(RFC 9113 §8) is complete including Content-Length consistency. `hive_buf_t`
ASan poisoning is active in `HIVE_DEBUG` builds. All security limits are
configurable via the options API.

**Reference documents**:
- ARCHITECTURE.md §8 — full security architecture
- ARCHITECTURE.md §8.2 — HPACK bomb (stream errors, both limits)
- ARCHITECTURE.md §8.3 — CONTINUATION flood (connection error, GOAWAY)
- ARCHITECTURE.md §8.4 — SETTINGS flood (`inbound_settings_count`, not pending ring)
- ARCHITECTURE.md §8.5 — RST_STREAM flood (rolling rate counter, CLOCK_MONOTONIC)
- ARCHITECTURE.md §8.6 — stream ID exhaustion (auto-GOAWAY at 2^31-1000)
- ARCHITECTURE.md §8.7 — receive-side flow control; buffer lifetime enforcement
- ARCHITECTURE.md §8.8 — `hive_buf_t` by-pointer ASan poisoning
- CODING_STANDARDS.md §3.2 — SECURITY comments on every check
- CODING_STANDARDS.md §3.5 — `hive_buf_t` lifetime enforcement (by pointer)
- CODING_STANDARDS.md §4.2 — error classification table

**Prerequisite**: Phase 6 complete.

### Tasks

**7.1 — CONTINUATION flood protection (§8.3) — connection error**
- In RECV_HEADERS_PAYLOAD and RECV_CONTINUATION_PAYLOAD: check
  `reassembly_len + n > opt_max_continuation_size` before writing to
  `reassembly_buf`; add `SECURITY:` comment
- Use `session_error()` (connection error / GOAWAY), **not** `stream_error()`
  (RST_STREAM) — this was a bug in earlier pseudocode; see CODING_STANDARDS.md §4.2
  and ARCHITECTURE.md §8.3

**7.2 — SETTINGS flood protection (§8.4)**
- SETTINGS flood is tracked via `inbound_settings_count` (Region E),
  **not** the `pending_settings` ring (which tracks outbound SETTINGS)
- In RECV_SETTINGS_PAYLOAD on non-ACK SETTINGS: increment
  `inbound_settings_count`; if `inbound_settings_count > opt_max_settings_pending`
  → GOAWAY PROTOCOL_ERROR
- Decrement `inbound_settings_count` when SETTINGS ACK is queued
- On SETTINGS ACK received: validate `pending_count > 0`; if zero →
  PROTOCOL_ERROR (unsolicited ACK, connection error)

**7.3 — RST_STREAM flood detection (§8.5) with clock abstraction**
- Introduce a clock abstraction in `src/hive_clock.h`:
  `uint64_t hive_monotonic_secs(void)` — calls `clock_gettime(CLOCK_MONOTONIC)`.
  In test builds (`HIVE_TEST_CLOCK=1`), this reads from a global
  `uint64_t hive_test_clock_secs` that tests can advance directly.
  This eliminates timing dependencies from security tests.
- Implement rolling rate counter using `hive_monotonic_secs()`:
  on each received RST_STREAM: check/reset window, increment counter,
  fire `on_rst_stream_flood` callback if threshold exceeded
- Add `SECURITY:` comment

**7.4 — Stream ID exhaustion (§8.6)**
- In new stream validation (Phase 4 task 4.7): after accepting the stream,
  check `stream_id > (0x7FFFFFFFu - 1000u)`; if true and `goaway_sent == 0`,
  call `hive_submit_goaway_prepare(s)` — not `hive_submit_goaway_final()`,
  as the two-phase approach is correct here (allow in-flight streams to finish)

**7.5 — HTTP messaging validation (RFC 9113 §8) — complete**
- When `opt_no_http_messaging == 0`, enforce fully in `hpack_decode_block()`:
  - Pseudo-header ordering: pseudo-headers must appear before regular headers
  - Unknown pseudo-headers → RST_STREAM PROTOCOL_ERROR
  - Duplicate pseudo-headers for same block → RST_STREAM PROTOCOL_ERROR
  - No pseudo-headers in trailers → RST_STREAM PROTOCOL_ERROR
  - Lowercase field names enforced: uppercase → RST_STREAM PROTOCOL_ERROR
  - Forbidden headers: `Connection`, `Keep-Alive`, `Proxy-Connection`,
    `Upgrade`, `Transfer-Encoding` → RST_STREAM PROTOCOL_ERROR
  - `TE` header: only `trailers` value permitted
  - `Content-Length` consistency: track expected body length in stream struct;
    on END_STREAM, if actual DATA bytes != Content-Length → RST_STREAM
    PROTOCOL_ERROR. The fields `content_length_expected` (int64_t, -1 = absent)
    and `content_length_received` (uint64_t) are already part of `hive_stream_t`
    as defined in Phase 4; the `_Static_assert` already reflects the 64-byte size.

**7.6 — `hive_buf_t` ASan poisoning (by-pointer delivery)**
- In `hpack_decode_block()`, after `on_header` callback returns:
  - Clear `HIVE_BUF_VALID` in `name->flags` and `value->flags`
    (the library's own handle objects, addressed via the pointers passed
    to the callback — NOT local copies)
  - In `HIVE_DEBUG` builds:
    - If `name->data` points into `reassembly_buf`, `hpack_scratch_name`, or
      `hpack_scratch_value`:
      `HIVE_ASAN_POISON(name->data, name->len)`
    - If `value->data` points into `reassembly_buf`, `hpack_scratch_name`, or
      `hpack_scratch_value`:
      `HIVE_ASAN_POISON(value->data, value->len)`
    - Do **not** poison data pointing into the static table or live dynamic
      table entries — those regions are reused across callbacks
- In `hive_buf_retain()`: call `HIVE_ASAN_UNPOISON(buf->data, buf->len)`
  before reading the data for copying (the source may have been poisoned)
- Verify: in `make dev` test run, accessing `name->data` or `value->data`
  after `on_header` returns (without calling `hive_buf_retain()`) triggers
  an ASan abort for headers decoded from `reassembly_buf`,
  `hpack_scratch_name`, or `hpack_scratch_value`

### Tests for Phase 7

File: `tests/test_security.c`

- `test_continuation_flood_is_connection_error`: feed HEADERS without
  END_HEADERS then CONTINUATION frames totalling > `opt_max_continuation_size`
  → GOAWAY (connection error), not RST_STREAM; session is CLOSED
- `test_settings_flood_uses_inbound_counter`: send
  `opt_max_settings_pending + 1` SETTINGS frames without ACKs →
  GOAWAY on the last one; verify it is `inbound_settings_count` that
  triggered it (not `pending_count`)
- `test_settings_unsolicited_ack`: feed SETTINGS ACK with `pending_count == 0`
  → GOAWAY PROTOCOL_ERROR
- `test_rst_stream_flood_callback`: using `hive_test_clock_secs` to fix time,
  send `opt_rst_flood_threshold + 1` RST_STREAM frames within the window →
  `on_rst_stream_flood` fires with correct rate
- `test_rst_stream_flood_window_reset`: advance `hive_test_clock_secs` past
  the window; send threshold RSTs → callback not fired (counter reset)
- `test_stream_id_exhaustion_triggers_prepare`: open a stream with ID =
  2^31 - 999 → `hive_submit_goaway_prepare()` called; GOAWAY with
  last_stream_id=0x7FFFFFFF queued; session still OPEN (not CLOSED)
- `test_http_messaging_pseudo_after_regular`: HEADERS with `:method`
  appearing after `content-type` → RST_STREAM PROTOCOL_ERROR (stream error)
- `test_http_messaging_uppercase_field_name`: HEADERS with `Content-Type`
  (uppercase C and T) → RST_STREAM PROTOCOL_ERROR
- `test_http_messaging_duplicate_pseudo_header`: HEADERS with `:method`
  appearing twice → RST_STREAM PROTOCOL_ERROR
- `test_http_messaging_forbidden_connection_header`: HEADERS with
  `Connection: keep-alive` → RST_STREAM PROTOCOL_ERROR
- `test_http_messaging_te_invalid_value`: `TE: gzip` → RST_STREAM PROTOCOL_ERROR
- `test_http_messaging_content_length_mismatch`: request with
  `Content-Length: 100`; feed 50 bytes of DATA with END_STREAM →
  RST_STREAM PROTOCOL_ERROR
- `test_hpack_negative_index_zero`: (integration check — covered by Phase 3
  unit test; confirm session handles the error correctly)
- `test_hive_buf_asan_poisoning`: in `HIVE_DEBUG` build, save `name->data`
  during `on_header` callback (without retaining); access it after callback
  returns → ASan `heap-use-after-poison` abort (manual verification;
  documented as a comment in test_security.c)

### Phase 7 Completion Criteria

- [ ] CONTINUATION flood test confirms connection error (GOAWAY), not RST_STREAM
- [ ] SETTINGS flood test confirms `inbound_settings_count` is the counter
- [ ] RST_STREAM flood tests pass using clock abstraction (no sleep)
- [ ] Stream ID exhaustion triggers prepare-phase GOAWAY, not final GOAWAY
- [ ] HTTP messaging validation tests pass: all rule categories covered
- [ ] Content-Length consistency enforcement passes
- [ ] Quality milestone M8 confirmed (allocator discipline audit)
- [ ] All Phase 7 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms

---

## Phase 8 — h2c and Server Push

**Goal**: h2c cleartext connections work — both direct h2c (no Upgrade) and
the HTTP/1.1 Upgrade path via `hive_session_server_upgrade()` +
`hive_session_feed_upgrade_headers()`. The client role can initiate requests
and receive responses correctly. Server push works: `hive_submit_push_promise()`
sends a PUSH_PROMISE frame, reserves the promised stream, and the promised
stream can be responded to. Two-phase GOAWAY shutdown is tested end-to-end.

**Reference documents**:
- ARCHITECTURE.md §1.2 — session lifecycle diagram (h2c Upgrade path)
- ARCHITECTURE.md §9.7 — `hive_session_server_upgrade()` and
  `hive_session_feed_upgrade_headers()`
- ARCHITECTURE.md §9.9 — `hive_submit_push_promise()`,
  `hive_submit_goaway_prepare()`, `hive_submit_goaway_final()`
- ARCHITECTURE.md §9.10 — `hive_submit_request()` (client role)
- ARCHITECTURE.md §5.3 — stream states: RESERVED_LOCAL, RESERVED_REMOTE
- ARCHITECTURE.md §5.7 — stream state legality table

**Prerequisite**: Phase 7 complete.

### Tasks

**8.1 — h2c Upgrade path**
- `hive_session_server_upgrade()` per ARCHITECTURE.md §9.7:
  parse `settings_payload` (base64url-decoded bytes) as a SETTINGS body;
  apply to `remote_settings`; initialise stream 1 in HALF_CLOSED_REMOTE
  state; queue server preface SETTINGS as normal
- `hive_session_feed_upgrade_headers()` per ARCHITECTURE.md §9.7:
  inject the HTTP/1.1 request as stream 1's HEADERS by calling
  `hpack_decode_block()` directly with the provided header array;
  fires `on_begin_headers`, `on_header` × nvlen, `on_headers_complete`
  through the normal callback path (ensures bomb limits and messaging
  validation apply to the upgraded request)

**8.2 — Server push**
- `hive_submit_push_promise()` per ARCHITECTURE.md §9.9:
  validate `SETTINGS_ENABLE_PUSH != 0` from remote_settings;
  allocate next even server stream ID; open stream in RESERVED_LOCAL state;
  queue PUSH_PROMISE frame (4-byte promised stream ID + HPACK-encoded
  request headers)
- Push response: caller calls `hive_submit_response()` on the reserved
  stream ID; stream transitions from RESERVED_LOCAL to HALF_CLOSED_REMOTE
  when HEADERS is sent, then to CLOSED when DATA END_STREAM is sent

**8.3 — PUSH_PROMISE receive (client role)**
- In RECV_PUSH_PROMISE_PAYLOAD: extract 4-byte promised stream ID; validate
  ID is even and > last seen server stream; copy remainder into
  `reassembly_buf`; on END_HEADERS, call `hpack_decode_block()`
- Open promised stream in RESERVED_REMOTE state
- Fire `on_push_promise` callback with the promised stream ID

**8.4 — Two-phase GOAWAY: end-to-end test path**
- `hive_submit_goaway_prepare()` and `hive_submit_goaway_final()` are
  implemented in Phase 6. Phase 8 tests the complete two-phase sequence:
  prepare → drain in-flight streams → final → session free
- `on_goaway` receive path (implemented in Phase 6 task 6.7) is tested here
  end-to-end: feed GOAWAY from peer; verify `want_read()` returns 0 (advisory);
  verify streams with ID > last_stream_id are closed with REFUSED_STREAM;
  verify streams with ID <= last_stream_id remain open for in-flight responses

**8.5 — Client role: full request-response**
- `hive_submit_request()` is implemented in Phase 6. Phase 8 adds the
  complementary client-side receive path: HEADERS response fires
  `on_begin_headers`, `on_header` × n, `on_headers_complete` in the same
  sequence as on the server side; DATA response fires `on_data_chunk`.

### Tests for Phase 8

File: `tests/test_session.c` (extended)

- `test_h2c_upgrade_settings_applied`: create with `server_upgrade()`,
  verify client's HTTP2-Settings are applied to `remote_settings`
- `test_h2c_upgrade_stream1_open`: verify stream 1 is in HALF_CLOSED_REMOTE
  after `server_upgrade()`
- `test_h2c_feed_upgrade_headers_fires_callbacks`: call
  `hive_session_feed_upgrade_headers()`; verify `on_begin_headers`,
  `on_header`, `on_headers_complete` fire with correct values; verify
  bomb limits apply
- `test_server_push_promise`: submit PUSH_PROMISE, verify PUSH_PROMISE
  frame bytes correct and stream in RESERVED_LOCAL state
- `test_server_push_response`: send push response on reserved stream,
  verify stream transitions through HALF_CLOSED_REMOTE to CLOSED
- `test_push_disabled_by_remote_settings`: `remote_settings.enable_push = 0`
  then `hive_submit_push_promise()` → `HIVE_ERR_PROTOCOL`
- `test_goaway_two_phase`: call `hive_submit_goaway_prepare()`; verify
  GOAWAY with 0x7FFFFFFF queued and session still OPEN; call
  `hive_submit_goaway_final()`; verify GOAWAY with `last_stream_id_remote`
  queued and session GOAWAY_SENT; verify no new streams accepted
- `test_goaway_recv_want_read_advisory`: feed GOAWAY from peer; verify
  `want_read()` returns 0; manually call `hive_session_recv()` again
  (simulating caller ignoring advisory) → no crash
- `test_goaway_recv_streams_closed`: feed GOAWAY with last_stream_id=1;
  have streams 1, 3, 5 open; verify streams 3 and 5 are closed with
  REFUSED_STREAM; stream 1 remains open
- `test_client_full_request_response`: client submits GET; server responds
  with 200 + body; feed response bytes to client session; verify client
  fires `on_headers_complete` and `on_data_chunk` callbacks

### Phase 8 Completion Criteria

- [ ] h2c Upgrade tests pass; `feed_upgrade_headers()` fires callbacks correctly
- [ ] Server push tests pass; reserved stream state transitions correct
- [ ] Two-phase GOAWAY end-to-end test passes
- [ ] `want_read()` advisory test passes (0 after GOAWAY recv, but no crash
      if caller continues reading)
- [ ] Client role end-to-end test passes
- [ ] All Phase 8 tests pass on Linux and OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms

---

## Phase 9 — Conformance and Polish

**Goal**: All h2spec tests pass. The frame decoder tool is complete.
README.md is written and accurate. The API reference comments in `hive.h`
are complete. All quality milestones are confirmed. The library is ready
for embedder use.

**Reference documents**:
- TECH_STACK.md §7.7 — h2spec installation and usage
- TECH_STACK.md §7.8 — frame decoder tool
- ARCHITECTURE.md §10 — event loop walkthrough (README example source)

**Prerequisite**: Phase 8 complete.

### Tasks

**9.1 — h2spec test server (`tests/h2spec_server.c`)**
- Minimal HTTP/2 server built on `libhive.a` using libtls for TLS
- Handles all h2spec test scenarios: responds with 200 to all valid
  requests, handles RST_STREAM, GOAWAY, PING, SETTINGS exchanges correctly
- Tested via `make h2spec-server && ./tests/h2spec_server --port 8443 &`

**9.2 — h2spec run and fix**
- Run the full h2spec suite: `make h2spec`
- Fix all failures. Every failure is either a genuine bug in Hive or a
  misunderstanding of the RFC — resolve against the RFC, not against h2spec
  alone. Document any disagreement explicitly before concluding Hive is
  correct and h2spec is wrong.
- Target: zero failures across all h2spec test groups

**9.3 — Frame decoder tool (`tools/hive_decode.c`)**
- Implement per TECH_STACK.md §7.8:
  reads raw HTTP/2 bytes from file or stdin;
  pretty-prints each frame: type name, flags, stream ID, length, and known
  payload fields (SETTINGS params, error codes, WINDOW_UPDATE increment);
  built from `tools/hive_decode.c` + `src/hive_frame_bare.c` only —
  no dependency on `hive_session_t`, HPACK, stream table, or allocator
- `make tools` builds the binary

**9.4 — README.md**
- Written for an embedder coming to the project cold
- Contents: one-paragraph description, requirements (libc only), how to
  build (`make && make install`), minimal integration example (server role:
  the ~30-line event loop pattern from ARCHITECTURE.md §10), known
  limitations, link to ARCHITECTURE.md for full design
- The integration example must compile and run correctly

**9.5 — API reference comments in `include/hive.h`**
- All public functions have complete doc comments per CODING_STANDARDS.md §5.1:
  what it does, parameters, return values, lifetime constraints
- Review every comment against the final implementation — update any that
  no longer match exactly. In particular verify:
  - `on_header` callback comment states by-pointer delivery and HIVE_BUF_VALID
  - `send` callback comment states `ssize_t` return and partial-send semantics
  - `hive_submit_goaway_prepare()` and `hive_submit_goaway_final()` documented
  - `HIVE_OK` vs `HIVE_H2_NO_ERROR` distinction noted

**9.6 — Final quality pass**
- `make lint` → zero clang-tidy and cppcheck warnings
- `make test` → all tests pass
- `make valgrind` → clean on Linux
- Full ASan/UBSan run on both platforms
- `grep -r "malloc\|free\|calloc\|realloc" src/ | grep -v null_alloc` →
  zero results
- Update all status cells in phase summary table
- Update all quality milestone statuses

### Phase 9 Completion Criteria

- [ ] h2spec: zero failures — quality milestone M11 confirmed
- [ ] `tools/hive_decode` decodes a captured HTTP/2 stream correctly;
      builds standalone from `hive_frame_bare.c` only
- [ ] README integration example compiles and runs
- [ ] All quality milestones M1–M12 confirmed
- [ ] `make lint` zero warnings
- [ ] All phase status cells updated to DONE

---

## Cross-Reference Index

| Topic | Primary reference | Secondary reference |
|---|---|---|
| Session struct layout (all regions) | ARCHITECTURE.md §2 | — |
| Session lifecycle | ARCHITECTURE.md §1.2 | ARCHITECTURE.md §9.7 |
| Pre-allocation at session creation | ARCHITECTURE.md §11 | CODING_STANDARDS.md §2.3 |
| Post-creation allocations (HPACK, retain) | CODING_STANDARDS.md §2.3 | ARCHITECTURE.md §9.1 |
| `goto cleanup` for session creation | CODING_STANDARDS.md §3.6 | — |
| Receive state machine states | ARCHITECTURE.md §3.1 | — |
| Frame header staging (9 bytes) | ARCHITECTURE.md §2.7 | ARCHITECTURE.md §3.2 |
| Frame header serialisation | ARCHITECTURE.md §6.2 | — |
| hive_frame_bare.c (standalone parser) | REPOSITORY_STRUCTURE.md §3 | TECH_STACK.md §7.8 |
| ctrl_staging accumulation | ARCHITECTURE.md §2.7 | ARCHITECTURE.md §3.3 |
| CONTINUATION lockout (connection error) | ARCHITECTURE.md §3.5 | CODING_STANDARDS.md §4.2 |
| New stream validation | ARCHITECTURE.md §3.4 | — |
| Client-role server preface validation | ARCHITECTURE.md §3.3 | ARCHITECTURE.md §2.7 |
| Fixed-length frame validation | ARCHITECTURE.md §3.3 | — |
| SETTINGS directionality (local vs remote) | ARCHITECTURE.md §2.5 | CODING_STANDARDS.md §3.1 |
| HPACK dynamic table layout | ARCHITECTURE.md §4.1 | — |
| HPACK entry layout (packed alloc) | ARCHITECTURE.md §4.2 | CODING_STANDARDS.md §3.2 |
| HPACK oversized entry rule | ARCHITECTURE.md §4.2 | — |
| HPACK always-copy rule | ARCHITECTURE.md §8.1 | CODING_STANDARDS.md §3.2 |
| HPACK static table (61 entries) | ARCHITECTURE.md §4.3 | — |
| Huffman decode (iterative bit accumulator) | ARCHITECTURE.md §4.4 | — |
| `hpack_decode_block()` loop | ARCHITECTURE.md §4.5 | — |
| Index-0 and out-of-range rejection | ARCHITECTURE.md §4.5 | — |
| Size-update position constraint | ARCHITECTURE.md §4.5 | — |
| HPACK string decode; truncation rejection | ARCHITECTURE.md §4.6 | — |
| Integer varint decode; HPACK_INT_OVERFLOW | ARCHITECTURE.md §4.7 | — |
| HPACK encoder; multiple pending updates | ARCHITECTURE.md §4.8 | — |
| HPACK hash index (large tables) | ARCHITECTURE.md §4.9 | — |
| Stream hash function (Knuth) | ARCHITECTURE.md §5.4 | — |
| Stream open / lookup / close | ARCHITECTURE.md §5.5 | — |
| tombstone_count and closes_since_compact | ARCHITECTURE.md §2.9 | ARCHITECTURE.md §5.5 |
| Hash table compaction | ARCHITECTURE.md §5.6 | — |
| Stream state object (64 bytes) | ARCHITECTURE.md §5.3 | CODING_STANDARDS.md §1.3 |
| Stream state legality table | ARCHITECTURE.md §5.7 | — |
| `frame_hdr_write()` | ARCHITECTURE.md §6.2 | — |
| Control frame queuing | ARCHITECTURE.md §6.3 | — |
| HEADERS frame queuing (no byte-shifting) | ARCHITECTURE.md §6.4 | — |
| DATA frame queuing (uint8_t **buf) | ARCHITECTURE.md §6.5 | ARCHITECTURE.md §9.4 |
| Partial-send drain model (ssize_t) | ARCHITECTURE.md §6.6 | ARCHITECTURE.md §9.8 |
| iovec overflow handling | ARCHITECTURE.md §6.7 | — |
| WINDOW_UPDATE coalescing (recv_window restore) | ARCHITECTURE.md §7.7 | CODING_STANDARDS.md §3.2 |
| Receive-side flow control enforcement | ARCHITECTURE.md §8.7 | CODING_STANDARDS.md §3.2 |
| HPACK bomb protection (stream errors) | ARCHITECTURE.md §8.2 | CODING_STANDARDS.md §4.2 |
| CONTINUATION flood (connection error) | ARCHITECTURE.md §8.3 | CODING_STANDARDS.md §4.2 |
| SETTINGS flood (inbound_settings_count) | ARCHITECTURE.md §8.4 | — |
| RST_STREAM flood + clock abstraction | ARCHITECTURE.md §8.5 | — |
| Stream ID exhaustion (two-phase GOAWAY) | ARCHITECTURE.md §8.6 | — |
| `hive_buf_t` lifetime (by-pointer, ASan) | ARCHITECTURE.md §8.8 | CODING_STANDARDS.md §3.5 |
| data_source contract (uint8_t **buf) | ARCHITECTURE.md §9.4 | — |
| Options table (defaults + ranges) | ARCHITECTURE.md §9.5 | — |
| Callbacks struct (all 12 callbacks) | ARCHITECTURE.md §9.6 | — |
| Two-phase GOAWAY API | ARCHITECTURE.md §9.9 | — |
| hive_submit_trailers / interim_response | ARCHITECTURE.md §9.9 | — |
| MAX_CONCURRENT_STREAMS enforcement | ARCHITECTURE.md §9.10 | — |
| Stream user_data getter/setter | ARCHITECTURE.md §9.11 | — |
| Stream state query | ARCHITECTURE.md §9.11 | — |
| HIVE_OK (library code) vs HIVE_H2_NO_ERROR (wire) | ARCHITECTURE.md §9.13 | CODING_STANDARDS.md §5.4 |
| Memory budget (default options) | ARCHITECTURE.md §11 | — |
| Event loop walkthrough | ARCHITECTURE.md §10 | — |
| NULL-allocator shim location | CODING_STANDARDS.md §2.1 | — |
| Allocator discipline rule | CODING_STANDARDS.md §2 | — |
| `_Static_assert` placement | CODING_STANDARDS.md §1.3 | — |
| SECURITY comment convention | CODING_STANDARDS.md §3.2 | — |
| Connection vs stream error classification | CODING_STANDARDS.md §4.2 | — |
| KNF code style | CODING_STANDARDS.md §1 | — |
| Byte injection test technique | TECH_STACK.md §6.2 | — |
| Callback capture pattern | TECH_STACK.md §6.3 | — |
| ASan poison macros | TECH_STACK.md §7.2 | — |
| h2spec installation and use | TECH_STACK.md §7.7 | — |
| Frame decoder tool | TECH_STACK.md §7.8 | — |
| Source file purposes | REPOSITORY_STRUCTURE.md §3 | — |

REPOSITORY_STRUCTURE.md, TESTING.md
