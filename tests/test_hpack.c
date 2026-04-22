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
int test_huffman_eos_rejected(void);
int test_huffman_invalid_padding(void);

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
	static const uint8_t in[] = { 0xff, 0xff, 0xff, 0xff };
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

