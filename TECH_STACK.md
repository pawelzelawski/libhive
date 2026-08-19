# Tech Stack

## 1. Language

### C (C11)

Hive is written in C11. No C++, no scripting languages, no code generation.

**Standard**: C11 (`-std=c11`)

**Why C11 over C99**:
- `_Static_assert` for compile-time checks on struct sizes, protocol
  constants, and buffer dimensions - for example, verifying that
  `sizeof(hive_stream_t) == 64` does not silently change
- `_Alignas` / `_Alignof` for aligned iovec and arena declarations
- Anonymous structs and unions for frame header parsing
- Designated initialisers for the static HPACK table and Huffman decode table

**Feature test macros** (defined in Makefile, not in source files):
```c
_POSIX_C_SOURCE=200809L   /* POSIX.1-2008 */
_XOPEN_SOURCE=700         /* XSI extensions */
```

Do not define `_GNU_SOURCE` - it pulls in non-portable extensions and
breaks OpenBSD builds.

---

## 2. External Dependencies

Hive has **zero external library dependencies**. This is a hard requirement,
not a preference. Everything is implemented from the authoritative
specifications with no third-party code.

The only dependency is the C standard library (libc), which is available on
every supported platform without installation.

There are no vendored libraries, no submodules, no package manager files.

**Runtime dependencies for embedders**:
- Linux: none beyond libc
- OpenBSD: none beyond libc

---

## 3. Self-Written Modules

These modules are written from scratch as part of Hive. They have no
external dependencies and together form the complete library.

### 3.1 Session (`src/hive.c`, `src/hive.h`)

**Purpose**: Public API surface and session lifecycle. Creates and tears
down `hive_session_t`, orchestrates `hive_session_recv()` and
`hive_session_send()`, dispatches to internal modules.

**Responsibilities**:
- Session struct allocation and pre-allocation of all sub-buffers
- Session creation for server role, client role, and h2c Upgrade
- `hive_session_recv()` entry point - drives the receive state machine
- `hive_session_send()` entry point - drives the DATA flush and send callback
- `hive_buf_retain()` and `hive_buf_free()`
- `hive_session_want_read()` and `hive_session_want_write()`
- Introspection functions

See ARCHITECTURE.md §1 and §9 for full session lifecycle and public API.

### 3.2 Frame Parser (`src/hive_frame.c`, `src/hive_frame.h`)

**Purpose**: Incremental HTTP/2 frame parsing from the caller's input buffer
and frame header serialisation into `send_buf`.

**Responsibilities**:
- All 18 receive states of the state machine (ARCHITECTURE.md §3.1)
- 9-byte frame header staging and parsing (ARCHITECTURE.md §3.2)
- Frame validation: length vs `SETTINGS_MAX_FRAME_SIZE`, stream_id rules
- CONTINUATION lockout enforcement (ARCHITECTURE.md §3.5)
- New stream validation: ID parity, monotonicity, concurrent limit
  (ARCHITECTURE.md §3.4)
- `ctrl_staging` accumulation for small control frame payloads
- `frame_hdr_write()` and `frame_hdr_write_at()` for outgoing frames

### 3.3 HPACK (`src/hive_hpack.c`, `src/hive_hpack.h`)

**Purpose**: Full RFC 7541 HPACK encoder and decoder. Also implements the
standalone HPACK public API.

**Responsibilities**:
- Dynamic table management: ring buffer, entry allocation, eviction
  (ARCHITECTURE.md §4.1, §4.2)
- Static table: compile-time array, index 1–61 (ARCHITECTURE.md §4.3)
- Huffman decode: 256-entry lookup table (ARCHITECTURE.md §4.4)
- Huffman encode: RFC 7541 Appendix B code table, compile-time
- `hpack_decode_block()`: decode a full HEADERS block with bomb protection
  (ARCHITECTURE.md §4.5)
- String decode: Huffman path via `hpack_scratch`, literal path via
  direct pointer (ARCHITECTURE.md §4.6)
- Integer varint encode/decode (ARCHITECTURE.md §4.7)
- `hpack_encode_block()`: encode header list into `send_buf` (ARCHITECTURE.md §4.8)
- Hash index for large tables (ARCHITECTURE.md §4.9)
- Standalone `hive_hpack_encoder_t` and `hive_hpack_decoder_t` objects

### 3.4 Stream Table (`src/hive.c`)

