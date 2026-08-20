# Hive

## Overview

Hive is a standalone HTTP/2 library written in C11 for 64-bit POSIX Linux and
OpenBSD targets.
It implements the full HTTP/2 protocol - binary framing, stream multiplexing,
HPACK header compression, flow control, and stream state management - as a
pure protocol engine. It performs zero I/O. The caller feeds bytes in, the
library processes them and fires callbacks, and the caller writes the resulting
output bytes to the network. Hive integrates into any existing event loop
without conflict.

The library supports both client and server roles in v1. Any developer can
embed it to build an HTTP/2 server, client, proxy, load balancer, or any
other component that speaks HTTP/2. Wraith, a companion HTTP server, uses
Hive for its HTTP/2 transport, but Hive is an independent library with no
dependency on Wraith.

The design philosophy is the same as the rest of this author's infrastructure
tooling: zero application-level dependencies, explicit memory ownership,
defensible security properties, first-class OpenBSD support, and code that
can be audited, understood, and trusted.

## Philosophy

### Core Principles

1. **Zero I/O**: Hive does not touch sockets, file descriptors, or TLS. It
   does not own an event loop. The caller reads bytes from the network and
   feeds them to `hive_session_recv()`. The caller calls `hive_session_send()`
   to drain serialised frames and writes the result to the network. This is
   not a limitation - it is the design. A library that owns I/O cannot be
   embedded cleanly in an existing event loop. Hive works in any host
   application - epoll, kqueue, io_uring, test harnesses, proxies - without
   modification.

2. **Zero Dependencies**: No external libraries. No TLS stack, no allocator
   library, no protocol helpers. Everything is implemented from the
   authoritative specifications: RFC 9113 for HTTP/2, RFC 7541 for HPACK.
   The zero-dependency requirement is non-negotiable - it is what makes Hive
   auditable, portable, and embeddable without dependency management.

3. **Explicit Memory Ownership**: The allocator is injected at session
   creation. Pass NULL for system malloc. Pass a custom allocator - an arena,
   a pool, a region - and Hive uses it for every internal allocation without
   exception. All structural per-session memory (stream table, send buffers,
   reassembly buffer, HPACK scratch) is pre-allocated at session creation.
   Two categories of allocation occur after creation during normal operation:
   HPACK dynamic table entries (one allocation per inserted header, freed on
   eviction) and `hive_buf_retain()` calls (one allocation per retained
   header value). Both go through the session allocator. With a
   connection-scoped arena, these are free-list pops, not system malloc calls.
   The allocator model is not a hook added for flexibility - it is the
   architecture.

4. **Security in the Design**: HPACK bomb protection, CONTINUATION flood
   protection, SETTINGS flood protection, RST_STREAM flood detection, and
   stream ID exhaustion handling are not optional hardening steps applied
   after the fact. They are in the protocol processing path from the first
   implementation, with configurable limits exposed through the options API.
   A library that cannot defend against known HTTP/2 attack vectors is not
   fit for production use.

5. **Explicit Lifetime Contracts**: Every pointer delivered to a callback
   has a documented lifetime, enforced in debug builds. Header name/value
   pairs are delivered via `hive_buf_t` handles (passed by pointer to the
   callback) that invalidate on callback return. DATA payload pointers are
   valid only within `on_data_chunk`. ASan builds poison invalidated regions
   immediately. The API surface makes lifetime decisions visible rather than
   burying them in prose.

6. **Write Coalescing by Design**: The drain model means `hive_session_send()`
   fires the send callback once per call with a batched scatter-gather iovec.
   SETTINGS ACK, WINDOW_UPDATE, HEADERS, and DATA all go out in a single
   `writev` or TLS write regardless of how many frames are queued. This is
   not an optimisation layered on top - it is how the send path works from
   the start.

7. **Boring Technology**: Open-addressed hash table for stream lookup, linear
   scan for HPACK table lookup at default sizes, 256-entry Huffman decode
   table with iterative bit accumulator, ring buffer for dynamic table
   eviction. All well-understood, all implementable correctly in auditable C.
   Nothing experimental, nothing clever.

## Problem Statement

Most HTTP/2 implementations in C either:

- Perform their own I/O, which makes them impossible to embed cleanly in an
  existing event loop without fighting the library's threading or I/O model
- Use a fixed allocator (system malloc), which prevents zero-malloc hot paths
  and forces every connection to pay the cost of individual allocations
- Conflate TLS with HTTP/2, making it impossible to use the library with a
  different TLS stack or on a cleartext internal network
