# Coding Standards

## 1. Code Style

### 1.1 OpenBSD KNF (Kernel Normal Form)

Hive follows OpenBSD's Kernel Normal Form style. This is the style used
throughout the OpenBSD base system and is required for all source files.

**Indentation**:
- Tabs for indentation, 8-character display width
- Spaces only for alignment within a line, never for indentation
- Never mix tabs and spaces for indentation

```c
/* Correct */
static int
stream_open(hive_session_t *s, uint32_t stream_id, uint8_t state)
{
	uint32_t slot;

	if (s->stream_open_count >= s->opt_max_concurrent_streams)
		return HIVE_ERR_REFUSED_STREAM;
	slot = stream_free_pop(s);
	return stream_hash_insert(s, stream_id, slot, state);
}

/* Wrong - spaces used for indentation */
static int
stream_open(hive_session_t *s, uint32_t stream_id, uint8_t state)
{
    uint32_t slot;   /* spaces, not tabs */
}
```

**Braces**:
```c
/* Functions: opening brace on its own line */
static int
hpack_decode_block(hive_session_t *s, const uint8_t *data, size_t len)
{
	/* body */
}

/* Control structures: opening brace on same line */
if (session->recv_state == RECV_CONTINUATION_PAYLOAD) {
	if (cur_frame.type != HIVE_FRAME_CONTINUATION)
		return session_error(s, HIVE_ERR_PROTOCOL,
		    HIVE_H2_PROTOCOL_ERROR, 0);
}

/* Single-statement bodies: no braces, indented on next line */
if (s->reassembly_len + n > s->opt_max_continuation_size)
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);

/* Loop with single statement */
for (i = 0; i < nvlen; i++)
	hpack_encode_field(&s->enc_table, &nva[i], out, &pos);
```

**Line length**: Maximum 80 characters. Break long lines at logical points,
aligning continuation with the opening parenthesis or using one extra tab:

```c
/* Break at logical point, align with opening paren */
consumed = hive_session_recv(session,
    read_buf + pos, read_buf_len - pos);

/* Extra tab indent for continuation */
return session_error(s, HIVE_ERR_PROTOCOL,
	HIVE_H2_PROTOCOL_ERROR, stream_id);
```

**Naming conventions**:
```c
/* Variables and function parameters: lowercase with underscores */
size_t    name_len;
uint32_t  stream_id;
uint8_t   recv_state;
int32_t   send_window;

/* Functions: lowercase with underscores, module prefix */
int    hpack_decode_block(hive_session_t *s, const uint8_t *data, size_t len);
int    stream_open(hive_session_t *s, uint32_t stream_id, uint8_t state);
void   send_queue_append_ctrl(hive_session_t *s, uint8_t type, ...);

/* Constants and macros: uppercase with underscores, HIVE_ prefix */
#define HIVE_FRAME_HEADERS        0x1
#define HIVE_FRAME_DATA           0x0
#define HPACK_LINEAR_THRESHOLD    16384
#define HIVE_BUF_VALID            0x01u
#define HIVE_DATA_FLAG_NO_COPY    0x02u

/* Structs and typedefs: lowercase, _t suffix */
typedef struct hive_session  hive_session_t;
typedef struct hpack_table   hpack_table_t;
typedef struct hive_stream   hive_stream_t;

/* Enums: uppercase values with HIVE_ prefix */
typedef enum {
	HIVE_ROLE_SERVER = 0,
	HIVE_ROLE_CLIENT = 1,
} hive_role_t;

typedef enum {
	RECV_FRAME_HEADER         = 0,
	RECV_DATA_PAYLOAD         = 1,
	RECV_HEADERS_PAYLOAD      = 2,
	/* ... */
} hive_recv_state_t;
```

**Spacing**:
```c
/* Space after keywords, not after function names */
if (condition)                 /* correct */
if(condition)                  /* wrong */
hpack_decode_block(s, data, n) /* correct */
hpack_decode_block (s, data, n)/* wrong */

/* No space inside parentheses */
if (n > 0)                     /* correct */
if ( n > 0 )                   /* wrong */

/* Space around binary operators */
encoded_len = name_len + value_len + 32;
h = (stream_id * 2654435761u) >> (32 - table_bits);

/* No space for unary operators */
ptr = &entry;
val = *ptr;
n   = -1;
```

