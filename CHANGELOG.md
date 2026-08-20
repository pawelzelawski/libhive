# Changelog

All notable changes to libhive are documented here.

## Unreleased

## v1.1.0 — 2026-08-20

- Hardened HPACK memory safety, GOAWAY reassembly, SETTINGS flood accounting,
  receive-side backpressure, HTTP messaging validation, and HPACK encoder
  transactions.
- Published stream-state values, declared 64-bit POSIX Linux/OpenBSD support,
  removed the unused `strlcpy`/`strlcat` compatibility layer, and expanded
  reproducible Linux/OpenBSD validation coverage.

## v1.0.1 — 2026-04-30

- Fixed send-queue iovec-offset handling after automatic flushes.
- Made mandatory receive-side control-frame enqueue failures fail closed.
- Added regressions for those queue-failure paths.

## v1.0.0 — 2026-04-29

- Initial public release of the standalone HTTP/2 framing and HPACK engine.
