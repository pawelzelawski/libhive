# Testing Strategy

## 1. Overview

Testing operates at three levels:

| Level | What | When | Tools |
|---|---|---|---|
| Unit | Individual functions in isolation | During each phase, before moving on | Plain C test programs, Valgrind, ASan/UBSan |
| Manual | Live byte injection into sessions | Phases 4–8, verifying end-to-end behaviour | Custom test harness, frame decoder tool |
| Conformance | Full HTTP/2 spec coverage against a real server | Phase 9 | h2spec |

The rule is simple: **a phase is not done until its tests pass on both Linux
and OpenBSD**. Do not accumulate untested code. Each phase is small enough
that bugs are easy to find when caught immediately.

Hive's zero-I/O design makes testing significantly more tractable than
testing a server or client directly. There are no sockets, no TLS stacks,
no threads, and no timing dependencies in the unit test path. A test
constructs raw HTTP/2 bytes, feeds them to `hive_session_recv()`, and
asserts on the callback sequence and session state. This is the primary
testing technique throughout all phases.

**h2spec is not sufficient on its own**. It does not cover: allocator
discipline, arena exhaustion, partial-send behaviour, `NO_COPY` pointer
semantics, standalone HPACK API, clock-abstraction tests for flood
protection, or the `hive_buf_t` by-pointer lifetime contract. All of these
have explicit unit tests. h2spec is the conformance gate; unit tests are the
correctness and security gate.

---

## 2. Unit Testing

### 2.1 Framework

No external framework. Plain C programs with a simple assertion macro.
See TECH_STACK.md §6 for the test harness structure, `RUN()` macro, and
test binary contract.

Every test binary:
- Returns 0 if all tests pass
- Returns 1 if any test fails
- Prints `PASS: test_name` or `FAIL: test_name` per test case
- Prints `N/M tests passed` as the final line

`tests/test_harness.h` defines the `RUN()` macro. `tests/run_tests.c` is
the binary entry point. `tests/run_tests.sh` runs the binary and fails if
it exits non-zero. `make test` invokes `run_tests.sh`.

### 2.2 Byte Injection Technique

The primary unit testing technique for the receive path. See TECH_STACK.md
§6.2 for the `feed_bytes()` helper. All receive path tests use this approach:

```c
/* Construct a raw SETTINGS frame (9-byte header + 6-byte param) */
static const uint8_t settings_frame[] = {
    0x00, 0x00, 0x06,        /* length: 6 */
    0x04,                    /* type: SETTINGS */
    0x00,                    /* flags: none */
    0x00, 0x00, 0x00, 0x00,  /* stream_id: 0 */
    /* INITIAL_WINDOW_SIZE = 131072 */
    0x00, 0x04,              /* param: SETTINGS_INITIAL_WINDOW_SIZE */
    0x00, 0x02, 0x00, 0x00,  /* value: 131072 */
};

feed_bytes(session, settings_frame, sizeof(settings_frame));

/* Assert remote_settings updated */
assert(session->remote_settings.initial_window_size == 131072);
/* Assert SETTINGS ACK queued */
assert(session->send_iov_count == 1);
```

**Split delivery testing** - the most important correctness property of the
receive state machine. Every frame type must be tested with 1-byte delivery:

```c
static void
test_split_feed(hive_session_t *s, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ssize_t n = hive_session_recv(s, data + i, 1);
        if (n < 0)
            return;  /* session error - test will fail on next assert */
    }
}
```

This exercises every state transition in the state machine including the
frame header staging accumulator and the `ctrl_staging` accumulator.

### 2.3 Callback Capture

Tests use a capture struct to record callback arguments. See TECH_STACK.md
§6.3 for the pattern. The capture struct is passed as `user_data` and
populated by test callback implementations.

Verifying callback ordering is equally important as verifying values -
`on_begin_headers` must fire before `on_header`, which must fire before
`on_headers_complete`. Tests assert on both the call count and the order.

```c
/* Verify correct callback sequence for a HEADERS frame */
assert(cap.begin_headers_count == 1);
assert(cap.header_count        == 4);   /* :method, :path, :scheme, :authority */
assert(cap.headers_complete    == 1);
assert(cap.end_stream          == 1);   /* GET request, no body */
/* Verify order: begin_headers fired before first on_header */
assert(cap.begin_headers_seq < cap.first_header_seq);
assert(cap.last_header_seq   < cap.headers_complete_seq);
```