**Return type on its own line**:
```c
/* Correct */
static ssize_t
hive_session_recv(hive_session_t *session, const uint8_t *data, size_t len)
{
	/* ... */
}

/* Wrong */
static ssize_t hive_session_recv(hive_session_t *session,
    const uint8_t *data, size_t len)
{
	/* ... */
}
```

### 1.2 File Organisation

**Public header** (`include/hive.h`):

The public header exposes only what embedders need. Internal types and
functions are never declared here. The header is self-contained - an
embedder includes only `hive.h` and nothing else.

```c
#ifndef HIVE_H
#define HIVE_H

/*
 * hive.h - HTTP/2 library public API
 *
 * Create a session with hive_session_server_new() or hive_session_client_new().
 * Feed incoming bytes with hive_session_recv().
 * Submit responses with hive_submit_response().
 * Drain outgoing frames with hive_session_send().
 *
 * See ARCHITECTURE.md for full protocol and data flow documentation.
 * See README.md for a quickstart integration example.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/* ... public types, constants, and function declarations ... */

#endif /* HIVE_H */
```

**Internal headers** (e.g. `src/hive_frame.h`):

```c
#ifndef HIVE_FRAME_H
#define HIVE_FRAME_H

/*
 * hive_frame.h - frame parser and serialiser (internal)
 * See ARCHITECTURE.md §3 for the receive state machine.
 * See ARCHITECTURE.md §6 for frame header serialisation.
 */

#include "../include/hive.h"
#include "hive_internal.h"

/* ... internal types and function declarations ... */

#endif /* HIVE_FRAME_H */
```

**Source files** (`.c`):
```c
/*
 * hive_hpack.c - HPACK encoder and decoder
 *
 * See ARCHITECTURE.md §4 for dynamic table design and entry layout.
 * See ARCHITECTURE.md §8.1 for the always-copy security requirement.
 * RFC 7541: https://www.rfc-editor.org/rfc/rfc7541
 */

#include <string.h>

#include "../include/hive.h"
#include "hive_hpack.h"
#include "hive_internal.h"

/* Static table - RFC 7541 Appendix A */
static const hive_nv_t hpack_static_table[61] = { /* ... */ };

/* Huffman decode table - RFC 7541 Appendix B, 256 entries */
static const huff_entry_t huff_table[256] = { /* ... */ };
```

**Include order** (within each group, alphabetical):
```c
/* 1. System headers */
#include <stdint.h>
#include <string.h>
#include <time.h>

/* 2. OS-specific headers */
#ifdef __OpenBSD__
#include <sys/event.h>
#endif
#ifdef __linux__
#include <sys/epoll.h>
#endif

/* 3. Public library header */
#include "../include/hive.h"

/* 4. Internal headers */
#include "hive_frame.h"
#include "hive_hpack.h"
#include "hive_internal.h"
#include "hive_stream.h"
```

### 1.3 Static Assertions

Use `_Static_assert` to catch layout changes at compile time. These must
not be removed - they are the first line of defence against silent struct
size changes that would break memory budget calculations and pre-allocation.

```c
/* In hive_internal.h */
_Static_assert(sizeof(hive_stream_t) == 64,
    "hive_stream_t size changed - update ARCHITECTURE.md §5.3 and §11");
_Static_assert(sizeof(stream_hash_entry_t) == 8,
    "stream_hash_entry_t size changed - update ARCHITECTURE.md §5.2");
_Static_assert(sizeof(hpack_entry_t) == 8,
    "hpack_entry_t size changed - update ARCHITECTURE.md §4.2");
_Static_assert(sizeof(hive_settings_t) == 24,
    "hive_settings_t size changed - update ARCHITECTURE.md §2.5");
_Static_assert(sizeof(huff_entry_t) == 4,
    "huff_entry_t size changed - Huffman table is 1KB at 4 bytes per entry");
```

---

## 2. Allocator Discipline

This is the most important coding rule in Hive. It is not optional and it
is enforced by code review on every function.

### 2.1 The Rule

**No direct calls to `malloc`, `free`, `calloc`, or `realloc` anywhere in
the library source, except in the NULL-allocator shim.**

Every allocation must go through the session's allocator interface:

