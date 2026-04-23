/*
 * hive_frame_bare.c — standalone frame header serialisation and parsing
 *
 * No dependency on hive_session_t, callbacks, or stream state.
 * Shared between libhive.a (frame parser and send path) and the
 * tools/hive_decode binary (frame decoder tool).
 *
 * See ARCHITECTURE.md §6.2 for the exact wire format.
 * RFC 9113 §4.1: https://www.rfc-editor.org/rfc/rfc9113#section-4.1
 */

#include "hive_frame_bare.h"

/*
 * Serialise one 9-byte HTTP/2 frame header at dst.
 *
 * Big-endian encoding per RFC 9113 §4.1:
 *   Bytes 0–2: 24-bit payload length
 *   Byte  3:   frame type
 *   Byte  4:   flags
 *   Bytes 5–8: R bit (0) + 31-bit stream identifier
 *
 * See ARCHITECTURE.md §6.2 for the canonical implementation reference.
 */
void
frame_hdr_write_at(uint8_t *dst, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id)
{
	dst[0] = (uint8_t)((length >> 16) & 0xFFu);
	dst[1] = (uint8_t)((length >> 8) & 0xFFu);
	dst[2] = (uint8_t)(length & 0xFFu);
	dst[3] = type;
	dst[4] = flags;
	dst[5] = (uint8_t)((stream_id >> 24) & 0x7Fu); /* R bit cleared */
	dst[6] = (uint8_t)((stream_id >> 16) & 0xFFu);
	dst[7] = (uint8_t)((stream_id >> 8) & 0xFFu);
	dst[8] = (uint8_t)(stream_id & 0xFFu);
}

/*
 * Parse one 9-byte HTTP/2 frame header from src into *out.
 *
 * The reserved R bit in bytes 5–8 is masked off when populating
 * out->stream_id. No validation is performed here.
 *
 * See ARCHITECTURE.md §3.2 for the frame_hdr_t field definitions.
 */
void
frame_hdr_parse(const uint8_t *src, frame_hdr_t *out)
{
	out->length = ((uint32_t)src[0] << 16) |
	    ((uint32_t)src[1] << 8) |
	    (uint32_t)src[2];
	out->type   = src[3];
	out->flags  = src[4];
	out->stream_id = (((uint32_t)src[5] & 0x7Fu) << 24) |
	    ((uint32_t)src[6] << 16) |
	    ((uint32_t)src[7] << 8) |
	    (uint32_t)src[8];
}