The capture struct for `on_header` must record the `hive_buf_t *` pointer
values (addresses of the library's handle objects), not copies of the data.
This allows tests to verify that the library correctly clears `HIVE_BUF_VALID`
in those objects after callback return.

### 2.4 Partial Send Testing

The send callback in tests must be able to simulate partial writes.
The capture struct includes a field `bytes_to_write` that limits how
many bytes the stub callback reports as written:

```c
typedef struct {
    /* ... other fields ... */
    ssize_t  bytes_to_write;  /* -1 = write all, 0 = fatal error, > 0 = partial */
    ssize_t  bytes_written;   /* total bytes the callback reported writing */
} test_capture_t;

static ssize_t
stub_send(hive_session_t *s, const struct iovec *iov, int iovcnt, void *ud)
{
    test_capture_t *cap = (test_capture_t *)ud;
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += (ssize_t)iov[i].iov_len;

    if (cap->bytes_to_write == 0)
        return -1;                   /* fatal error */
    if (cap->bytes_to_write < 0 || cap->bytes_to_write >= total) {
        cap->bytes_written += total;
        return total;                /* full write */
    }
    cap->bytes_written += cap->bytes_to_write;
    return cap->bytes_to_write;     /* partial write */
}
```

Partial send tests set `bytes_to_write` to a fraction of the total, verify
`hive_session_want_write()` returns 1, then set `bytes_to_write = -1` and
call `hive_session_send()` again to complete the send.

Receive-side backpressure is separately tested: a full output queue causes
`hive_session_recv()` to return the exact unconsumed boundary without invoking
the transport callback. After the caller drains output, re-feeding that input
resumes parsing.

### 2.5 What Gets Unit Tests

Every module in `src/`:
- Frame parser and standalone frame header parser (`hive_frame.c`, `hive_frame_bare.c`)
- HPACK encoder and decoder (`hive_hpack.c`)
- Stream table (`hive_stream.c`)
- Flow control (`hive_flow.c`)
- Send queue (`hive_send.c`)
- Security checks (`hive_security.c`)
- Session lifecycle (`hive.c`)

What does **not** need unit tests:
- The NULL-allocator shim (tested implicitly - every session test with NULL
  allocator exercises it)
- Build system files and config files

See DEVELOPMENT.md for the specific test cases required per phase.

### 2.6 Memory Testing

**After every phase on Linux**:

```sh
make dev
make test           # ASan/UBSan compiled in - catches issues at runtime

make valgrind       # additional coverage
# equivalent to:
valgrind --leak-check=full          \
         --show-leak-kinds=all      \
         --track-origins=yes        \
         --error-exitcode=1         \
         ./tests/run_tests
```

Hive has zero external library dependencies so no Valgrind suppression file
is needed.

**On OpenBSD**: Valgrind is not available. The ASan/UBSan build (`make dev`)
is sufficient.

All code must pass Valgrind clean on Linux and ASan/UBSan clean on both
platforms before a phase is considered complete.

### 2.7 Allocator Discipline Testing

The allocator discipline rule (CODING_STANDARDS.md §2) is verified at two
levels:

**Static check** - run after every phase:
```sh
grep -rn "malloc\|calloc\|realloc\|free" src/ | grep -v "null_alloc"
```
Any match outside the NULL-allocator shim in `hive.c` is a violation and
must be fixed before proceeding.

**Dynamic check** - tracking allocator in unit tests:

```c
typedef struct {
    int   alloc_count;
    int   free_count;
    void *allocs[128];  /* track pointers for leak detection */
} tracking_ctx_t;

static void *
tracking_malloc(size_t size, void *ctx)
{
    tracking_ctx_t *t = (tracking_ctx_t *)ctx;
    void *p = malloc(size);
    if (p != NULL)
        t->allocs[t->alloc_count++] = p;
    return p;
}

static void
tracking_free(void *ptr, void *ctx)
{
    tracking_ctx_t *t = (tracking_ctx_t *)ctx;
    t->free_count++;
    free(ptr);
}

/* Test: create + free with tracking allocator */
static int
test_session_no_leak(void)
{
    tracking_ctx_t t = {0};
    hive_mem_t mem = { tracking_malloc, tracking_free, ... };
    hive_session_t *s = hive_session_server_new(&mem, NULL, &cb, NULL);
    hive_session_free(s);
    return t.alloc_count == t.free_count;  /* no leaks */
}
```

The tracking allocator test runs at Phase 4 and is repeated as a regression
check at every subsequent phase. Note: after Phase 6, HPACK entry allocations
and `hive_buf_retain()` calls contribute to `alloc_count`. The no-leak test
must ensure all such allocations are freed by the end of the test, not just
those from session creation.

### 2.8 ThreadSanitizer

TSAN detects data races. Hive itself has no threads, but the h2spec test
server (`tests/h2spec_server.c`) handles multiple connections and may use
a simple multi-connection model. Run TSAN on the test server binary.

```sh
make test-tsan
```

TSAN and ASan are mutually exclusive - TSAN runs as a separate target on
Linux only. It is not a per-commit gate; run it at phase boundaries and
before each release.

---

## 3. Manual Testing - Live Session Verification

Starting from Phase 4, after a basic session can handle SETTINGS, a
minimal test server can be used to verify behaviour with real HTTP/2 clients
before h2spec is introduced in Phase 9.

### 3.1 Minimal Test Server

`tests/h2spec_server.c` is built progressively through phases 4–8. In its
early form it is a minimal echo server that handles SETTINGS exchange and
responds to HEADERS frames.

```sh
make h2spec-server
./tests/h2spec_server --port 8443 &
SERVER_PID=$!
```

### 3.2 curl Smoke Tests

curl supports HTTP/2 directly. Use these to verify session behaviour
manually during development.

**Verify SETTINGS exchange and basic response**:
```sh
# --http2-prior-knowledge bypasses TLS - for h2c test server
curl -v --http2-prior-knowledge http://127.0.0.1:8080/
# Expected:
#   < HTTP/2 200
#   [response body]

# With TLS (requires test server cert)
curl -v --http2 --insecure https://127.0.0.1:8443/
# Expected: HTTP/2 200
# Verify in verbose output:
#   * ALPN: server accepted h2
#   * Using HTTP2, server supports multiplexing
```

**Verify multiplexed streams**:
```sh
# Open 3 parallel requests - tests concurrent stream handling
curl -v --http2-prior-knowledge            \
     --parallel --parallel-immediate       \
     http://127.0.0.1:8080/one             \
     http://127.0.0.1:8080/two             \
     http://127.0.0.1:8080/three
# Expected: all three return 200
# Verify stream IDs 1, 3, 5 in verbose output
```

**Verify PING keepalive**:
```sh
# curl does not expose PING directly - use the frame decoder instead
# Capture traffic and inspect with tools/hive_decode (see §3.3)
```

**Verify partial send / TLS backpressure**:
```sh
# Use a large response body to test that partial writes are handled:
curl -v --http2-prior-knowledge http://127.0.0.1:8080/large
# While running, watch with ss(8) or netstat to verify the socket
# send buffer fills and drains correctly under backpressure
```

**Verify server push** (Phase 8+):
```sh
curl -v --http2 --insecure https://127.0.0.1:8443/with-push
# Expected: HTTP/2 200 for the main resource
# Verbose output should show:
#   * h2 PUSH_PROMISE (stream=2)
#   * receiving pushed /pushed-asset
```

### 3.3 Frame Decoder Tool

`tools/hive_decode` reads raw HTTP/2 bytes (post-TLS-decryption) and
pretty-prints each frame. Built from `tools/hive_decode.c` +
`src/hive_frame_bare.c` only - no dependency on the session, HPACK, or
stream table. See TECH_STACK.md §7.8 for usage.

```sh
# To capture h2c traffic from the test server:
# 1. Start test server on port 8080 (no TLS)
# 2. Capture with tcpdump
tcpdump -i lo -w /tmp/h2c.pcap port 8080

# 3. Extract TCP payload
tcpflow -r /tmp/h2c.pcap -C > /tmp/raw_h2.bin

# 4. Decode
./tools/hive_decode /tmp/raw_h2.bin
# Expected output:
# --- frame 1 ---
# type:     0x4  SETTINGS
# flags:    0x00
# stream:   0 (connection)
# length:   18 bytes
#   INITIAL_WINDOW_SIZE = 65535
#   MAX_FRAME_SIZE      = 16384
#   MAX_HEADER_LIST_SIZE= 65536
# ...
```

Note: for TLS traffic, TLS decryption must happen before `hive_decode`
can process the bytes. Use the test server's built-in plaintext mode
(h2c, port 8080) for frame-level debugging.

---

## 4. Conformance Testing - h2spec

### 4.1 Purpose

h2spec is the authoritative HTTP/2 conformance suite. It fires RFC-conformant
frame sequences at a running HTTP/2 server and asserts the server responds
correctly per RFC 9113.

h2spec is a mandatory gate for v1. All h2spec tests must pass before Hive v1
is considered complete. There are no planned exceptions.

If h2spec reports a failure, the correct response is:
1. Reproduce the failure with the byte injection technique in unit tests
2. Diagnose against the RFC - which section, which MUST?
3. Fix the library
4. Only if the RFC analysis confirms Hive is correct and h2spec's expectation
   is wrong: document the discrepancy explicitly with the RFC citation

Do not assume h2spec is wrong. The default assumption is that Hive has a bug.

### 4.2 Setup

See TECH_STACK.md §7.7 for h2spec installation instructions.

```sh
# Build the test server
make h2spec-server

# Start it (runs on port 8443 with TLS by default)
./tests/h2spec_server --port 8443 &
SERVER_PID=$!

# Run the full suite
make h2spec
# equivalent to:
h2spec -h 127.0.0.1 -p 8443 --tls --insecure

# Stop the server
kill $SERVER_PID
```

### 4.3 h2spec Test Groups

h2spec covers these RFC 9113 areas. All must pass:

| h2spec group | RFC section | Key scenarios |
|---|---|---|
| `generic/1` | §3 - Starting HTTP/2 | Connection preface, upgrade |
| `generic/2` | §4 - HTTP Frames | Frame format, unknown frames |
| `generic/3` | §4.2 - Frame size | MAX_FRAME_SIZE enforcement |
| `generic/4` | §4.3 - Header compression | HPACK integration |
| `generic/5` | §5.1 - Stream states | State machine transitions |
| `generic/6` | §5.4 - Error handling | Connection vs stream errors |
| `generic/7` | §6 - Frame definitions | All 10 frame types |
| `generic/8` | §6.5 - SETTINGS | Exchange, ACK, flood |
| `generic/9` | §6.7 - PING | Auto-ACK, opaque data |
| `generic/10` | §6.8 - GOAWAY | Graceful shutdown |
| `generic/11` | §6.9 - Flow control | Window management |
| `generic/12` | §7 - Error codes | All wire error codes |
| `hpack/1–5` | RFC 7541 | Encoding, decoding, table management |
| `http2/3.5` | §3.5 - h2c Upgrade | HTTP/1.1 Upgrade path |
| `http2/4–8` | §4–8 | Full HTTP/2 frame semantics |

### 4.4 Interpreting h2spec Output

h2spec prints each test result:

```
  ✓ 3/ 1: Sends a SETTINGS frame as the connection preface
  ✓ 3/ 2: Sends a connection preface with valid client magic
  ✗ 5/ 1: Sends a HEADERS frame
    -> Timeout: an expected response was not received
```

A timeout usually means the server did not send the expected response
within h2spec's deadline. Common causes:
- SETTINGS ACK not queued (check Phase 4 task 4.0 - minimal send queue)
- Response not submitted in the callback (test server bug)
- Send callback not writing to the socket (partial write not being retried)
- `want_write()` not triggering re-registration for write-readiness

An unexpected frame type usually means:
- Wrong error code in RST_STREAM or GOAWAY
- Missing END_STREAM flag
- Wrong stream ID in response
- Connection error sent where a stream error was expected (or vice versa)

### 4.5 Reproducing h2spec Failures in Unit Tests

When h2spec reports a failure, reproduce it as a byte injection unit test
before attempting a fix. This documents the failure as a regression test
and confirms the fix is correct.

```c
/*
 * Reproduces h2spec generic/5.1.1 failure:
 * "Sends a HEADERS frame with an invalid stream ID (even number)"
 * Expected: server sends RST_STREAM with PROTOCOL_ERROR
 */
static int
test_h2spec_stream_id_even_from_client(void)
{
    test_capture_t  cap   = {0};
    hive_session_t *s     = make_test_session_server(&cap);
    uint8_t         frame[9];

    /* Build HEADERS frame with stream_id = 2 (even - invalid from client) */
    frame[0] = 0x00; frame[1] = 0x00; frame[2] = 0x00;  /* length: 0 */
    frame[3] = 0x01;                                      /* type: HEADERS */
    frame[4] = 0x04;                                      /* END_HEADERS */
    frame[5] = 0x00; frame[6] = 0x00; frame[7] = 0x00; frame[8] = 0x02; /* stream=2 */

    feed_bytes(s, frame, sizeof(frame));

    /* Expect connection error (GOAWAY PROTOCOL_ERROR) for even stream from client */
    int result = (s->session_state == HIVE_SESSION_CLOSED) &&
                 cap.goaway_fired &&
                 cap.goaway_error == HIVE_H2_PROTOCOL_ERROR;
    hive_session_free(s);
    return result;
}
```

---

## 5. Security Testing

### 5.1 Flood and Limit Tests

The security tests live in `tests/test_security.c`. Every security limit
defined in ARCHITECTURE.md §8 must have a test. See DEVELOPMENT.md Phase 7
for the full required test list. Summary table with correct error classes:

| Test | Limit under test | Expected outcome | Error class |
|---|---|---|---|
| `test_continuation_flood` | `opt_max_continuation_size` | GOAWAY PROTOCOL_ERROR | **Connection** (RFC 9113 §4.3) |
| `test_push_promise_flood` | `opt_max_continuation_size` | GOAWAY PROTOCOL_ERROR | **Connection** |
| `test_settings_flood` | `inbound_settings_count` | GOAWAY PROTOCOL_ERROR | Connection |
| `test_settings_unsolicited_ack` | `pending_count == 0` check | GOAWAY PROTOCOL_ERROR | Connection |
| `test_hpack_bomb_size` | `opt_max_header_list_size` | RST_STREAM PROTOCOL_ERROR | **Stream** (session continues) |
| `test_hpack_bomb_count` | `opt_max_header_count` | RST_STREAM PROTOCOL_ERROR | **Stream** (session continues) |
| `test_recv_flow_control_stream` | `stream->recv_window` | RST_STREAM FLOW_CONTROL_ERROR | **Stream** |
| `test_recv_flow_control_conn` | `session->recv_window` | GOAWAY FLOW_CONTROL_ERROR | **Connection** |
| `test_rst_stream_flood_callback` | `opt_rst_flood_threshold` | `on_rst_stream_flood` fires | n/a (callback only) |
| `test_rst_stream_flood_window_reset` | rate window expiry | callback not fired after reset | n/a |
| `test_stream_id_exhaustion` | 2^31 - 1000 threshold | GOAWAY NO_ERROR (prepare phase) | Connection |

The error class column is critical. Getting CONTINUATION flood wrong
(RST_STREAM instead of GOAWAY) or HPACK bomb wrong (GOAWAY instead of
RST_STREAM) causes h2spec failures. All security tests must verify the
**correct error class**, not just that an error was returned.

The RST_STREAM flood tests use the clock abstraction (`HIVE_TEST_CLOCK=1`,
`hive_test_clock_secs` global) to avoid sleep-based timing. They are compiled
and registered only by `make test-clock`, so the normal test binary cannot
silently count their no-op fallback as coverage. No security test may use
`sleep()` or `usleep()`.

### 5.2 Malformed Frame Tests

All malformed frame inputs must result in a protocol error, never a crash,
never a memory access violation. The following classes must be covered:

**Oversized frame** (inbound validation uses `local_settings.max_frame_size`):
```c
/* Frame length > local_settings.max_frame_size */
static const uint8_t oversized_frame[] = {
    0xFF, 0xFF, 0xFF,                   /* length: 16777215 */
    0x00,                               /* type: DATA */
    0x00,                               /* flags: none */
    0x00, 0x00, 0x00, 0x01,             /* stream_id: 1 */
};
/*
 * Expected: FRAME_SIZE_ERROR connection error - do not attempt to read payload.
 * Verify using local_settings.max_frame_size (not remote_settings).
 */
feed_bytes(session, oversized_frame, sizeof(oversized_frame));
assert(session->session_state == HIVE_SESSION_CLOSED);
```

**Fixed-length frame violations** - complete matrix per ARCHITECTURE.md §3.3:

| Frame type | Required length | Wrong length to test | Expected error |
|---|---|---|---|
| SETTINGS with ACK | 0 | 6 (one param) | FRAME_SIZE_ERROR |
| SETTINGS without ACK | multiple of 6 | 5 (not multiple of 6) | FRAME_SIZE_ERROR |
| PING | 8 | 4 | FRAME_SIZE_ERROR |
| RST_STREAM | 4 | 5 | FRAME_SIZE_ERROR |
| WINDOW_UPDATE | 4 | 3 | FRAME_SIZE_ERROR |
| PRIORITY | 5 | 4 | FRAME_SIZE_ERROR |
| GOAWAY | >= 8 | 4 | FRAME_SIZE_ERROR |
| PUSH_PROMISE (no PADDED) | >= 4 | 3 | FRAME_SIZE_ERROR |
| PUSH_PROMISE (PADDED) | >= 5 | 4 | FRAME_SIZE_ERROR |

All of these are connection errors (GOAWAY FRAME_SIZE_ERROR). Build one
test case per row, feed the malformed frame, verify session CLOSED.

**Stream ID violations**:
- DATA on stream 0 → connection error PROTOCOL_ERROR
- SETTINGS with non-zero stream_id → connection error PROTOCOL_ERROR
- PRIORITY with stream_id 0 → connection error PROTOCOL_ERROR (RFC 9113 §6.3)
- HEADERS with even stream_id from client → connection error PROTOCOL_ERROR
- HEADERS with stream_id lower than previous → connection error PROTOCOL_ERROR
- CONTINUATION with wrong stream_id → connection error PROTOCOL_ERROR

**HPACK violations** (all result in COMPRESSION_ERROR connection error
except bomb limits which are stream errors):
- Invalid Huffman sequence (EOS in non-terminal position) → COMPRESSION_ERROR
- Truncated varint (runs out of input bytes mid-continuation) → COMPRESSION_ERROR
- Varint overflow (m > 28) → COMPRESSION_ERROR
- Indexed representation with index = 0 → COMPRESSION_ERROR
- Index beyond static + dynamic table size → COMPRESSION_ERROR
- Dynamic table size update after first header field → COMPRESSION_ERROR
- String length claims more bytes than remain in block → COMPRESSION_ERROR

**Padding violations**:
- PADDED flag set, `pad_length >= frame payload length` → connection error

For each of these, the test feeds the malformed bytes and asserts:
1. The session returns a negative error code from `hive_session_recv()`, or
2. The session transitions to CLOSED state

No test should result in an assertion failure, crash, or AddressSanitizer
report. Any such result is a bug, not a test failure.

### 5.3 PUSH_PROMISE Flood Tests

PUSH_PROMISE reassembly uses the same `reassembly_buf` as HEADERS
reassembly and is subject to the same flood cap. Tests must verify this
parallel attack surface explicitly:

```c
/*
 * PUSH_PROMISE flood: send PUSH_PROMISE without END_HEADERS
 * then repeated CONTINUATION frames exceeding opt_max_continuation_size.
 * Expected: GOAWAY PROTOCOL_ERROR (same as CONTINUATION flood for HEADERS).
 */
static int
test_push_promise_flood(void)
{
    test_capture_t  cap = {0};
    hive_session_t *s   = make_test_session_client(&cap);
    /* ... build and feed oversized PUSH_PROMISE+CONTINUATION sequence ... */
    assert(s->session_state == HIVE_SESSION_CLOSED);
    assert(cap.goaway_fired);
    hive_session_free(s);
    return cap.goaway_error == HIVE_H2_PROTOCOL_ERROR;
}
```

### 5.4 SETTINGS Directionality Tests

Verify that the SETTINGS direction rules are correctly implemented:

- `test_inbound_frame_validated_against_local_settings`: set
  `local_settings.max_frame_size = 100`; send a frame of length 101 →
  FRAME_SIZE_ERROR connection error. Verify the check uses `local_settings`,
  not `remote_settings`.

- `test_outbound_frame_respects_remote_max_frame_size`: set
  `remote_settings.max_frame_size = 100`; submit a response with a 200-byte
  body; verify the DATA frame in the send queue has length 100 (capped by
  peer's advertised limit).

- `test_settings_header_table_size_constrains_encoder`: feed a SETTINGS frame
  from peer with `HEADER_TABLE_SIZE = 512`; verify `enc_table.pending_max = 512`
  and `enc_table.has_pending = 1`; verify `dec_table.max_size` is unchanged
  (peer's HEADER_TABLE_SIZE constrains our encoder, not our decoder).

### 5.5 Receive-Side Flow Control Violation Tests

```c
/*
 * Stream-level: peer sends more DATA than stream->recv_window allows.
 * Expected: RST_STREAM FLOW_CONTROL_ERROR (stream error - session continues).
 */
static int
test_recv_exceeds_stream_window(void)
{
    test_capture_t  cap = {0};
    hive_session_t *s   = make_test_session_server(&cap);
    hive_stream_t  *st;

    /* Open stream 1 and set its recv_window to 10 bytes */
    open_test_stream(s, 1);
    st = stream_lookup(s, 1);
    st->recv_window = 10;

    /* Feed 11 bytes of DATA */
    feed_data_frame(s, 1, 11);

    /* Expected: RST_STREAM FLOW_CONTROL_ERROR; session still OPEN */
    assert(s->session_state == HIVE_SESSION_OPEN);
    assert(cap.rst_stream_fired);
    assert(cap.rst_error == HIVE_H2_FLOW_CONTROL_ERROR);
    hive_session_free(s);
    return 1;
}
```

Similar test for connection-level violation expects GOAWAY (connection error).

### 5.6 Always-Copy Verification

The HPACK dynamic table always-copy rule (ARCHITECTURE.md §8.1) must be
verified as a property, not just as a code review.

```c
static int
test_hpack_always_copy(void)
{
    /* Insert a header using a stack-allocated name buffer */
    uint8_t name_buf[16];
    memcpy(name_buf, "x-custom-header", 15);

    hpack_table_insert(&table, name_buf, 15,
        (uint8_t *)"value", 5, alloc, ctx);

    /* Overwrite the original buffer after insertion */
    memset(name_buf, 0xFF, sizeof(name_buf));

    /* The table entry must still contain the correct name */
    hpack_lookup_result_t r = hpack_table_lookup(&table, ...);
    return memcmp(HPACK_ENTRY_NAME(r.entry), "x-custom-header", 15) == 0;
}
```

This test verifies that the table holds a copy, not a pointer to the
original buffer. The original is deliberately corrupted after insertion to
make the distinction unambiguous.

### 5.7 hive_buf_t Lifetime Verification

`on_header` passes `hive_buf_t *name` and `hive_buf_t *value` - pointers
to library-owned handle objects. After the callback returns, the library
clears `HIVE_BUF_VALID` in those objects.

**Automated test** (tests the flag clearing, runs in all builds):

```c
static hive_buf_t *saved_name_ptr;  /* pointer to the library's handle */
static uint32_t    post_callback_flags;

static int
capture_on_header(hive_session_t *s, uint32_t sid,
    hive_buf_t *name, hive_buf_t *value, uint8_t flags, void *ud)
{
    saved_name_ptr = name;  /* save the pointer to the library's object */
    return HIVE_OK;
}

static int
test_hive_buf_valid_cleared_after_callback(void)
{
    /* ... set up session with capture_on_header ... */
    /* ... feed a HEADERS frame ... */

    /* After feed_bytes() returns, the callback has completed.
     * The library must have cleared HIVE_BUF_VALID in the handle. */
    post_callback_flags = saved_name_ptr->flags;
    return !(post_callback_flags & HIVE_BUF_VALID);
}
```

**ASan poisoning verification** (manual, `HIVE_DEBUG` builds only):
In Linux ASan `HIVE_DEBUG` builds, ephemeral header storage is poisoned after
callback return. Scratch storage is unpoisoned before a later internal reuse,
so this is a diagnostic aid rather than a durable stale-pointer detector.
Verify the immediate post-callback case manually during Phase 7:

1. Build with `make dev` (`-fsanitize=address -DHIVE_DEBUG=1`)
2. In `capture_on_header`, save `name->data` (not the pointer to the struct,
   but the pointer to the raw bytes)
3. Access that raw pointer after `feed_bytes()` returns
4. For a non-indexed Huffman-decoded header (decoded into `hpack_scratch_name`
   or `hpack_scratch_value`), verify ASan reports `heap-use-after-poison`
5. For a non-Huffman header (pointer into `reassembly_buf`), same result
6. For an indexed header (pointer into static table), verify no ASan abort -
   static table data must NOT be poisoned

Document both results as comments in `tests/test_security.c`.

### 5.8 Callback Error Handling Tests

Callbacks may return `HIVE_ERR_*`. The library must handle these correctly:

- `on_begin_headers` returns `HIVE_ERR_NOMEM` → library suppresses all
  `on_header` delivery for this block but continues consuming and applying
  dynamic table updates until the full block is decoded (RFC 7541 §2.3.2 -
  the connection-scoped dynamic table must remain consistent); RST_STREAM
  is sent after the block is fully consumed; session continues
- `on_header` returns `HIVE_ERR_PROTOCOL` → same behaviour: remaining
  entries are decoded (dynamic table updates applied) but not delivered;
  RST_STREAM sent after block is fully consumed; session continues
- `on_headers_complete` returns error → RST_STREAM for this stream;
  session continues
- `on_data_chunk` returns error → send RST_STREAM for this stream;
  session continues

Implement one test per callback for error return handling. Each test verifies:
1. The session remains open (stream error, not connection error)
2. The correct RST_STREAM error code is sent
3. For `on_begin_headers` and `on_header` errors: subsequent `hive_session_recv()`
   calls with new HEADERS frames decode correctly (confirming the dynamic table
   was not left in an inconsistent state)

---

## 6. Test Coverage Tracking

Track coverage manually. Update after each phase. A cell is marked done
only when the test passes cleanly with no Valgrind or ASan errors on both
platforms.

### Unit Test Coverage

| Module | Test file | Regression location |
|---|---|---|
| hive_frame_bare (standalone header parser) | test_frame.c | frame parser tests |
| HPACK (static + Huffman tables) | test_hpack.c | HPACK tests |
| HPACK (full encode/decode) | test_hpack.c | HPACK tests |
| Frame parser | test_frame.c | frame and wire-driven security tests |
| Stream table | test_session.c | session tests |
| Flow control | test_flow.c | flow-control tests |
| Session lifecycle | test_session.c | session tests |
| Security limits | test_security.c | security tests; RST clock cases use `make test-clock` |

### Key Test Cases

| Test case | File | Reference |
|---|---|---|
| RFC 7541 §C.3 decode vector (no Huffman) | test_hpack.c | ARCHITECTURE.md §4.5 |
| RFC 7541 §C.4 decode vector (Huffman) | test_hpack.c | ARCHITECTURE.md §4.4 |
| RFC 7541 §C.6 response decode vector | test_hpack.c | ARCHITECTURE.md §4.5 |
| Always-copy after buffer overwrite | test_hpack.c | ARCHITECTURE.md §8.1 |
| HPACK bomb: size limit (stream error, session continues) | test_hpack.c | ARCHITECTURE.md §8.2 |
| HPACK bomb: count limit (stream error) | test_hpack.c | ARCHITECTURE.md §8.2 |
| HPACK index-0 rejection | test_hpack.c | ARCHITECTURE.md §4.5 |
| HPACK out-of-range index rejection | test_hpack.c | ARCHITECTURE.md §4.5 |
| HPACK size-update after first header field | test_hpack.c | ARCHITECTURE.md §4.5 |
| HPACK truncated varint | test_hpack.c | ARCHITECTURE.md §4.7 |
| HPACK truncated string | test_hpack.c | ARCHITECTURE.md §4.6 |
| HPACK oversized entry: table empties, no insert | test_hpack.c | ARCHITECTURE.md §4.2 |
| Split delivery: 1-byte feed for all frame types | test_frame.c | ARCHITECTURE.md §3.3 |
| Frame length uses local_settings (not remote) | test_frame.c | ARCHITECTURE.md §2.5 |
| Fixed-length frame matrix (all 9 frame type constraints) | test_frame.c | ARCHITECTURE.md §3.3 |
| CONTINUATION lockout: connection error (GOAWAY) | test_frame.c | ARCHITECTURE.md §3.5 |
| CONTINUATION lockout: zero-length HEADERS still requires CONTINUATION | test_frame.c | ARCHITECTURE.md §3.5 |
| PRIORITY stream_id=0: connection error PROTOCOL_ERROR | test_frame.c | ARCHITECTURE.md §3.3 |
| SETTINGS non-ACK non-multiple-of-6: FRAME_SIZE_ERROR | test_frame.c | ARCHITECTURE.md §3.3 |
| PUSH_PROMISE minimum length: FRAME_SIZE_ERROR | test_frame.c | ARCHITECTURE.md §3.3 |
| Client preface: first frame must be SETTINGS | test_session.c | ARCHITECTURE.md §3.3 |
| Stream ID monotonicity violation | test_session.c | ARCHITECTURE.md §3.4 |
| Session create + free: zero outstanding allocations | test_session.c | CODING_STANDARDS.md §2.3 |
| Allocation failure mid-create: zero leaks | test_session.c | CODING_STANDARDS.md §3.6 |
| Partial send: resume on next hive_session_send() | test_session.c | ARCHITECTURE.md §6.6 |
| SETTINGS directionality: inbound uses local | test_session.c | ARCHITECTURE.md §2.5 |
| SETTINGS directionality: outbound uses remote | test_session.c | ARCHITECTURE.md §2.5 |
| SETTINGS_HEADER_TABLE_SIZE constrains encoder | test_session.c | ARCHITECTURE.md §2.5 |
| WINDOW_UPDATE coalescing: recv_window restored | test_flow.c | ARCHITECTURE.md §7.7 |
| DATA zero-copy: pointer in caller's buffer | test_flow.c | ARCHITECTURE.md §7.1 |
| Receive flow control: stream violation (RST) | test_flow.c | ARCHITECTURE.md §8.7 |
| Receive flow control: connection violation (GOAWAY) | test_flow.c | ARCHITECTURE.md §8.7 |
| send callback fires once per hive_session_send() | test_session.c | ARCHITECTURE.md §6.6 |
| NO_COPY: *buf redirect points to caller memory | test_session.c | ARCHITECTURE.md §6.5 |
| NO_COPY: caller memory valid until want_write() returns 0 | test_session.c | ARCHITECTURE.md §9.4 |
| Content-Length mismatch on END_STREAM: RST_STREAM PROTOCOL_ERROR | test_security.c | ARCHITECTURE.md §5.3 |
| Full GET request-response round-trip | test_session.c | ARCHITECTURE.md §10 |
| on_settings_ack fires on SETTINGS ACK received | test_session.c | ARCHITECTURE.md §9.6 |
| on_goaway fires with correct last_stream_id | test_session.c | ARCHITECTURE.md §9.6 |
| on_ping fires when auto-ACK disabled | test_session.c | ARCHITECTURE.md §9.6 |
| on_ping_ack fires on PING ACK received | test_session.c | ARCHITECTURE.md §9.6 |
| on_connection_error fires before GOAWAY queued | test_session.c | ARCHITECTURE.md §9.6 |
| CONTINUATION flood: GOAWAY (connection error) | test_security.c | ARCHITECTURE.md §8.3 |
| PUSH_PROMISE flood: GOAWAY (connection error) | test_security.c | ARCHITECTURE.md §8.3 |
| SETTINGS flood: queued ACK limit and full-send release | test_security.c, test_session.c | ARCHITECTURE.md §8.4 |
| RST_STREAM flood: callback at threshold (clock mock) | test_security.c | ARCHITECTURE.md §8.5 |
| RST_STREAM flood: window reset via clock mock | test_security.c | ARCHITECTURE.md §8.5 |
| Stream ID exhaustion: goaway_prepare triggered | test_security.c | ARCHITECTURE.md §8.6 |
| Oversized frame: FRAME_SIZE_ERROR, no crash | test_security.c | ARCHITECTURE.md §3.3 |
| Fixed-length frame violations: full matrix | test_security.c | ARCHITECTURE.md §3.3 |
| hive_buf_t: HIVE_BUF_VALID cleared after callback | test_security.c | ARCHITECTURE.md §8.8 |
| Callback error returns: stream error, session continues | test_security.c | ARCHITECTURE.md §9.6 |
| Callback error: dynamic table consistent after suppressed block | test_security.c | ARCHITECTURE.md §4.5 |
| h2c Upgrade: feed_upgrade_headers fires callbacks | test_session.c | ARCHITECTURE.md §9.7 |
| Two-phase GOAWAY: prepare then final | test_session.c | ARCHITECTURE.md §9.9 |
| Client role: stream IDs 1, 3, 5 | test_session.c | ARCHITECTURE.md §9.10 |
| Client role: MAX_CONCURRENT_STREAMS enforced | test_session.c | ARCHITECTURE.md §9.10 |
| Stream user_data setter/getter | test_session.c | ARCHITECTURE.md §9.11 |
| Stream state query | test_session.c | ARCHITECTURE.md §9.11 |
| GOAWAY recv: want_read advisory, no crash on continued read | test_session.c | ARCHITECTURE.md §9.8 |
| GOAWAY recv: only locally-initiated streams above last_stream_id closed | test_session.c | ARCHITECTURE.md §3.3 |
| Fragmented PUSH_PROMISE + CONTINUATION fires on_push_promise (not on_headers_complete) | test_session.c | ARCHITECTURE.md §3.3 |
| DATA on idle stream (parity-aware): connection error; DATA on closed stream: RST_STREAM | test_frame.c | ARCHITECTURE.md §3.3 |
| Content-Length mismatch on HEADERS+END_STREAM (no DATA): RST_STREAM PROTOCOL_ERROR | test_security.c | ARCHITECTURE.md §3.3, §5.3 |
| RESERVED_REMOTE + HEADERS without END_STREAM: stream → half-closed(local) | test_session.c | ARCHITECTURE.md §3.3, §5.7 |
| RESERVED_REMOTE + HEADERS with END_STREAM: stream → closed, on_stream_close fires | test_session.c | ARCHITECTURE.md §3.3, §5.7 |
| Fragmented push-response HEADERS via CONTINUATION: correct RESERVED_REMOTE transition | test_session.c | ARCHITECTURE.md §3.3 |
| DATA on reserved stream: connection error PROTOCOL_ERROR (not RST_STREAM STREAM_CLOSED) | test_frame.c | ARCHITECTURE.md §3.3, §5.7 |
| WINDOW_UPDATE on idle stream (parity-aware): connection error PROTOCOL_ERROR | test_frame.c | ARCHITECTURE.md §3.3 |
| RST_STREAM on idle stream (parity-aware): connection error PROTOCOL_ERROR | test_frame.c | ARCHITECTURE.md §3.3 |
| HPACK string truncation: connection error COMPRESSION_ERROR (not stream error) | test_security.c | ARCHITECTURE.md §4.6 |
| HPACK string-too-long: connection error COMPRESSION_ERROR (not stream error) | test_security.c | ARCHITECTURE.md §4.6 |
| HPACK encoder two-update: pending_min emitted first; eviction applied to pending_min then pending_max | test_hpack.c | ARCHITECTURE.md §4.8 |
| Trailer sequence: read_callback returns 0/no-EOF → data_source cleared → hive_submit_trailers() sends HEADERS+END_STREAM | test_session.c | ARCHITECTURE.md §6.5, §9.9 |
| on_stream_close fires at queue time (before transmission) for locally-sent END_STREAM | test_session.c | ARCHITECTURE.md §6.5, §9.11 |
| SETTINGS_MAX_CONCURRENT_STREAMS: peer change applied; subsequent hive_submit_request() honors new limit | test_session.c | ARCHITECTURE.md §3.3 |
| HEADERS on HALF_CLOSED_REMOTE: full HPACK decode with suppressed callbacks; RST_STREAM after END_HEADERS | test_frame.c | ARCHITECTURE.md §3.3, §4.5 |
| HEADERS on RESERVED_LOCAL: full HPACK decode with suppressed callbacks; RST_STREAM after END_HEADERS | test_frame.c | ARCHITECTURE.md §3.3, §4.5 |
| Fragmented illegal-state HEADERS: reassembly_stream_error_code carried to CONTINUATION END_HEADERS | test_frame.c | ARCHITECTURE.md §3.3 |
| HEADERS on RESERVED_LOCAL without END_STREAM: RESERVED_LOCAL → HALF_CLOSED_REMOTE transition | test_session.c | ARCHITECTURE.md §6.4, §5.7 |
| HEADERS on RESERVED_LOCAL with END_STREAM: RESERVED_LOCAL → CLOSED; on_stream_close fires at queue time | test_session.c | ARCHITECTURE.md §6.4, §5.7 |
| RST_STREAM on recently-closed stream: flood counter incremented (attacker cannot evade rate limiter) | test_security.c | ARCHITECTURE.md §8.5 |
| WINDOW_UPDATE zero-increment: returns after error; does not apply zero to window | test_frame.c | ARCHITECTURE.md §3.3 |
| DATA send path: send windows decremented before stream_close (no use-after-free) | test_session.c | ARCHITECTURE.md §6.5 |
| PUSH_PROMISE: peer_stream_open_count and stream_open_count checked before slot allocation; excess → RST_STREAM REFUSED_STREAM on promised stream | test_session.c | ARCHITECTURE.md §3.3 |
| PUSH_PROMISE: HPACK decode error sends RST_STREAM on promised stream (not carrying stream); carrying stream continues | test_frame.c | ARCHITECTURE.md §3.3, §4.5 |
| §7.7 recv_window restoration: overflow check fires FLOW_CONTROL_ERROR if recv_window + recv_consumed > 2^31-1 | test_flow.c | ARCHITECTURE.md §7.7 |

### Conformance Test Coverage

The pinned h2spec v2.6.0 gate is run against `tests/h2spec_server.c` on Linux
amd64 when libtls is available. Its output must report zero failed tests;
total and skipped counts are not release evidence because they are tool-output
details rather than a stable project contract. See TECH_STACK.md §7.7 for the
fixture and dependency requirements.

---

## 7. Cross-Reference

| Topic | Reference |
|---|---|
| Unit test framework and pattern | TECH_STACK.md §6 |
| Test harness header (RUN macro) | TECH_STACK.md §6.1 |
| Byte injection helper (`feed_bytes`) | TECH_STACK.md §6.2 |
| Callback capture pattern | TECH_STACK.md §6.3 |
| Partial send stub callback pattern | TESTING.md §2.4 |
| Per-phase test tasks and specific cases | DEVELOPMENT.md (each phase) |
| ASan poison macros (`HIVE_ASAN_POISON`) | TECH_STACK.md §7.2 |
| Clock abstraction for flood tests | ARCHITECTURE.md §8.5 |
| h2spec installation and usage | TECH_STACK.md §7.7 |
| Frame decoder tool (`hive_decode`) | TECH_STACK.md §7.8 |
| Valgrind and ASan setup | TECH_STACK.md §7.1, §7.2 |
| HPACK bomb limits (stream errors) | ARCHITECTURE.md §8.2 |
| CONTINUATION flood (connection error) | ARCHITECTURE.md §8.3 |
| SETTINGS flood (`inbound_settings_count`) | ARCHITECTURE.md §8.4 |
| RST_STREAM flood rate counter + clock | ARCHITECTURE.md §8.5 |
| Stream ID exhaustion threshold | ARCHITECTURE.md §8.6 |
| Receive-side flow control enforcement | ARCHITECTURE.md §8.7 |
| Buffer lifetime contract (by-pointer) | ARCHITECTURE.md §8.8 |
| Always-copy rule | ARCHITECTURE.md §8.1 |
| SETTINGS directionality | ARCHITECTURE.md §2.5 |
| Fixed-length frame validation | ARCHITECTURE.md §3.3 |
| Connection vs stream errors | CODING_STANDARDS.md §4.2 |
| Allocator discipline | CODING_STANDARDS.md §2 |
| Zero malloc in src/ check | CODING_STANDARDS.md §2.1 |
| SECURITY comment convention | CODING_STANDARDS.md §3.2 |
| `_Static_assert` size checks | CODING_STANDARDS.md §1.3 |
| Event loop walkthrough (round-trip ref) | ARCHITECTURE.md §10 |
| Source file purposes | REPOSITORY_STRUCTURE.md §3 |

**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md