```c
/* Correct - allocate via session allocator */
entry = (hpack_entry_t *)s->mem.malloc(
    sizeof(hpack_entry_t) + name_len + value_len,
    s->mem.ctx);
if (entry == NULL)
	return HIVE_ERR_NOMEM;

/* Wrong - direct malloc */
entry = malloc(sizeof(hpack_entry_t) + name_len + value_len);
```

This rule applies to every `.c` file in `src/` without exception. The
one place `malloc` appears by name in the library is in `hive.c`, in the
shim that wraps system malloc when the caller passes NULL:

```c
/* hive.c - NULL allocator shim.
 * This is the ONLY permitted direct use of malloc/free/calloc/realloc
 * in the library source. All other src/ files use s->mem.* exclusively. */
static void *
null_alloc_malloc(size_t size, void *ctx)
{
	(void)ctx;
	return malloc(size);
}

static void
null_alloc_free(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);
}

/* ... calloc, realloc similarly */

static const hive_mem_t null_allocator = {
	null_alloc_malloc,
	null_alloc_free,
	null_alloc_calloc,
	null_alloc_realloc,
	NULL,
};
```

### 2.2 Why This Rule Exists

The allocator interface allows Wraith to use a connection-scoped arena:
one `malloc` at connection accept, zero structural allocation during the
connection lifetime, one `free` at connection close. If any code path
bypasses the session allocator, Wraith's zero-malloc hot path breaks
silently.

This is the mechanism that makes `hive_session_free()` correct regardless
of what allocator the caller provided.

### 2.3 What Allocates After Session Creation

All structural per-session memory is pre-allocated at session creation.
The pre-allocated blocks are:

- `stream_hash` - stream lookup hash table entries
- `stream_slots` - stream state objects
- `stream_free_stack` - free slot index array
- `send_iov` - iovec array
- `send_buf` - frame serialisation buffer
- `reassembly_buf` - HEADERS+CONTINUATION reassembly
- `hpack_scratch_name` - Huffman decode scratch for header names
- `hpack_scratch_value` - Huffman decode scratch for header values
- `enc_table.ring` - encoder ring buffer pointer array
- `dec_table.ring` - decoder ring buffer pointer array
- `pending_settings` - outbound SETTINGS pending ring

Two additional allocation categories occur during normal protocol operation
after session creation:

1. **HPACK dynamic table entries** - one `mem.malloc` per header inserted
   into the dynamic table, one `mem.free` per eviction. With an arena
   allocator, free is a no-op and insertions are free-list pops.

2. **`hive_buf_retain()` calls** - one `mem.malloc` per retained header
   value, freed explicitly by the caller via `hive_buf_free()`.

Both go through the session allocator. Both are expected. Neither violates
the allocator discipline rule. The claim "no allocation in the hot path"
applies to the common case where the caller does not retain headers and the
HPACK dynamic table is at steady state.

### 2.4 Checking Allocator Return Values

Every call to the session allocator must check for NULL:

```c
p = s->mem.malloc(size, s->mem.ctx);
if (p == NULL)
	return HIVE_ERR_NOMEM;
```

Never assume an allocator call succeeds. Even arena allocators can fail if
the arena is exhausted.

---

## 3. Security Practices

Security is not added at the end. Every function that processes network
input must follow these practices unconditionally.

### 3.1 Input Validation: Length Before Content

All data received from the network is hostile until validated. Check length
fields before reading content. Reject immediately on any out-of-range value.

```c
/*
 * Correct: validate length before reading content.
 * See ARCHITECTURE.md §3.3 for the recv loop structure.
 */
static int
process_settings_param(hive_session_t *s,
    const uint8_t *ctrl, uint8_t ctrl_len)
{
	uint16_t param_id;
	uint32_t param_val;

	/* Require exactly 6 bytes before reading */
	if (ctrl_len != 6)
		return HIVE_ERR_PROTOCOL;

	memcpy(&param_id,  ctrl,     2);
	memcpy(&param_val, ctrl + 2, 4);
	param_id  = ntohs(param_id);
	param_val = ntohl(param_val);

	switch (param_id) {
	case HIVE_SETTINGS_INITIAL_WINDOW_SIZE:
		if (param_val > 0x7FFFFFFFu)  /* RFC 9113 §6.9.2 */
			return session_error(s, HIVE_ERR_FLOW_CONTROL,
			    HIVE_H2_FLOW_CONTROL_ERROR, 0);
		break;
	case HIVE_SETTINGS_MAX_FRAME_SIZE:
		if (param_val < 16384 || param_val > 16777215)
			return session_error(s, HIVE_ERR_PROTOCOL,
			    HIVE_H2_PROTOCOL_ERROR, 0);
		break;
	/* unknown parameter IDs: ignore per RFC 9113 §6.5 */
	}
	return HIVE_OK;
}
```