- Expose HPACK bomb and CONTINUATION flood vulnerabilities because security
  hardening was added reactively rather than designed in

Hive makes different assumptions. The caller owns I/O, owns TLS, owns the
event loop, and owns the allocator. The library owns the protocol. This
separation is clean, complete, and allows Hive to be embedded in contexts
the author did not anticipate.

## Goals

### Primary Goals

1. **Full RFC 9113 Compliance**: All 10 frame types, all stream states, the
   full connection preface sequence, SETTINGS negotiation, flow control at
   both connection and stream level, server push, two-phase GOAWAY, and h2c
   cleartext including the HTTP/1.1 Upgrade path. Compliance verified against
   h2spec.

2. **Full RFC 7541 Compliance**: Static table, dynamic table, Huffman
   encode and decode, all four header field representations, dynamic table
   size updates mid-connection. HPACK also exposed as a standalone public API
   for callers who need header compression without a full session.

3. **Zero I/O, Zero TLS**: The library processes bytes. The caller moves
   them. This invariant must hold in every code path - no hidden socket
   operations, no internal threads, no blocking calls.

4. **Configurable Allocator**: Every internal allocation goes through the
   injected allocator interface. No direct `malloc` calls anywhere in the
   library source. This is a non-negotiable coding standard, enforced as
   strictly as code style.

5. **Client and Server Roles**: Both roles are supported in v1. The
   marginal implementation cost is low and the value for developers building
   clients, proxies, and load balancers is high. Sessions are created with
   an explicit role; the protocol processing is symmetric where the spec
   allows it.

6. **Defence Against Known Attack Vectors**: HPACK bomb (decoded size limit
   + header count limit, enforced incrementally), CONTINUATION flood (hard
   cap on reassembly buffer before HPACK decode, connection error on
   violation), SETTINGS flood (fixed-size inbound pending queue, GOAWAY on
   violation), RST_STREAM flood (rate counter with caller callback), stream
   ID exhaustion (automatic GOAWAY near 2^31). All limits configurable, all
   defaults safe for production deployment.

7. **Write Coalescing**: One writev or TLS write per event loop iteration
   regardless of frame count. The scatter-gather send model is the
   architecture, not a tuning option.

8. **Zero-Copy Body Path**: Response bodies can be delivered to the send
   callback without copying, via `HIVE_DATA_FLAG_NO_COPY`. The iovec entry
   points directly into caller-owned memory. This is how Wraith achieves
   zero-copy file serving over HTTP/2.

### Non-Goals

1. **I/O Ownership**: Hive will never own a socket, file descriptor, or
   thread. Callers that want I/O management built into the library should
   use a different library.

2. **TLS**: TLS is the caller's responsibility. Hive sees plaintext bytes
   only. Use libtls, OpenSSL, BoringSSL, or any other TLS stack - Hive
   does not care.

3. **HTTP/3 and QUIC**: Permanently out of scope. Different transport layer,
   different framing model, different library.

4. **WebSocket over HTTP/2 (RFC 8441)**: Planned for v2. Extended CONNECT
   and WebSocket tunnelling over HTTP/2 streams are additive scope that does
   not block v1.

5. **Extensible Priorities (RFC 9218)**: Planned for v2. Not blocking v1.
   PRIORITY frames (deprecated by RFC 9113) are accepted and silently
   ignored.

6. **Dynamic Configuration**: Options are set once at session creation.
   There is no mechanism to change option values on a live session. This is
   intentional - live reconfiguration of protocol limits introduces edge
   cases that are difficult to reason about.

7. **Thread Safety on a Single Session**: A session is not thread-safe.
   The caller is responsible for ensuring that `hive_session_recv()`,
   `hive_session_send()`, `hive_submit_*()`, and `hive_session_free()` are
   never called concurrently on the same session. The intended model is one
   session per connection, one connection owned by one event loop thread.

## Architecture Overview

See ARCHITECTURE.md for full detail. The model in one paragraph:

The caller creates a session per connection, passing an allocator, options,
a callback struct, and a user data pointer. On the receive side, the caller
reads bytes from the network and passes them to `hive_session_recv()`. The
library processes as many bytes as it can, firing callbacks as frames are
parsed, and returns the number of bytes consumed. On the send side, the
caller calls `hive_session_send()` once per event loop iteration after all
recv and submit calls. The library fires the `send` callback once with a
batched scatter-gather iovec covering every frame queued during that
iteration. The callback returns the number of bytes it actually wrote; the
library retains any unsent tail for the next call.

