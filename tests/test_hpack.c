/*
 * test_hpack.c - Phase 1.4 HPACK static and Huffman tests
 *
 * Covers DEVELOPMENT.md Task 1.4 requirements only.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../src/hive_hpack.h"
#include "test_harness.h"

int test_static_table_size(void);
int test_static_table_index1(void);
int test_static_table_index2(void);
int test_static_table_index61(void);
int test_huffman_decode_empty(void);
int test_huffman_decode_www(void);
int test_huffman_encode_decode_roundtrip(void);
int test_huffman_roundtrip_long_codes(void);
int test_huffman_eos_rejected(void);
int test_huffman_invalid_padding(void);
int test_huffman_decode_truncated_long_code(void);

int
test_static_table_size(void)
{
	size_t i;

	ASSERT(HPACK_STATIC_TABLE_SIZE == 61);
	for (i = 0; i < HPACK_STATIC_TABLE_SIZE; i++) {
		ASSERT(hpack_static_table[i].name != NULL);
		ASSERT(hpack_static_table[i].name_len > 0);
	}
	return 1;
}

int
test_static_table_index1(void)
{
	const hive_nv_t *nv;

	nv = &hpack_static_table[0];
	ASSERT(nv->name_len == 10);
	ASSERT(memcmp(nv->name, ":authority", 10) == 0);
	ASSERT(nv->value == NULL);
	ASSERT(nv->value_len == 0);
	return 1;
}

int
test_static_table_index2(void)
{
	const hive_nv_t *nv;

	nv = &hpack_static_table[1];
	ASSERT(nv->name_len == 7);
	ASSERT(memcmp(nv->name, ":method", 7) == 0);
	ASSERT(nv->value_len == 3);
	ASSERT(memcmp(nv->value, "GET", 3) == 0);
	return 1;
}

int
test_static_table_index61(void)
{
	const hive_nv_t *nv;

	nv = &hpack_static_table[60];
	ASSERT(nv->name_len == 16);
	ASSERT(memcmp(nv->name, "www-authenticate", 16) == 0);
	ASSERT(nv->value == NULL);
	ASSERT(nv->value_len == 0);
	return 1;
}

int
test_huffman_decode_empty(void)
{
	uint8_t out[16];
	size_t out_len;
	int ret;

	ret = huff_decode(NULL, 0, out, sizeof(out), &out_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(out_len == 0);
	return 1;
}

int
test_huffman_decode_www(void)
{
	static const uint8_t in[] = {
		0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0,
		0xab, 0x90, 0xf4, 0xff,
	};
	uint8_t out[64];
	size_t out_len;
	int ret;

	ret = huff_decode(in, sizeof(in), out, sizeof(out), &out_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(out_len == 15);
	ASSERT(memcmp(out, "www.example.com", 15) == 0);
	return 1;
}

int
test_huffman_encode_decode_roundtrip(void)
{
	static const uint8_t src[] = "no-cache";
	uint8_t enc[64];
	uint8_t dec[64];
	size_t enc_len;
	size_t dec_len;
	int ret;

	ret = huff_encode(src, sizeof(src) - 1, enc, sizeof(enc), &enc_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(enc_len > 0);

	ret = huff_decode(enc, enc_len, dec, sizeof(dec), &dec_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(dec_len == sizeof(src) - 1);
	ASSERT(memcmp(dec, src, dec_len) == 0);
	return 1;
}

int
test_huffman_eos_rejected(void)
{
	/*
	 * Drive the EOS-detection branch in huff_decode, not the
	 * leftover-bits guard. Encode 'a' (5 bits = 00011), then append
	 * the EOS code (30 ones), then 5 padding ones to reach a byte
	 * boundary. Total 40 bits = 5 bytes, all 11s after the leading
	 * 5-bit 'a' code:
	 *   00011 + 30*1 + 5*1 = 00011 11111111 11111111 11111111 11111111
	 *   = 0x1f 0xff 0xff 0xff 0xff
	 * The decoder must consume 'a', then enter the slow path on the
	 * 0xff prefix, where huff_decode_long matches the 30-bit EOS code
	 * and returns HIVE_ERR_COMPRESSION (RFC 7541 §5.2 — EOS in a
	 * non-terminal position is a decoding error).
	 */
	static const uint8_t in[] = { 0x1f, 0xff, 0xff, 0xff, 0xff };
	uint8_t out[16];
	size_t out_len;
	int ret;

	ret = huff_decode(in, sizeof(in), out, sizeof(out), &out_len);
	ASSERT(ret == HIVE_ERR_COMPRESSION);
	return 1;
}

int
test_huffman_invalid_padding(void)
{
	static const uint8_t in[] = { 0x00 };
	uint8_t out[16];
	size_t out_len;
	int ret;

	ret = huff_decode(in, sizeof(in), out, sizeof(out), &out_len);
	ASSERT(ret == HIVE_ERR_COMPRESSION);
	return 1;
}

/*
 * test_huffman_roundtrip_long_codes — exercise the >8-bit code path.
 *
 * The decoder's 256-entry fast-path table only handles codes up to
 * 8 bits. HPACK Huffman defines codes from 5 to 30 bits (RFC 7541
 * Appendix B); the implementation must dispatch to the slow path for
 * any input containing a code with bit-length > 8.
 *
 * The byte '|' (0x7c) has the 11-bit code 0x7fc — encoding "a|" yields
 * exactly two bytes (0x1f 0xfc) which fit no fast-path entry. Without
 * the slow path this test fails (huff_decode returns HIVE_ERR_COMPRESSION
 * via the leftover-bits guard with out_len == 0).
 *
 * Regression for the audit finding "huff_decode cannot decode any HPACK
 * code longer than 8 bits".
 */
int
test_huffman_roundtrip_long_codes(void)
{
	static const uint8_t src[] = "a|b\\c'?";
	uint8_t enc[64];
	uint8_t dec[64];
	size_t enc_len;
	size_t dec_len;
	int ret;

	ret = huff_encode(src, sizeof(src) - 1, enc, sizeof(enc), &enc_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(enc_len > 0);

	ret = huff_decode(enc, enc_len, dec, sizeof(dec), &dec_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(dec_len == sizeof(src) - 1);
	ASSERT(memcmp(dec, src, dec_len) == 0);
	return 1;
}

/*
 * test_huffman_decode_truncated_long_code — slow path must reject input
 * that ends mid-long-code instead of looping or returning success.
 *
 * Encode '|' (11-bit code 0x7fc) on its own: bytes 0xff 0xff (the second
 * byte is padding ones to fill the partial code's tail). Then truncate
 * to a single 0xff byte: that's only 8 bits, not enough to identify any
 * code, and the leftover-bits guard must reject it.
 */
int
test_huffman_decode_truncated_long_code(void)
{
	static const uint8_t in[] = { 0xff };
	uint8_t out[16];
	size_t out_len;
	int ret;

	ret = huff_decode(in, sizeof(in), out, sizeof(out), &out_len);
	ASSERT(ret == HIVE_ERR_COMPRESSION);
	return 1;
}