Rules:
- Validate inbound frame `length` against **`local_settings.max_frame_size`**
  (what we advertised to the peer) before transitioning to any payload state.
  Do **not** use `remote_settings.max_frame_size` for inbound validation.
  See ARCHITECTURE.md §2.5 for the SETTINGS directionality rules.
- `remote_settings.max_frame_size` governs outbound frame sizing - the
  maximum size of frames we send, which the peer told us it can accept.
- Validate `stream_id` rules (parity, monotonicity, zero-vs-nonzero) before
  creating or looking up stream state. See ARCHITECTURE.md §3.4.
- Validate HPACK decoded header size and count incrementally, before
  firing the `on_header` callback. See ARCHITECTURE.md §8.2.
- Validate `reassembly_len + n ≤ opt_max_continuation_size` before writing
  into `reassembly_buf`. Exceeding this is a **connection error** (GOAWAY),
  not a stream error (RST_STREAM). See ARCHITECTURE.md §8.3 and §4.2.
- Never crash, never assert on network input in release builds.

### 3.2 SECURITY Comments

Mark security decisions explicitly so they are never accidentally removed
or simplified during refactoring. The comment format is `/* SECURITY: ... */`
on its own line before the guarded code.

```c
/*
 * SECURITY: enforce reassembly cap before writing into reassembly_buf.
 * Connection error (GOAWAY) per RFC 9113 §4.3 - NOT a stream error.
 * Changing this to RST_STREAM would leave the connection vulnerable.
 * Do not remove or defer this check. See ARCHITECTURE.md §8.3.
 */
if (s->reassembly_len + n > s->opt_max_continuation_size)
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);

/*
 * SECURITY: enforce HPACK bomb limit incrementally.
 * Stream error (RST_STREAM) - session continues after this.
 * Checked after each header, before on_header fires.
 * Size per RFC 7541 §4.1: name_len + value_len + 32 per entry.
 * Do NOT return early here. Set stream_error_pending and continue
 * decoding to the end of the block - aborting mid-block desynchronizes
 * the dynamic table (RFC 7541 §2.3.2), corrupting all subsequent HPACK
 * on this connection. RST_STREAM is sent after the full block is consumed.
 * See ARCHITECTURE.md §4.5 and §8.2.
 */
decoded_size += name_len + value_len + 32;
if (decoded_size > s->opt_max_header_list_size)
	stream_error_pending = 1;  /* suppress delivery, continue decoding */

/*
 * SECURITY: stream ID monotonicity check.
 * Connection error per RFC 9113 §5.1.1.
 */
if (stream_id <= s->last_stream_id_remote)
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);

/*
 * SECURITY: receive-side flow control enforcement.
 * Peer exceeded the receive window. Stream error for stream-level;
 * connection error for connection-level. See ARCHITECTURE.md §8.7.
 */
if (n > stream->recv_window)
	return stream_error(s, stream_id, HIVE_ERR_FLOW_CONTROL,
	    HIVE_H2_FLOW_CONTROL_ERROR);
```

### 3.3 Integer Safety

Use fixed-width types for all protocol fields and size calculations. Check
for overflow before arithmetic on values from network input.

```c
/* Use sized types for all protocol fields */
uint8_t  frame_type;
uint32_t stream_id;
uint32_t payload_length;   /* 24-bit on wire, promoted to uint32_t */
int32_t  send_window;      /* signed - window can temporarily go negative */

/* Big-endian conversion is mandatory for all multi-byte wire fields */

/* Writing (3-byte length field in frame header) */
p[0] = (length >> 16) & 0xFF;
p[1] = (length >> 8)  & 0xFF;
p[2] =  length        & 0xFF;

/* Reading (4-byte stream_id field, R bit masked) */
memcpy(&raw, src + 5, 4);
stream_id = ntohl(raw) & 0x7FFFFFFFu;

/* Overflow check before window arithmetic */
if ((int64_t)stream->send_window + (int64_t)increment > 0x7FFFFFFFLL)
	return stream_error(s, stream_id, HIVE_ERR_FLOW_CONTROL,
	    HIVE_H2_FLOW_CONTROL_ERROR);
stream->send_window += (int32_t)increment;
```