```
caller reads bytes from network
    │
    ▼
hive_session_recv(session, buf, n)
    │
    ├── fires on_begin_headers / on_header / on_headers_complete
    ├── fires on_data_chunk  (pointer into caller's buf - zero copy)
    ├── fires on_stream_close / on_settings / on_goaway / on_ping / etc.
    └── queues SETTINGS ACK / WINDOW_UPDATE / error responses internally

caller calls hive_submit_response(session, stream_id, headers, &data_source)
    │
    └── queues HEADERS + DATA frames internally

hive_session_send(session)
    │
    └── fires send(session, iov, iovcnt, user_data) - once per call
            │
            └── returns bytes_written; library retains unsent tail
                iov covers: SETTINGS ACK + WINDOW_UPDATE +
                            HEADERS + DATA header + DATA body (zero copy)

caller calls writev / tls_write with the iovec
```

All structural session memory is pre-allocated at `hive_session_*_new()`.
With a connection-scoped arena allocator (the Wraith model), this is one
`malloc` at connection start and one `free` at connection close.

## Scope

### v1 (This Implementation)

**Included:**

- Full RFC 9113 frame parsing and serialisation (all 10 frame types)
- Full RFC 7541 HPACK: static table, dynamic table, Huffman encode/decode
- HPACK standalone public API (encoder and decoder independently usable)
- Client role and server role, both in v1
- Connection preface (client and server)
- SETTINGS negotiation, ACK, and all SETTINGS-derived limit enforcement
- Flow control: connection-level and stream-level, WINDOW_UPDATE coalescing,
  receive-side flow control enforcement (FLOW_CONTROL_ERROR on violation)
- Stream state machine: all states including reserved states for push;
  full frame-legality enforcement per stream state per RFC 9113 §5.1
- SERVER PUSH: PUSH_PROMISE framing, reserved stream state, ENABLE_PUSH
  enforcement
- h2c cleartext: direct h2c and HTTP/1.1 Upgrade path (HTTP2-Settings
  header, stream 1 initialisation)
- GOAWAY: proper two-phase graceful shutdown (prepare phase sends
  last_stream_id=0x7FFFFFFF; final phase sends actual last processed
  stream_id), correct last-stream-ID tracking
- RST_STREAM: stream vs connection error classification
- PING: automatic PING ACK (disableable), PING submission API, PING/ACK
  callbacks for RTT measurement
- HTTP messaging validation (RFC 9113 §8): pseudo-header rules, lowercase
  field-name enforcement, forbidden headers, duplicate pseudo-header
  rejection, Content-Length consistency, trailer rules. Disableable.
- PRIORITY frames: accepted and silently ignored (deprecated by RFC 9113)
- Configurable allocator interface with NULL fallback to system malloc
- All flood and exhaustion protections (see Security section in ARCHITECTURE.md)
- Scatter-gather iovec send path with drain model and partial-send support
- Zero-copy body path via `HIVE_DATA_FLAG_NO_COPY`
- `hive_buf_t` buffer handle with explicit retain and debug lifetime
  enforcement
- Full callback set including connection-level events: `on_goaway`,
  `on_ping`, `on_ping_ack`, `on_settings_ack`, `on_connection_error`

**Explicitly excluded (v1):**

- WebSocket over HTTP/2 (RFC 8441) - v2
- Extended CONNECT (RFC 8441) - v2
- Extensible Priorities (RFC 9218) - v2
- HTTP/3 / QUIC - permanently out of scope
- I/O, sockets, TLS - permanently out of scope

### v2 (Planned)

- WebSocket over HTTP/2 (RFC 8441)
- Extended CONNECT (RFC 8441)
- Extensible Priorities (RFC 9218)

## Security Properties

Hive defends against the following classes of attack:

**HPACK bomb**: A malicious peer sends a tiny HEADERS frame that expands to
megabytes of decoded headers via HPACK compression. Hive enforces two
independent limits - maximum decoded header list size and maximum header
count per HEADERS block - incrementally as each header is decoded. Neither
limit requires buffering the full decoded block first. Exceeded:
RST_STREAM PROTOCOL_ERROR.

**CONTINUATION flood (CVE-2024-27316 class)**: A malicious peer sends an
unbounded stream of CONTINUATION frames, forcing the receiver to buffer
compressed header data indefinitely before any decoding occurs. Hive enforces
a hard cap on total compressed bytes in any HEADERS+CONTINUATION reassembly
sequence, applied before HPACK decoding begins. Exceeded: connection error
PROTOCOL_ERROR (GOAWAY). This is a connection error per RFC 9113 §4.3 -
receiving any frame other than CONTINUATION while in a CONTINUATION sequence
is a connection-level protocol violation.

