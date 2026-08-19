# Changelog

All notable changes to libhive are documented here.

## Unreleased

- Security fixes and release-preparation work are under review on `dev`.
  Nothing in this section has been published as a release.

## v1.0.1 — 2026-04-30

- Fixed send-queue iovec-offset handling after automatic flushes.
- Made mandatory receive-side control-frame enqueue failures fail closed.
- Added regressions for those queue-failure paths.

## v1.0.0 — 2026-04-29

- Initial public release of the standalone HTTP/2 framing and HPACK engine.