Never read a multi-byte integer from a wire buffer without byte-order
conversion. Use `memcpy` to move bytes into a local variable before
conversion - never dereference an unaligned pointer from the wire buffer.

### 3.4 Memory Safety

**No unsafe string functions**:
```c
/* BANNED - never use these */
strcpy(dst, src);
strcat(dst, src);
sprintf(buf, fmt, ...);   /* use snprintf */
gets(buf);                /* never */
strncpy(dst, src, n);     /* does not guarantee termination */
```

```c
/* Required alternatives */
if (src_len >= sizeof(dst))
	return HIVE_ERR_INVALID_ARG;
memcpy(dst, src, src_len + 1u);
n = snprintf(buf, sizeof(buf), "%u", value);
if (n < 0 || (size_t)n >= sizeof(buf))
	return HIVE_ERR_INVALID_ARG;   /* truncation is an error */
```

**Null pointer on every allocator call** - see §2.4.

**NULL after free**:
```c
s->mem.free(entry, s->mem.ctx);
entry = NULL;   /* prevent accidental use-after-free */
```

**Bounds before memcpy** - never copy from a network buffer without
verifying that `offset + n ≤ buffer_len` first:
```c
if (offset + name_len > buf_len)
	return HIVE_ERR_PROTOCOL;
memcpy(out->name, buf + offset, name_len);
offset += name_len;
```

### 3.5 hive_buf_t Lifetime Enforcement

The `hive_buf_t` lifetime contract is enforced at two levels: contractual
in all builds, and mechanically in `HIVE_DEBUG` builds via ASan.

**`on_header` receives `hive_buf_t *` - handles passed by pointer**:

```c
int (*on_header)(hive_session_t *session,
    uint32_t stream_id,
    hive_buf_t *name, hive_buf_t *value,
    uint8_t flags, void *user_data);
```

The library owns the `hive_buf_t` objects. The callback receives pointers
to those objects. After the callback returns, the library clears
`HIVE_BUF_VALID` in the actual handles:

```c
/*
 * SECURITY: invalidate hive_buf_t handles on callback return.
 * Operates on the library's own objects via the pointers passed to the
 * callback - not on copies. Any stale pointer the callback retained now
 * dereferences an invalidated handle (HIVE_BUF_VALID cleared).
 * In HIVE_DEBUG builds, the underlying data is also ASan-poisoned.
 * See ARCHITECTURE.md §8.8.
 */
name->flags  &= ~HIVE_BUF_VALID;
value->flags &= ~HIVE_BUF_VALID;
HIVE_ASAN_POISON(name->data,  name->len);   /* only for non-indexed data */
HIVE_ASAN_POISON(value->data, value->len);  /* only for non-indexed data */
```

**Why by-pointer matters**: if `on_header` took `hive_buf_t name,
hive_buf_t value` (by value), the callback would hold its own copies of the
structs - clearing the library's local copy after the call would have no
effect on the caller's copies. By passing pointers to library-owned objects,
flag invalidation is effective. This is a fundamental design constraint; do
not change the callback signature to pass by value.

**What the callback may and may not do**:

```c
/* Correct - read within the callback */
static int
my_on_header(hive_session_t *s, uint32_t sid,
    hive_buf_t *name, hive_buf_t *value, uint8_t flags, void *ud)
{
	/* Read directly - valid within callback */
	if (name->len == 5 && memcmp(name->data, ":path", 5) == 0) {
		/* process path... */
	}
	return HIVE_OK;
}

/* Correct - retain for use beyond the callback */
static int
my_on_header(hive_session_t *s, uint32_t sid,
    hive_buf_t *name, hive_buf_t *value, uint8_t flags, void *ud)
{
	my_request_t *req = (my_request_t *)ud;
	if (name->len == 5 && memcmp(name->data, ":path", 5) == 0) {
		/* hive_buf_retain() copies data to arena memory */
		if (hive_buf_retain(s, value) != HIVE_OK)
			return HIVE_ERR_NOMEM;
		req->path_buf = *value;   /* copy the struct - now HIVE_BUF_OWNED */
	}
	return HIVE_OK;
}

/* WRONG - storing raw data pointer without retaining */
static int
my_on_header(hive_session_t *s, uint32_t sid,
    hive_buf_t *name, hive_buf_t *value, uint8_t flags, void *ud)
{
	my_request_t *req = (my_request_t *)ud;
	req->path = value->data;   /* WRONG - dangling pointer after callback */
	return HIVE_OK;
}
```