**Purpose**: Two-layer stream state store: open-addressed hash table for
O(1) lookup and pre-allocated slot array for state storage.

**Note**: originally planned as a separate `hive_stream.c`/`hive_stream.h`
pair; merged into `hive.c` during Phase 4 implementation. All stream table
symbols remain internal and are declared in `hive_internal.h`.

**Responsibilities**:
- `stream_open()`, `stream_lookup()`, `stream_close()`
- Knuth multiplicative hash function (ARCHITECTURE.md §5.4)
- Tombstone management and compaction (ARCHITECTURE.md §5.5, §5.6)
- Free stack management (O(1) slot alloc and free)
- Stream state transitions (ARCHITECTURE.md §5.3)

### 3.5 Flow Control (`src/hive_frame.c`)

**Purpose**: Connection-level and stream-level flow control windows, and
coalesced WINDOW_UPDATE emission.

**Note**: originally planned as a separate `hive_flow.c`/`hive_flow.h`
pair; merged into `hive_frame.c` during Phase 5 implementation alongside
the receive state machine that consumes DATA frames.

**Responsibilities**:
- Receive window tracking: `recv_consumed` per stream and per connection
- WINDOW_UPDATE coalescing: queue only when consumed > window/2
  (ARCHITECTURE.md §7.7)
- Send window enforcement: block DATA frames when `send_window == 0`
- Retroactive stream window adjustment on `SETTINGS_INITIAL_WINDOW_SIZE`
  change
- Connection-level WINDOW_UPDATE handling

### 3.6 Send Queue (`src/hive_send.c`, `src/hive_send.h`)

**Purpose**: Scatter-gather iovec accumulation and drain.

**Responsibilities**:
- `frame_hdr_write()` - write 9-byte frame header into `send_buf`
- Control frame queuing: SETTINGS, SETTINGS ACK, PING ACK, RST_STREAM,
  WINDOW_UPDATE, GOAWAY (ARCHITECTURE.md §6.3)
- HEADERS frame queuing with HPACK encode and CONTINUATION splitting
  (ARCHITECTURE.md §6.4)
- DATA frame queuing with `NO_COPY` zero-copy path (ARCHITECTURE.md §6.5)
- `send_queue_flush_data()`: drive pending data_sources within flow control
  limits at `hive_session_send()` time
- iovec overflow handling (ARCHITECTURE.md §6.7)

### 3.7 Security (`src/hive_frame.c`, `src/hive_hpack.c`)

**Purpose**: All flood detection and exhaustion protection logic.

**Note**: originally planned as a separate `hive_security.c`/`hive_security.h`
pair; implemented directly in `hive_frame.c` (frame-level checks) and
`hive_hpack.c` (HPACK bomb protection and HTTP messaging validation) during
Phase 7. There is no separate security module.

**Responsibilities**:
- HPACK bomb: decoded size and count limits (ARCHITECTURE.md §8.2)
- CONTINUATION flood: compressed reassembly cap (ARCHITECTURE.md §8.3)
- SETTINGS flood: inbound counter and GOAWAY (ARCHITECTURE.md §8.4)
- RST_STREAM flood: rolling rate counter and callback (ARCHITECTURE.md §8.5)
- Stream ID exhaustion: auto-GOAWAY threshold (ARCHITECTURE.md §8.6)
- HTTP messaging validation: RFC 9113 §8 rules (ARCHITECTURE.md §8)
- `hive_buf_t` ASan poisoning: ephemeral region marking (ARCHITECTURE.md §8.8)

Note: the HPACK always-copy rule (ARCHITECTURE.md §8.1) is enforced inside
`hive_hpack.c` at every dynamic table insertion. It is not a security module
function - it is a property of the table implementation itself.

---

## 4. System Libraries

These are part of the C standard library or POSIX and require no
installation.

### 4.1 Standard C Library

```c
#include <stdint.h>    /* uint8_t, uint32_t, int32_t, size_t */
#include <stddef.h>    /* offsetof, NULL, size_t */
#include <string.h>    /* memcpy, memset, memcmp */
#include <stdlib.h>    /* malloc, free, calloc (system allocator path only) */
#include <time.h>      /* clock_gettime - RST_STREAM flood window timing */
#include <sys/uio.h>   /* struct iovec - send callback and scatter-gather */
#include <errno.h>     /* EINVAL, ENOMEM - error returns */
```

