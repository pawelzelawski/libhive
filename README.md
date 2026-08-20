# libhive

libhive is a standalone HTTP/2 protocol engine in C11 using POSIX interfaces,
supported on Linux and OpenBSD.
It implements framing, stream state management, HPACK, flow control, push,
GOAWAY, and h2c, while doing zero I/O: you feed received bytes into
`hive_session_recv()`, submit work with `hive_submit_*()`, and flush queued
frames with `hive_session_send()` through your own transport layer.

## Requirements

- C11 compiler and POSIX interfaces (the default `Makefile` uses `clang`)
- No runtime dependency beyond the platform C/POSIX environment for the core
  static library

## Build and Install

```bash
make dev
make install
```

Optional install prefix:

```bash
make install PREFIX=/opt/libhive
```

## Minimal Integration Example (Server Role)

The snippet below shows the event-loop pattern from `ARCHITECTURE.md` §10 in
minimal form: read -> recv -> submit (inside callbacks) -> send.

```c
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hive.h"

static ssize_t
transport_send(hive_session_t *s, const struct iovec *iov, int iovcnt, void *ud)
{
	size_t i;
	ssize_t total;

	(void)s;
	(void)ud;
	total = 0;
	for (i = 0; i < (size_t)iovcnt; i++)
		total += (ssize_t)iov[i].iov_len;
	return total; /* full write */
}

static ssize_t
transport_read(uint8_t *buf, size_t cap)
{
	(void)buf;
	(void)cap;
	return 0; /* no inbound bytes in this tiny runnable example */
}

int
main(void)
{
	hive_session_t *session;
	hive_callbacks_t cb;
	uint8_t inbuf[4096];
	ssize_t nread;

	memset(&cb, 0, sizeof(cb));
	cb.send = transport_send;

	session = hive_session_server_new(NULL, NULL, &cb, NULL);
	if (session == NULL)
		return 1;

	nread = transport_read(inbuf, sizeof(inbuf));
	if (nread > 0)
		(void)hive_session_recv(session, inbuf, (size_t)nread);

	if (hive_session_want_write(session))
		(void)hive_session_send(session);

	hive_session_free(session);
	return 0;
}
```

Build and run this example locally against the built static library:

```bash
make release
clang -std=c11 -Wall -Wextra -Wpedantic -Iinclude \
  /tmp/libhive_example.c build/rel/libhive.a -o /tmp/libhive_example
/tmp/libhive_example
```

## Known Limitations

- Zero I/O by design: sockets/TLS/event-loop ownership is entirely caller-side
- Not thread-safe per session (`hive_session_recv/send/submit/free` must not race)
- `hive_session_recv()` is non-reentrant and never performs transport I/O or
  invokes a DATA-source read callback. When its bounded output queue is full,
  drain with `hive_session_send()` and re-feed the unconsumed input.
- Single-session configuration is fixed at creation (no live option mutation)
- Out of scope in v1: HTTP/3, RFC 8441 extended CONNECT/WebSocket, RFC 9218

## More Documentation

- Full design and state machines: `ARCHITECTURE.md`
- Build and toolchain details: `TECH_STACK.md`
- Coding rules and lifetime contracts: `CODING_STANDARDS.md`
- Test strategy and gates: `TESTING.md`

## License

[ISC](LICENSE)