**ASan poisoning in `HIVE_DEBUG` builds**:

For non-Huffman headers pointing into `reassembly_buf`, and for Huffman-
decoded headers pointing into `hpack_scratch_name` or `hpack_scratch_value`,
the data regions are poisoned via `HIVE_ASAN_POISON`. Any access through a
stale pointer after callback return produces an immediate ASan
`heap-use-after-poison` abort.

For indexed headers (pointers into the static table or live dynamic table
entries), the data must **not** be poisoned - those regions are reused
across many callbacks. The `HIVE_BUF_VALID` flag clearing is sufficient
enforcement for indexed data.

After `hive_buf_retain()` is called within the callback, the copied data is
in arena memory and is not poisoned.

**`on_data_chunk` lifetime**:

The `data` pointer is directly into the caller's input buffer passed to
`hive_session_recv()`. Valid only within the callback. The library does not
own the caller's buffer and does not poison it. Copy if you need the data
beyond callback return.

### 3.6 Resource Cleanup with goto

Use the `goto cleanup` pattern for functions that acquire multiple
resources. This ensures cleanup on every error path without duplication.

```c
static int
session_prealloc(hive_session_t *s)
{
	int ret = HIVE_ERR_NOMEM;

	s->stream_hash = s->mem.calloc(
	    s->hash_table_size, sizeof(stream_hash_entry_t), s->mem.ctx);
	if (s->stream_hash == NULL)
		goto cleanup;

	s->stream_slots = s->mem.calloc(
	    s->opt_max_concurrent_streams, sizeof(hive_stream_t), s->mem.ctx);
	if (s->stream_slots == NULL)
		goto cleanup;

	s->send_buf = s->mem.malloc(s->send_buf_cap, s->mem.ctx);
	if (s->send_buf == NULL)
		goto cleanup;

	/* ... remaining pre-allocations ... */

	ret = HIVE_OK;

cleanup:
	if (ret != HIVE_OK)
		session_prealloc_free(s);   /* frees any non-NULL sub-buffers */
	return ret;
}
```

This pattern is mandatory for session creation. If any pre-allocation fails,
all previously allocated sub-buffers are freed before returning NULL to the
caller - no partial initialisation is leaked.

---

## 4. Error Handling

### 4.1 Error Returns

Every function that can fail returns an error code. Callers must check
return values. `void` return is permitted only for functions that cannot
fail (e.g. pure computations, no allocation).

```c
/* Public API functions return HIVE_ERR_* or HIVE_OK (0) on success */
int ret = hive_submit_response(session, stream_id, nva, nvlen, &src);
if (ret != HIVE_OK) {
	/* handle error */
}

/* Internal functions follow the same convention */
if (stream_open(s, stream_id, HIVE_STREAM_OPEN) != HIVE_OK)
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);
```

Do not silently discard return values. If a return value is intentionally
ignored, cast to `(void)`:
```c
(void)clock_gettime(CLOCK_MONOTONIC, &ts);  /* cannot fail in practice */
```

### 4.2 Session Errors vs Stream Errors

HTTP/2 distinguishes connection errors (affect the whole session) from
stream errors (affect one stream). Use the correct error helper:

