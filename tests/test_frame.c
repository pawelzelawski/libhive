/*
 * test_frame.c — Phase 2.2 frame header serialisation tests
 *
 * Covers DEVELOPMENT.md Task 2.2: verify that frame_hdr_write_at()
 * produces the exact 9-byte wire output for known inputs.
 *
 * Tests use frame_hdr_write_at() directly (no hive_session_t required).
 * See ARCHITECTURE.md §6.2 for the expected wire format.
 * RFC 9113 §4.1: https://www.rfc-editor.org/rfc/rfc9113#section-4.1
 */

#include <stdint.h>
#include <string.h>

#include "../src/hive_frame_bare.h"
#include "test_harness.h"

int test_frame_hdr_write_data(void);
int test_frame_hdr_write_settings(void);
int test_frame_hdr_write_headers(void);

/*
 * DATA frame: stream_id=1, length=100 (0x000064), flags=0x01 (END_STREAM).
 *
 * Expected wire bytes (RFC 9113 §4.1):
 *   [0x00, 0x00, 0x64]  — length 100, big-endian 24-bit
 *   [0x00]              — type DATA (0x0)
 *   [0x01]              — flags END_STREAM
 *   [0x00, 0x00, 0x00, 0x01] — stream_id 1, 31-bit big-endian (R=0)
 */
int
test_frame_hdr_write_data(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x64, /* length = 100 */
		0x00,             /* type   = DATA */
		0x01,             /* flags  = END_STREAM */
		0x00, 0x00, 0x00, 0x01 /* stream_id = 1 */
	};

	frame_hdr_write_at(buf, 100, HIVE_FRAME_DATA, HIVE_FLAG_END_STREAM, 1);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

/*
 * SETTINGS ACK frame: stream_id=0, length=0, flags=0x01 (ACK).
 *
 * Expected wire bytes:
 *   [0x00, 0x00, 0x00]  — length 0
 *   [0x04]              — type SETTINGS (0x4)
 *   [0x01]              — flags ACK
 *   [0x00, 0x00, 0x00, 0x00] — stream_id 0 (connection-level)
 */
int
test_frame_hdr_write_settings(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x00, /* length = 0 */
		0x04,             /* type   = SETTINGS */
		0x01,             /* flags  = ACK */
		0x00, 0x00, 0x00, 0x00 /* stream_id = 0 */
	};

	frame_hdr_write_at(buf, 0, HIVE_FRAME_SETTINGS, HIVE_FLAG_ACK, 0);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

/*
 * HEADERS frame: stream_id=3, length=20 (0x000014), flags=END_HEADERS(0x04).
 *
 * Expected wire bytes:
 *   [0x00, 0x00, 0x14]  — length 20
 *   [0x01]              — type HEADERS (0x1)
 *   [0x04]              — flags END_HEADERS
 *   [0x00, 0x00, 0x00, 0x03] — stream_id 3
 */
int
test_frame_hdr_write_headers(void)
{
	uint8_t buf[9];
	const uint8_t expected[9] = {
		0x00, 0x00, 0x14, /* length = 20 */
		0x01,             /* type   = HEADERS */
		0x04,             /* flags  = END_HEADERS */
		0x00, 0x00, 0x00, 0x03 /* stream_id = 3 */
	};

	frame_hdr_write_at(buf, 20, HIVE_FRAME_HEADERS, HIVE_FLAG_END_HEADERS,
	    3);
	ASSERT(memcmp(buf, expected, 9) == 0);
	return 1;
}