**SETTINGS flood**: A malicious peer sends a stream of SETTINGS frames
without waiting for ACKs, consuming server resources in the pending ACK
queue. Hive maintains a fixed-size queue for inbound SETTINGS awaiting our
ACK, allocated at session creation. When the queue is full: GOAWAY
PROTOCOL_ERROR.

**RST_STREAM flood (Rapid Reset, CVE-2023-44487 class)**: A malicious peer
opens and immediately resets streams at high rate, exhausting server resources
without completing any requests. Hive tracks RST_STREAM rate in a rolling
window and fires the `on_rst_stream_flood` callback when the threshold is
exceeded. The caller decides the response - Hive does not act unilaterally.

**Stream ID exhaustion**: When the highest seen stream ID approaches 2^31,
Hive automatically initiates graceful shutdown via GOAWAY NO_ERROR. The
caller is notified via the `on_goaway` / shutdown callback. This prevents
silent protocol failure after long-lived connections process millions of
requests.

**Receive-side flow control enforcement**: Hive tracks the remaining receive
window at both connection and stream level. If a peer sends more DATA bytes
than the window allows, Hive issues a FLOW_CONTROL_ERROR - a connection
error for connection-level violations, a stream error for stream-level
violations.

All limits are configurable via the options API. Default values are
conservative and safe for production deployment. See ARCHITECTURE.md for
exact defaults and the rationale for each.

## Performance Targets

All figures are targets. Actual results depend on hardware, workload, and
the caller's allocator strategy.

| Scenario | Target |
|---|---|
| HEADERS frame decode (no Huffman, indexed lookup) | < 100 ns |
| HEADERS frame decode (Huffman, dynamic table insert) | < 500 ns |
| DATA frame delivery (zero-copy) | < 50 ns overhead above memcpy |
| Stream lookup (Knuth hash, L1 resident) | < 10 ns |
| WINDOW_UPDATE coalescing (eliminate 2× frames) | ≥ 2× reduction in WINDOW_UPDATE count |
| Session creation (with arena allocator) | 1 malloc |
| Session teardown (with arena allocator) | 1 free |
| Hot-path allocations per request (arena, no retain) | 0 |

The bottleneck for an HTTP/2 server at scale is not the protocol library
- it is TLS, syscall overhead, and file I/O. Hive is designed to not be
the bottleneck.

## Related Work

### nghttp2

The most widely deployed HTTP/2 C library. nghttp2 is production-hardened,
comprehensive, and battle-tested in nginx, curl, and many other projects.
It is the reference implementation for correctness. Hive differs in three
ways: Hive uses a configurable allocator interface (nghttp2 uses an internal
allocator), Hive performs strictly zero I/O (nghttp2 has optional I/O
callbacks), and Hive is simpler in scope - no HTTP/2 upgrade helpers beyond
what RFC 9113 requires, no HTTP/1.1 bridge layer. If you need a library with
the largest ecosystem, broadest compatibility, and the most deployment
history, use nghttp2. Hive is for callers who need allocator control, a
genuinely zero-dependency library, and code small enough to audit.

### H2O (the server, not the library)

H2O is an HTTP/2 web server written in C with emphasis on performance. Its
internal HTTP/2 implementation is tightly coupled to the server. Hive is a
standalone library with no server logic.

### libhttp2 (various)

Several projects use the name or concept of a C HTTP/2 library. None of
the widely available alternatives combine zero I/O, injected allocator,
zero application-level dependencies, and first-class OpenBSD support in a
single library under a permissive licence.

## License

ISC License. Simple, permissive, compatible with OpenBSD philosophy.

## Document Index

| Document | Audience | Purpose |
|---|---|---|
| README.md | Embedder | Integration guide, API quickstart, known limitations |
| PROJECT.md | Both | Overview, goals, scope, security model (this file) |
| ARCHITECTURE.md | Implementer | Internal design, memory layout, state machines, data structures |
| TECH_STACK.md | Implementer | Build system, compiler flags, tools, test harness |
| CODING_STANDARDS.md | Implementer | C style, security patterns, allocator discipline |
| DEVELOPMENT.md | Implementer | Phased build plan, milestones, task breakdown |
| TESTING.md | Implementer primary, embedder secondary | Test strategy, conformance suite (h2spec) |
| REPOSITORY_STRUCTURE.md | Implementer | Directory layout, file-by-file descriptions, naming conventions |