```c
/*
 * Connection error: fire on_connection_error, queue GOAWAY, close session.
 * Used for: invalid frame on stream 0, HPACK decoding failure (COMPRESSION_ERROR),
 *   CONTINUATION lockout violation, SETTINGS flood, flow-control overflow
 *   at connection level, unsolicited SETTINGS ACK.
 * See RFC 9113 §5.4.1.
 *
 * After session_error() returns, the session_state is HIVE_SESSION_CLOSED
 * and a GOAWAY frame has been queued in the send buffer. The caller of
 * hive_session_recv() will receive HIVE_ERR_* from that call. The caller
 * MUST then call hive_session_send() exactly once more to attempt to flush
 * the queued GOAWAY to the peer, then call hive_session_free(). No other
 * calls are valid after a fatal hive_session_recv() error return.
 */
static int
session_error(hive_session_t *s, int hive_err,
    uint32_t h2_error_code, uint32_t last_stream_id)
{
	if (s->callbacks.on_connection_error != NULL)
		s->callbacks.on_connection_error(s, hive_err, h2_error_code,
		    s->user_data);
	hive_submit_goaway_final(s, h2_error_code, NULL, 0);
	s->session_state = HIVE_SESSION_CLOSED;
	return hive_err;
}

/*
 * Stream error: send RST_STREAM for stream_id only. Session continues.
 * Used for: HPACK bomb limit exceeded, flow-control overflow at stream level,
 *   invalid stream-level state transitions, stream-specific protocol errors.
 * See RFC 9113 §5.4.2.
 */
static int
stream_error(hive_session_t *s, uint32_t stream_id,
    int hive_err, uint32_t h2_error_code)
{
	hive_submit_rst_stream(s, stream_id, h2_error_code);
	return hive_err;
}
```

RFC 9113 §5.4 determines which errors are connection errors and which are
stream errors. When in doubt, the RFC takes precedence. Do not promote
stream errors to connection errors - that terminates the entire connection
unnecessarily and will cause h2spec conformance failures.

**Classification table** - errors that are commonly misclassified:

| Condition | Error class | Reason |
|---|---|---|
| CONTINUATION lockout violation | Connection | RFC 9113 §4.3 |
| HPACK COMPRESSION_ERROR | Connection | RFC 9113 §4.3 |
| HPACK bomb (size/count limit) | Stream | RST_STREAM, session continues |
| Frame on stream 0 requiring stream_id > 0 | Connection | RFC 9113 §5 |
| Frame on idle stream (not HEADERS) | Connection | RFC 9113 §5.1 |
| Flow control overflow (connection) | Connection | RFC 9113 §6.9.1 |
| Flow control overflow (stream) | Stream | RFC 9113 §6.9.1 |
| SETTINGS flood | Connection | RFC 9113 §6.5 |
| Unsolicited SETTINGS ACK | Connection | RFC 9113 §6.5 |
| Invalid stream state for DATA | Stream | RST_STREAM STREAM_CLOSED |

### 4.3 No Assertions on Network Input

`assert()` must never fire on data from the network. Assertions are for
programmer errors (invariants that must hold given correct internal logic),
not for protocol violations from peers.

```c
/* Correct - internal invariant: programmer error if violated */
if (s->frame_hdr_count > 9) {
	assert(0 && "frame_hdr_count > 9: impossible internal state");
}

/* Correct - protocol violation from peer: error return, not assert */
if (stream_id == 0 && frame_type == HIVE_FRAME_DATA) {
	/* DATA on stream 0 is invalid per RFC 9113 §6.1 */
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);
}
```

The rule: if the condition can be caused by a peer sending malformed data,
it is an error return, never an assertion.

---

## 5. Documentation

### 5.1 Function Comments

Every non-trivial public function requires a comment block describing what
it does, its parameters, return value, and any lifetime or threading
constraints. Internal static helpers require a brief comment unless the
function name is entirely self-explanatory.

```c
/*
 * Decode a complete HEADERS block from reassembly_buf.
 *
 * Fires on_begin_headers, then one on_header per decoded pair (by pointer),
 * then on_headers_complete. Enforces HPACK bomb limits (decoded_size and
 * decoded_count) incrementally before each on_header call.
 *
 * data:   compressed header block bytes (from reassembly_buf)
 * len:    length of the block in bytes
 *
 * Returns HIVE_OK on success.
 * Returns HIVE_ERR_COMPRESSION on fatal HPACK error (GOAWAY sent).
 * Returns HIVE_ERR_PROTOCOL if bomb limit exceeded (RST_STREAM sent).
 *
 * Not thread-safe - called only within hive_session_recv().
 */
static int
hpack_decode_block(hive_session_t *s, const uint8_t *data, size_t len)
```

### 5.2 Architecture Reference Comments

When implementing protocol or state machine logic, reference the relevant
section of ARCHITECTURE.md and the RFC.