`malloc`, `free`, `calloc`, and `realloc` are called only in one place: the
NULL-allocator fallback path that wraps them into the `hive_mem_t` interface.
All other code in the library goes through `session->mem.*`. This is the
direct enforcement mechanism for the no-direct-malloc coding standard.

### 4.2 Clocks

```c
#include <time.h>
```

`clock_gettime(CLOCK_MONOTONIC, ...)` is used exclusively for the RST_STREAM
flood rate window (ARCHITECTURE.md §8.5). `CLOCK_MONOTONIC` is correct here
because we need elapsed seconds, not wall-clock time. No persistence, no
cross-process coordination - monotonicity is the only requirement.

No other clock usage exists in the library. Hive does not implement
timeouts - those are the caller's responsibility.

### 4.3 String Safety

**OpenBSD**: `strlcpy` and `strlcat` are in libc.

**Linux**: `strlcpy` and `strlcat` are not in glibc. Hive provides
`src/compat_str.c` and `src/compat_str.h` with portable implementations
compiled on Linux only. On OpenBSD, these files are not compiled.

Do not use `strcpy`, `strcat`, `sprintf`, or `gets` anywhere in the library.

---

## 5. Build System

### 5.1 Makefile Structure

One top-level Makefile. No per-directory Makefiles.

```
hive/
├── Makefile
├── src/          ← all library source files
├── tests/        ← unit tests and conformance helpers
├── include/      ← public header (hive.h)
└── tools/        ← optional developer tools (frame decoder, fuzz driver)
```

The output of `make` is a static library: `libhive.a`. Embedders link
against `libhive.a` and include `include/hive.h`.

### 5.2 Compiler

**Primary**: Clang (preferred on both platforms).

OpenBSD ships Clang as the system compiler. On Linux:
```sh
# Debian/Ubuntu
apt install clang

# RHEL/Fedora
dnf install clang

# Arch
pacman -S clang
```

Minimum supported version: Clang 11.0. Older versions lack full C11 support
and `_Static_assert` in all required positions.

### 5.3 Compiler Flags

```makefile
CC     = clang
CSTD   = -std=c11

# Warnings - all treated as errors in CI
CWARN  = -Wall -Wextra -Wpedantic    \
         -Wshadow                     \
         -Wformat=2                   \
         -Wconversion                 \
         -Wnull-dereference           \
         -Wstrict-prototypes          \
         -Wmissing-prototypes         \
         -Wold-style-definition

# Development build - sanitizers, debug symbols, no optimisation
CFLAGS_DEV = $(CSTD) $(CWARN)               \
             -g -O0                          \
             -fsanitize=address,undefined    \
             -fno-omit-frame-pointer         \
             -DDEBUG                         \
             -DHIVE_DEBUG=1

# Release build - optimised, hardened
CFLAGS_REL = $(CSTD) $(CWARN)               \
             -O2                             \
             -DNDEBUG                        \
             -fstack-protector-strong        \
             -fPIC                           \
             -D_FORTIFY_SOURCE=2

# Feature test macros (all builds)
CFLAGS_FT  = -D_POSIX_C_SOURCE=200809L      \
             -D_XOPEN_SOURCE=700

# Platform detection
UNAME := $(shell uname)

ifeq ($(UNAME), OpenBSD)
    CFLAGS_OS  = -DOPENBSD
    LDFLAGS_OS =
    COMPAT_SRC =
else ifeq ($(UNAME), Linux)
    CFLAGS_OS  = -DLINUX
    LDFLAGS_OS =
    COMPAT_SRC = src/compat_str.c
else
    $(error Unsupported platform: $(UNAME))
endif
```

`-fPIC` in release builds produces position-independent object code,
required for embedders who link Hive into a shared library.

`-DHIVE_DEBUG=1` in development builds enables:
- `HIVE_BUF_VALID` clearing and ASan poisoning on callback return
- `_Static_assert` expansion for struct size verification
- Additional assertion paths throughout the receive state machine

### 5.4 Build Targets

```makefile
make              # same as make dev
make dev          # debug build with ASan/UBSan
make release      # optimised hardened build
make test         # build and run unit tests
make test-asan    # run tests under ASan/UBSan (same as make dev + make test)
make test-clock   # run the clock-gated RST_STREAM flood regressions
make test-tsan    # run tests under ThreadSanitizer (Linux only)
make valgrind     # run tests under Valgrind (Linux only)
make h2spec       # run h2spec conformance suite against test server
make lint         # clang-tidy + cppcheck
make clean        # remove build artifacts
make install      # install libhive.a and include/hive.h to PREFIX
```

