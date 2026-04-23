/*
 * hive_frame_bare.h — standalone frame header serialisation and parsing
 *
 * Declares frame_hdr_write_at() and frame_hdr_parse() — functions with
 * no dependency on hive_session_t, callbacks, or stream state.  Used by
 * both libhive.a (via hive_frame.c and hive_send.c) and the standalone
 * tools/hive_decode binary.
 *
 * See ARCHITECTURE.md §6.2 for the wire format and the exact serialisation
 * logic that frame_hdr_write_at() implements.
 * RFC 9113 §4.1: https://www.rfc-editor.org/rfc/rfc9113#section-4.1
 *
 * Not included by embedders — internal to the library only.
 */

#ifndef HIVE_FRAME_BARE_H
#define HIVE_FRAME_BARE_H

#include <stdint.h>

#include "hive_frame.h"

/*
 * frame_hdr_write_at — serialise a 9-byte HTTP/2 frame header into dst.
 *
 * Wire layout written (RFC 9113 §4.1):
 *   dst[0..2] : 24-bit payload length, big-endian
 *   dst[3]    : frame type
 *   dst[4]    : flags
 *   dst[5..8] : reserved bit (R=0) + 31-bit stream_id, big-endian
 *
 * dst must point to at least 9 writable bytes.
 * length must be a 24-bit value (≤ 0xFFFFFF); the caller is responsible
 * for enforcing this before queuing the frame.
 * stream_id bits 31..28 are silently masked off (R bit cleared).
 *
 * Does not advance any cursor. The caller decides where to write.
 * See ARCHITECTURE.md §6.2.
 */
void frame_hdr_write_at(uint8_t *dst, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id);

/*
 * frame_hdr_parse — parse a 9-byte HTTP/2 frame header from src into *out.
 *
 * src must point to exactly 9 readable bytes.
 * The reserved R bit in the stream_id field is masked off.
 * No validation is performed — the caller must validate the parsed fields
 * (length, type, stream_id parity, etc.) per the receive state machine in
 * ARCHITECTURE.md §3.3.
 */
void frame_hdr_parse(const uint8_t *src, frame_hdr_t *out);

#endif /* HIVE_FRAME_BARE_H */