```c
/*
 * CONTINUATION frame lockout - connection error per RFC 9113 §4.3.
 * Uses reassembly_active, not reassembly_len, to correctly handle
 * zero-length HEADERS blocks. See ARCHITECTURE.md §3.5.
 */
if (s->reassembly_active &&
    (cur_frame.type != HIVE_FRAME_CONTINUATION ||
     cur_frame.stream_id != s->reassembly_stream_id)) {
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);
}
if (!s->reassembly_active && cur_frame.type == HIVE_FRAME_CONTINUATION) {
	return session_error(s, HIVE_ERR_PROTOCOL,
	    HIVE_H2_PROTOCOL_ERROR, 0);
}

/*
 * Knuth multiplicative hash for stream ID lookup.
 * See ARCHITECTURE.md §5.4.
 */
static uint32_t
stream_hash_fn(uint32_t stream_id, uint32_t hash_mask)
{
	return (stream_id * 2654435761u) >> (32 - __builtin_popcount(hash_mask));
}

/*
 * SETTINGS directionality: inbound frames validated against local_settings
 * (what we advertised); outbound frames sized to remote_settings (what peer
 * advertised). See ARCHITECTURE.md §2.5.
 */
```

### 5.3 SECURITY Comments

See §3.2 for the `/* SECURITY: ... */` comment convention. Every security
check in the protocol path must have one. These comments must not be removed
without understanding why the check exists and verifying the RFC permits it.

### 5.4 Deferred-Work Comment Markers

```c
/* NOTE: hash index for large HPACK tables (see ARCHITECTURE.md §4.9) */
/* NOTE: send_window is int32_t - can temporarily go negative after
 *       retroactive adjustment on SETTINGS_INITIAL_WINDOW_SIZE change */
/* NOTE: HIVE_OK == 0 and HIVE_H2_NO_ERROR == 0x0 share the same value
 *       but serve different purposes: HIVE_OK is a library return code,
 *       HIVE_H2_NO_ERROR is an HTTP/2 wire error code for GOAWAY/RST_STREAM */
```

---

## 6. Pre-Commit Checklist

Before every commit:

- [ ] Compiles without warnings on Linux (`-Wall -Wextra -Wpedantic`)
- [ ] Compiles without warnings on OpenBSD (`-Wall -Wextra -Wpedantic`)
- [ ] All tests pass (`make test`)
- [ ] Valgrind clean on Linux (`make valgrind`)
- [ ] ASan/UBSan clean on both platforms (`make dev && make test`)
- [ ] clang-format clean (`make lint`)
- [ ] No direct `malloc`/`free`/`calloc`/`realloc` calls in `src/` except
      the NULL-allocator shim in `hive.c` - verify with:
      `grep -n "malloc\|calloc\|realloc\|free" src/*.c | grep -v null_alloc`
- [ ] Every allocator call checks for NULL return
- [ ] `SECURITY:` comment present on every new security check
- [ ] CONTINUATION flood exceeding `opt_max_continuation_size` uses
      `session_error()` (connection error / GOAWAY), not `stream_error()`
- [ ] HPACK bomb limit exceeded uses `stream_error()` (RST_STREAM),
      not `session_error()`
- [ ] Inbound frame `length` validated against `local_settings.max_frame_size`;
      outbound frame sizing respects `remote_settings.max_frame_size`
      (see ARCHITECTURE.md §2.5 - getting these backwards is a protocol bug)
- [ ] New protocol validation uses `session_error()` or `stream_error()`
      with the correct error class per §4.2 classification table
- [ ] No `assert()` on any condition reachable via network input
- [ ] `on_header` callback signature uses `hive_buf_t *name, hive_buf_t *value`
      (by pointer, not by value) - do not store `name->data` or `value->data`
      beyond callback without calling `hive_buf_retain()` first
- [ ] `send` callback correctly returns `ssize_t` (bytes written, 0..total),
      not `int` - partial writes are normal, not errors
- [ ] `HIVE_OK` used for library success returns; `HIVE_H2_NO_ERROR` used for
      wire error code argument to GOAWAY/RST_STREAM - never mix
- [ ] Big-endian conversion on every multi-byte wire field read and write
- [ ] `_Static_assert` in place for any new struct whose size matters
- [ ] New public functions have doc comment blocks
- [ ] New protocol logic has ARCHITECTURE.md section cross-reference
- [ ] No deferred-work marker added without a comment explaining the issue

**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, DEVELOPMENT.md