`make install` installs two files:

```makefile
PREFIX     = /usr/local
LIBDIR     = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

install:
    install -m 644 libhive.a          $(LIBDIR)/libhive.a
    install -m 644 include/hive.h     $(INCLUDEDIR)/hive.h
```

No other files are installed. No pkg-config file is generated in v1.

### 5.5 Source File List

The Makefile builds all `.c` files in `src/` plus the platform-specific
compat file. Platform-specific compilation is handled via `$(COMPAT_SRC)`:

```makefile
LIB_SRC = src/hive.c             \
           src/hive_frame_bare.c \
           src/hive_frame.c      \
           src/hive_hpack.c      \
           src/hive_send.c       \
           $(COMPAT_SRC)

LIB_OBJ = $(LIB_SRC:.c=.o)

libhive.a: $(LIB_OBJ)
    $(AR) rcs $@ $^
```

Stream table (§3.4), flow control (§3.5), and security (§3.7) logic are
implemented directly in `hive.c` and `hive_frame.c` - there are no separate
`hive_stream.c`, `hive_flow.c`, or `hive_security.c` files.

### 5.6 Test Binary Structure

```makefile
TEST_SRC = tests/test_hpack.c       \
           tests/test_frame.c       \
           tests/test_flow.c        \
           tests/test_session.c     \
           tests/test_security.c    \
           tests/run_tests.c

TEST_BIN = build/tests/run_tests

$(TEST_BIN): $(TEST_SRC) libhive.a
    $(CC) $(CFLAGS) $(CFLAGS_FT) $(CFLAGS_OS) \
        -I include/ $(TEST_SRC) libhive.a -o $@
```

`tests/test_harness.h` is a header included by each test file - it is not
a compiled source file and must not appear in TEST_SRC.

Stream table tests are in `tests/test_session.c` (not a separate
`test_stream.c`), co-located with session creation and send-queue tests.

---

## 6. Test Harness

No external test framework. Plain C with a minimal assertion macro, identical
in structure to the kvd test harness.

### 6.1 Assertion Macro

```c
/* tests/test_harness.h */
static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name)                                  \
    do {                                           \
        tests_run++;                               \
        if (test_##name()) {                       \
            tests_passed++;                        \
            printf("PASS: " #name "\n");           \
        } else {                                   \
            printf("FAIL: " #name "\n");           \
        }                                          \
    } while (0)

/* In each test file's main(): */
RUN(hpack_static_table_lookup);
RUN(hpack_huffman_decode_hello);
/* ... */
printf("%d/%d tests passed\n", tests_passed, tests_run);
return tests_passed == tests_run ? 0 : 1;
```

Every test binary:
- Returns 0 if all tests pass
- Returns 1 if any test fails
- Prints `PASS: test_name` or `FAIL: test_name` per test case
- Prints `N/M tests passed` as the final line

`make test` runs `tests/run_tests.sh` which runs the test binary and fails
if it exits non-zero.

### 6.2 Byte Stream Injection

Hive's zero-I/O design makes unit testing straightforward - test code
constructs raw HTTP/2 byte sequences and feeds them directly to
`hive_session_recv()`. No sockets, no threads, no timing.

```c
/* helper used throughout test files */
static void
feed_bytes(hive_session_t *s, const uint8_t *data, size_t len)
{
    ssize_t consumed;
    size_t  pos = 0;

    while (pos < len) {
        consumed = hive_session_recv(s, data + pos, len - pos);
        if (consumed < 0)
            return;  /* session error - test will fail on next assert */
        pos += (size_t)consumed;
    }
}
```

Tests feed bytes in varying chunk sizes (1 byte at a time, full frames,
split across frame boundaries) to exercise all code paths in the state
machine. This is the primary technique for testing partial delivery.

### 6.3 Callback Capture

Tests intercept callbacks via a capture struct populated by the test's
callback implementations:

```c
typedef struct {
    uint32_t  last_stream_id;
    int       headers_complete_fired;
    int       end_stream;
    char      last_method[16];
    char      last_path[256];
    int       data_chunk_count;
    size_t    data_total_bytes;
    uint32_t  stream_close_error;
    int       rst_flood_fired;
    uint32_t  rst_flood_rate;
} test_capture_t;

static int
test_on_header(hive_session_t *s, uint32_t sid,
    hive_buf_t *name, hive_buf_t *value, uint8_t flags, void *ud)
{
    test_capture_t *cap = (test_capture_t *)ud;
    /* copy name/value into cap fields for assertion after callback */
    return 0;
}
```

---

## 7. Development Tools

### 7.1 Valgrind (Linux only)

**Purpose**: Memory error detection - leaks, use-after-free, uninitialised
reads.

**Installation**: `apt install valgrind`

**Usage**:
```sh
make dev
valgrind --leak-check=full          \
         --show-leak-kinds=all      \
         --track-origins=yes        \
         --error-exitcode=1         \
         ./tests/run_tests
```

Hive has no external library dependencies, so no suppression file is needed.
All code must pass Valgrind clean. Run on Linux before every commit.
Valgrind is not available on OpenBSD - use the ASan build there.

### 7.2 AddressSanitizer + UndefinedBehaviorSanitizer

**Purpose**: Runtime memory and undefined behaviour detection.

**Compiler flags**: `-fsanitize=address,undefined` (included in `make dev`)

Enabled by the Makefile on Linux. OpenBSD builds retain `HIVE_DEBUG` checks,
but its default toolchain does not support this AddressSanitizer target. Do
not ship sanitizer builds to embedders.

The `HIVE_DEBUG=1` macro activates additional ASan behaviour within the
library: `hive_buf_t` handles are poisoned via `__asan_poison_memory_region`
on callback return, and retained buffers are unpoisoned via
`__asan_unpoison_memory_region` when retained. These macros are no-ops in
non-ASan builds.

```c
#if defined(HIVE_DEBUG) && defined(__SANITIZE_ADDRESS__)
#  include <sanitizer/asan_interface.h>
#  define HIVE_ASAN_POISON(ptr, size)   __asan_poison_memory_region((ptr), (size))
#  define HIVE_ASAN_UNPOISON(ptr, size) __asan_unpoison_memory_region((ptr), (size))
#else
#  define HIVE_ASAN_POISON(ptr, size)   ((void)0)
#  define HIVE_ASAN_UNPOISON(ptr, size) ((void)0)
#endif
```

### 7.3 ThreadSanitizer (Linux only)

**Purpose**: Data race detection. TSAN and ASan are mutually exclusive -
TSAN runs as a separate target.

**Usage**:
```sh
make test-tsan
```

Hive itself is not thread-safe by design (one session, one thread). However,
TSAN is useful during development of the test server used for h2spec runs,
which handles multiple connections and uses a simple multi-connection model.

TSAN is not a per-commit gate. Run at phase boundaries and before release.

### 7.4 clang-tidy

**Purpose**: Static analysis.

**Installation**: `apt install clang-tidy` (Linux) - included with Clang on
OpenBSD.

**Usage**:
```sh
make lint
# or directly:
clang-tidy src/*.c -- $(CFLAGS_DEV) $(CFLAGS_FT) $(CFLAGS_OS) -I include/
```

### 7.5 cppcheck

**Purpose**: Additional static analysis, complements clang-tidy.

**Installation**: `apt install cppcheck` / `pkg_add cppcheck`

```sh
cppcheck --enable=all --error-exitcode=1 \
         --suppress=missingIncludeSystem \
         src/
```

### 7.6 clang-format

**Purpose**: Consistent code formatting.

**Configuration**: `.clang-format` in repository root, OpenBSD KNF style.

```sh
clang-format -i src/*.c src/*.h include/hive.h
```

CI rejects commits that are not clang-format clean.

### 7.7 h2spec

**Purpose**: HTTP/2 protocol conformance suite. Fires RFC-conformant frame
sequences at a running HTTP/2 server and verifies correct responses.

**Repository**: https://github.com/summerwind/h2spec

**Installation**:
```sh
# Linux amd64: pinned CI artifact (h2spec v2.6.0)
curl -fsSLO https://github.com/summerwind/h2spec/releases/download/v2.6.0/h2spec_linux_amd64.tar.gz
echo '157ee0de702e01ad40e752dbf074b366027e550c8e7504f9450da2809e279318  h2spec_linux_amd64.tar.gz' | sha256sum -c -
tar xzf h2spec_linux_amd64.tar.gz
chmod +x h2spec
mv h2spec /usr/local/bin/

# OpenBSD: download the openbsd/amd64 build or build from source
```

**Usage**:
```sh
# Start the test server (built from tests/h2spec_server.c)
make h2spec-server
./tests/h2spec_server --port 8443 &

# Run the full conformance suite
make h2spec H2SPEC_PORT=8443
# equivalent to:
h2spec -h 127.0.0.1 -p 8443 --tls --insecure
```

`tests/h2spec_server.c` is a minimal HTTP/2 server built on top of
`libhive.a`. It handles all h2spec test cases and returns appropriate
responses. It uses libtls from LibreSSL for TLS, because h2spec requires
TLS by default (ALPN `h2` negotiation). This is the only development
dependency beyond the C toolchain.

**libtls installation** (for the test server only - not linked into
`libhive.a`):
```sh
# OpenBSD - included in base system
# No installation required

# Debian/Ubuntu
apt install libssl-dev   # libtls is part of LibreSSL on newer Debian
# or for portbsd/LibreSSL:
apt install libtls-dev

# RHEL/Fedora
dnf install libressl-devel
```

All h2spec tests must pass before Hive v1 is considered complete. There are
no documented planned exceptions. If a test case requires a behaviour that
Hive implements differently from h2spec's expectation, the discrepancy must
be diagnosed against the RFC before concluding that Hive is correct and
h2spec is wrong - that conclusion requires explicit documentation and
is not the default assumption.

See TESTING.md §4 for the full conformance test strategy.

### 7.8 Frame Decoder Tool (`tools/hive_decode.c`)

**Purpose**: Offline HTTP/2 frame decoder for protocol debugging. Reads
raw HTTP/2 bytes from a file or stdin and pretty-prints each frame.
Useful when diagnosing unexpected frames from real clients or servers.

**Built from**: `tools/hive_decode.c` and `src/hive_frame_bare.c` only - no
dependency on the session, HPACK, stream, or send modules.

**Usage**:
```sh
make tools
# Capture raw bytes (after TLS decryption) and decode:
./tools/hive_decode /tmp/raw_h2_frames.bin

# Pipe from stdin:
cat /tmp/raw_h2_frames.bin | ./tools/hive_decode
```

**Output**:
```
--- frame 1 ---
type:     0x1  HEADERS
flags:    0x25 END_STREAM|END_HEADERS|PRIORITY
stream:   1
length:   23 bytes

--- frame 2 ---
type:     0x4  SETTINGS
flags:    0x00
stream:   0 (connection)
length:   18 bytes
  HEADER_TABLE_SIZE     = 4096
  INITIAL_WINDOW_SIZE   = 1048576
  MAX_FRAME_SIZE        = 16384

--- frame 3 ---
type:     0x0  DATA
flags:    0x01 END_STREAM
stream:   1
length:   8192 bytes
```

`hive_decode` does not decode HPACK - it shows raw compressed bytes for
HEADERS frames. It is an offline protocol debugging tool, not a full
HTTP/2 analyser.

---

## 8. Dependency Summary

| Dependency | Version | Type | Purpose | Platforms |
|---|---|---|---|---|
| Clang | 18 in CI; 22 also checked locally | Build tool | Compilation | Linux, OpenBSD |
| libc | system | System lib | Standard C | Linux, OpenBSD |
| Valgrind | latest | Dev tool | Memory checking | Linux only |
| clang-tidy | 18 in CI; current releases supported locally | Dev tool | Static analysis | Linux, OpenBSD |
| cppcheck | latest | Dev tool | Static analysis | Linux, OpenBSD |
| h2spec | 2.6.0 | Conformance | HTTP/2 spec test | Linux, OpenBSD |
| libtls (LibreSSL) | ≥ 3.3 | Dev tool | TLS for test server | Linux, OpenBSD |

**Runtime dependencies for embedders**:
- Linux: none (libc only)
- OpenBSD: none (libc only)

**Development dependencies** (not required to embed the library):
- Valgrind (Linux only)
- clang-tidy, cppcheck
- h2spec binary
- libtls (LibreSSL) - for the h2spec test server only

libtls is **not** linked into `libhive.a`. It is used only in
`tests/h2spec_server.c`. Embedders who do not run h2spec do not need
libtls installed.
