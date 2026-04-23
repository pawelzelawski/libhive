/*
 * test_hpack.c - HPACK static table, Huffman, dynamic table and varint tests
 *
 * Phase 1.4: static table and Huffman tests.
 * Phase 3.1: dynamic table (hpack_table_t) tests.
 * Phase 3.2: integer varint encode/decode tests.
 * Phase 3.3: string encode/decode tests.
 *
 * See DEVELOPMENT.md tasks 1.4, 3.1, 3.2 and 3.3.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hive_hpack.h"
#include "../src/hive_internal.h"
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

/* Phase 3.1 — dynamic table */
int test_hpack_table_insert_basic(void);
int test_hpack_table_evict_on_insert(void);
int test_hpack_table_evict_to_zero(void);
int test_hpack_table_rfc_size(void);
int test_hpack_table_oversized_entry(void);
int test_hpack_always_copy(void);

/* Phase 3.2 — integer varint encode/decode */
int test_hpack_int_decode_1byte(void);
int test_hpack_int_decode_multibyte(void);
int test_hpack_int_decode_truncated(void);
int test_hpack_int_decode_overflow(void);
int test_hpack_int_encode_decode_roundtrip(void);

/* Phase 3.3 — string encode/decode */
int test_hpack_string_decode_literal(void);
int test_hpack_string_decode_huffman(void);
int test_hpack_string_encode_huffman(void);
int test_hpack_string_scratch_limit(void);
int test_hpack_string_truncated(void);

/* Phase 3.4 — full decoder */
int test_hpack_decode_rfc_c3(void);
int test_hpack_decode_rfc_c4(void);
int test_hpack_decode_rfc_c6(void);
int test_hpack_index_zero_rejected(void);
int test_hpack_index_out_of_range(void);
int test_hpack_size_update_after_header(void);
int test_hpack_size_update_exceeds_pending_max(void);
int test_hpack_bomb_size_limit(void);
int test_hpack_bomb_count_limit(void);
int test_hpack_header_callback_by_pointer(void);

/* Phase 3.5 — full encoder */
int test_hpack_encode_decode_roundtrip_no_huff(void);
int test_hpack_encode_decode_roundtrip_huff(void);
int test_hpack_encode_pending_size_update_dual(void);

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

/* ------------------------------------------------------------------ */
/* Phase 3.1 — hpack_table_t dynamic table                            */
/* ------------------------------------------------------------------ */

/*
 * Minimal allocator shim for dynamic table tests.
 * Test code is not library source, so direct malloc/free is permitted.
 */
static void *
test_malloc_fn(size_t size, void *ctx)
{
	(void)ctx;
	return malloc(size);
}

static void
test_free_fn(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);
}

static void *
test_calloc_fn(size_t nmemb, size_t size, void *ctx)
{
	(void)ctx;
	return calloc(nmemb, size);
}

static const hive_mem_t test_mem = {
	test_malloc_fn,
	test_free_fn,
	test_calloc_fn,
	NULL, /* realloc not needed for table tests */
	NULL,
};

/*
 * test_hpack_table_insert_basic — insert one entry, verify via lookup.
 *
 * RFC 7541 §4.1: size = name_len + value_len + 32.
 */
int
test_hpack_table_insert_basic(void)
{
	hpack_table_t t;
	uint32_t      dyn_idx;
	int           ret, match;

	ret = hpack_table_init(&t, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.count == 0);
	ASSERT(t.size == 0);

	/* Insert "custom-key" / "custom-value" */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"custom-key", 10,
	    (const uint8_t *)"custom-value", 12);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.count == 1);
	ASSERT(t.size == 54); /* 10 + 12 + 32 */

	/* Lookup: should be an exact match at dyn_idx 0 (newest) */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"custom-key", 10,
	    (const uint8_t *)"custom-value", 12,
	    &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_EXACT);
	ASSERT(dyn_idx == 0);

	/* Name-only lookup: different value should give NAME_ONLY match */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"custom-key", 10,
	    (const uint8_t *)"other", 5,
	    &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_NAME_ONLY);
	ASSERT(dyn_idx == 0);

	/* Unknown name: should give NOT_FOUND */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"unknown", 7,
	    (const uint8_t *)"value", 5,
	    &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_NOT_FOUND);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/*
 * test_hpack_table_evict_on_insert — verify oldest entry is evicted.
 *
 * max_size=135; each entry: 2+2+32=36 bytes.
 * Three entries total 108 bytes.  Insert of 4th (36 bytes) would reach
 * 144 > 135 — oldest entry must be evicted first.
 */
int
test_hpack_table_evict_on_insert(void)
{
	hpack_table_t t;
	uint32_t      dyn_idx;
	int           ret, match;

	ret = hpack_table_init(&t, &test_mem, 135);
	ASSERT(ret == HIVE_OK);

	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"n1", 2, (const uint8_t *)"v1", 2);
	ASSERT(ret == HIVE_OK);  /* size = 36 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"n2", 2, (const uint8_t *)"v2", 2);
	ASSERT(ret == HIVE_OK);  /* size = 72 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"n3", 2, (const uint8_t *)"v3", 2);
	ASSERT(ret == HIVE_OK);  /* size = 108 */

	ASSERT(t.count == 3);
	ASSERT(t.size == 108);

	/* 4th insert: 108+36=144 > 135; evict n1/v1; size=72+36=108 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"n4", 2, (const uint8_t *)"v4", 2);
	ASSERT(ret == HIVE_OK);

	ASSERT(t.count == 3);
	ASSERT(t.size == 108);

	/* n1/v1 should be gone */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"n1", 2, (const uint8_t *)"v1", 2, &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_NOT_FOUND);

	/* n4/v4 should be newest (dyn_idx 0) */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"n4", 2, (const uint8_t *)"v4", 2, &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_EXACT);
	ASSERT(dyn_idx == 0);

	/* n2 is now oldest and at dyn_idx 2 */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"n2", 2, (const uint8_t *)"v2", 2, &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_EXACT);
	ASSERT(dyn_idx == 2);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/*
 * test_hpack_table_evict_to_zero — evict_to(0) removes all entries.
 */
int
test_hpack_table_evict_to_zero(void)
{
	hpack_table_t t;
	int           ret;

	ret = hpack_table_init(&t, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"a", 1, (const uint8_t *)"b", 1);
	ASSERT(ret == HIVE_OK);
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"c", 1, (const uint8_t *)"d", 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.count == 2);

	hpack_table_evict_to(&t, &test_mem, 0);

	ASSERT(t.count == 0);
	ASSERT(t.size == 0);

	/* Table is valid and usable after full eviction */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"x", 1, (const uint8_t *)"y", 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.count == 1);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/*
 * test_hpack_table_rfc_size — verify name + value + 32 accounting.
 *
 * RFC 7541 §4.1: each entry costs name_len + value_len + 32 bytes.
 */
int
test_hpack_table_rfc_size(void)
{
	hpack_table_t t;
	int           ret;

	ret = hpack_table_init(&t, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	/* "custom-key" (10) + "" (0) + 32 = 42 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"custom-key", 10,
	    (const uint8_t *)"", 0);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.size == 42);

	/* "" (0) + "custom-val" (10) + 32 = 42; total = 84 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"", 0,
	    (const uint8_t *)"custom-val", 10);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.size == 84);

	/* "ab" (2) + "cd" (2) + 32 = 36; total = 120 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"ab", 2, (const uint8_t *)"cd", 2);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.size == 120);
	ASSERT(t.count == 3);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/*
 * test_hpack_table_oversized_entry — rfc_size > max_size.
 *
 * RFC 7541 §4.4: when the new entry's rfc_size exceeds max_size, the
 * entire existing table is evicted and the entry is NOT inserted.
 * The table must remain valid and empty for future use.
 */
int
test_hpack_table_oversized_entry(void)
{
	hpack_table_t t;
	uint32_t      dyn_idx;
	int           ret, match;

	/* max_size=64; "hello"/"world" rfc_size=5+5+32=42 — fits */
	ret = hpack_table_init(&t, &test_mem, 64);
	ASSERT(ret == HIVE_OK);

	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"hello", 5, (const uint8_t *)"world", 5);
	ASSERT(ret == HIVE_OK);
	ASSERT(t.count == 1);
	ASSERT(t.size == 42);

	/*
	 * Insert oversized entry: 20+20+32=72 > 64.
	 * Existing entry must be evicted; oversized entry not inserted.
	 */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"12345678901234567890", 20,
	    (const uint8_t *)"12345678901234567890", 20);
	ASSERT(ret == HIVE_OK);   /* not an error — evict and skip */
	ASSERT(t.count == 0);     /* table is empty */
	ASSERT(t.size == 0);

	/* Original entry is also gone */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"hello", 5, (const uint8_t *)"world", 5,
	    &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_NOT_FOUND);

	/* Table is still valid: small entries can be inserted again */
	ret = hpack_table_insert(&t, &test_mem,
	    (const uint8_t *)"x", 1, (const uint8_t *)"y", 1);
	ASSERT(ret == HIVE_OK); /* 1+1+32=34 <= 64 */
	ASSERT(t.count == 1);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Phase 3.2 — hpack_decode_int / hpack_encode_int                    */
/* ------------------------------------------------------------------ */

/*
 * test_hpack_int_decode_1byte — value fits entirely in the prefix bits.
 *
 * RFC 7541 §C.1.1: integer value 10 with a 5-bit prefix.
 * Input byte: 0x0a (= 10 decimal).  Since 10 < 31 (prefix_max for N=5),
 * the value is encoded in the single prefix byte.
 * consumed must be 1.
 */
int
test_hpack_int_decode_1byte(void)
{
	static const uint8_t src[] = { 0x0a };
	size_t   consumed;
	uint32_t val;

	val = hpack_decode_int(src, sizeof(src), 5, &consumed);
	ASSERT(val == 10);
	ASSERT(consumed == 1);
	return 1;
}

/*
 * test_hpack_int_decode_multibyte — multi-byte continuation decoding.
 *
 * RFC 7541 §C.1.3: integer value 1337 with a 5-bit prefix.
 * Encoded as: [0x1f, 0x9a, 0x0a]
 *   src[0] = 0x1f: prefix value = 31 = prefix_max → continuation
 *   src[1] = 0x9a: data = 0x1a = 26, continuation bit set
 *   src[2] = 0x0a: data = 0x0a = 10, continuation bit clear → done
 *   val = 31 + (26 << 0) + (10 << 7) = 31 + 26 + 1280 = 1337
 * consumed must be 3.
 */
int
test_hpack_int_decode_multibyte(void)
{
	static const uint8_t src[] = { 0x1f, 0x9a, 0x0a };
	size_t   consumed;
	uint32_t val;

	val = hpack_decode_int(src, sizeof(src), 5, &consumed);
	ASSERT(val == 1337);
	ASSERT(consumed == 3);
	return 1;
}

/*
 * test_hpack_int_decode_truncated — input ends mid-continuation.
 *
 * src[0] = 0x1f (prefix_bits=5, value=31, multi-byte required).
 * src[1] = 0x9a (continuation bit set, but no further byte follows).
 * The loop exhausts the input without a terminating byte → OVERFLOW.
 */
int
test_hpack_int_decode_truncated(void)
{
	static const uint8_t src[] = { 0x1f, 0x9a };
	size_t   consumed;
	uint32_t val;

	val = hpack_decode_int(src, sizeof(src), 5, &consumed);
	ASSERT(val == HPACK_INT_OVERFLOW);
	return 1;
}

/*
 * test_hpack_int_decode_overflow — continuation bytes push value above
 * UINT32_MAX.
 *
 * Input (prefix_bits=5):
 *   [0x1f, 0xff, 0xff, 0xff, 0xff, 0xff]
 *   src[0] = 0x1f: val=31, multi-byte
 *   Bytes 1-4 (0xff each, data=0x7f, continuation set):
 *     iter1: val = 31 + 127 = 158,            m=7
 *     iter2: val = 158 + 16256 = 16414,       m=14
 *     iter3: val = 16414 + 2080768 = 2097182, m=21
 *     iter4: val = 2097182 + 266338304 = 268435486, m=28
 *   Byte 5 (0xff, data=0x7f, m=28 at entry):
 *     tmp = 268435486 + (0x7f << 28) = 34359738398 > UINT32_MAX
 *     → HPACK_INT_OVERFLOW (64-bit overflow check fires)
 */
int
test_hpack_int_decode_overflow(void)
{
	static const uint8_t src[] = { 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff };
	size_t   consumed;
	uint32_t val;

	val = hpack_decode_int(src, sizeof(src), 5, &consumed);
	ASSERT(val == HPACK_INT_OVERFLOW);
	return 1;
}

/*
 * test_hpack_int_encode_decode_roundtrip — encode then decode, verify.
 *
 * Tests values covering single-byte (val < prefix_max) and multi-byte
 * (val >= prefix_max) paths.  Confirms the consumed byte count equals
 * the encoded byte count for every test vector.
 */
int
test_hpack_int_encode_decode_roundtrip(void)
{
	/*
	 * {val, prefix_bits, expected_encoded_len}
	 * 0 with 5-bit:  single byte (0 < 31)
	 * 30 with 5-bit: single byte (30 < 31)
	 * 1337 with 5-bit: 3 bytes (RFC 7541 §C.1.3)
	 * 0 with 1-bit:  single byte (0 < 1)
	 * 1 with 1-bit:  multi-byte (1 >= 1)
	 * 254 with 8-bit: single byte (254 < 255)
	 * 256 with 8-bit: 2 bytes (256 - 255 = 1 < 128): 0xFF, 0x01
	 */
	static const struct {
		uint32_t val;
		int      prefix_bits;
		size_t   expected_len;
	} cases[] = {
		{0,    5, 1},
		{30,   5, 1},
		{1337, 5, 3},
		{0,    1, 1},
		{1,    1, 2},
		{254,  8, 1},
		{256,  8, 2},
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t  buf[16];
		size_t   enc_len;
		size_t   consumed;
		uint32_t decoded;

		enc_len = hpack_encode_int(buf, sizeof(buf), 0x00,
		    cases[i].prefix_bits, cases[i].val);
		ASSERT(enc_len == cases[i].expected_len);

		decoded = hpack_decode_int(buf, enc_len, cases[i].prefix_bits,
		    &consumed);
		ASSERT(decoded != HPACK_INT_OVERFLOW);
		ASSERT(decoded == cases[i].val);
		ASSERT(consumed == enc_len);
	}
	return 1;
}

/*
 * test_hpack_always_copy — overwrite source buffers after insert.
 *
 * SECURITY: the table must hold its own copies of name and value bytes.
 * Overwriting the caller's source buffers must not corrupt table data.
 * See ARCHITECTURE.md §8.1 and CODING_STANDARDS.md §3.2.
 */
int
test_hpack_always_copy(void)
{
	hpack_table_t        t;
	uint8_t              name_buf[10];
	uint8_t              value_buf[12];
	int                  ret, match;
	uint32_t             dyn_idx;
	const hpack_entry_t *entry;

	ret = hpack_table_init(&t, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	memcpy(name_buf, "custom-key", 10);
	memcpy(value_buf, "custom-value", 12);

	ret = hpack_table_insert(&t, &test_mem, name_buf, 10, value_buf, 12);
	ASSERT(ret == HIVE_OK);

	/* Overwrite the original source buffers */
	memset(name_buf, 0xFF, sizeof(name_buf));
	memset(value_buf, 0xFF, sizeof(value_buf));

	/*
	 * Verify the allocated entry still holds the original bytes.
	 * ring_head advanced past the insertion point; newest is at
	 * (ring_head - 1) & (ring_cap - 1).
	 */
	entry = t.ring[(t.ring_head - 1u) & (t.ring_cap - 1u)];
	ASSERT(entry != NULL);
	ASSERT(entry->name_len == 10);
	ASSERT(entry->value_len == 12);
	ASSERT(memcmp(HPACK_ENTRY_NAME(entry), "custom-key", 10) == 0);
	ASSERT(memcmp(HPACK_ENTRY_VALUE(entry), "custom-value", 12) == 0);

	/* Lookup must also succeed using the original bytes */
	match = hpack_table_lookup(&t,
	    (const uint8_t *)"custom-key", 10,
	    (const uint8_t *)"custom-value", 12,
	    &dyn_idx);
	ASSERT(match == HPACK_LOOKUP_EXACT);
	ASSERT(dyn_idx == 0);

	hpack_table_free(&t, &test_mem);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Phase 3.3 — hpack_decode_string / hpack_encode_string              */
/* ------------------------------------------------------------------ */

/*
 * test_hpack_string_decode_literal — decode a non-Huffman HPACK string.
 *
 * Encodes "www.example.com" (15 bytes) as a literal string: the first
 * byte is 0x0f (Huffman flag = 0, length = 15), followed by the raw
 * ASCII bytes.  The decoded output must match and consumed must be 16.
 *
 * For non-Huffman strings, out.data must point directly into src (no
 * copy — the zero-copy non-Huffman property per ARCHITECTURE.md §4.6).
 */
int
test_hpack_string_decode_literal(void)
{
	static const uint8_t src[] = {
		0x0f,
		'w', 'w', 'w', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
		'.', 'c', 'o', 'm',
	};
	uint8_t    scratch[256];
	hive_buf_t out;
	size_t     consumed;
	int        ret;

	ret = hpack_decode_string(src, sizeof(src),
	    scratch, sizeof(scratch), &out, &consumed);
	ASSERT(ret == HIVE_OK);
	ASSERT(consumed == sizeof(src));
	ASSERT(out.len == 15);
	ASSERT(memcmp(out.data, "www.example.com", 15) == 0);
	ASSERT(out.flags & HIVE_BUF_VALID);
	/* Non-Huffman: data is a direct pointer into src, not a copy */
	ASSERT(out.data == src + 1);
	return 1;
}

/*
 * test_hpack_string_decode_huffman — decode a Huffman-encoded HPACK string.
 *
 * Uses the RFC 7541 §C.4 example: "www.example.com" Huffman-encoded as
 * 12 bytes.  The HPACK string header is 0x8c (bit 7 = Huffman, length = 12).
 *
 * Total consumed = 1 + 12 = 13.  Decoded length = 15.  out.data must
 * point into the scratch buffer (the Huffman decoder writes there).
 */
int
test_hpack_string_decode_huffman(void)
{
	/* 0x8c: Huffman flag set (bit 7 = 1), compressed length = 12 */
	static const uint8_t src[] = {
		0x8c,
		0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a,
		0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
	};
	uint8_t    scratch[256];
	hive_buf_t out;
	size_t     consumed;
	int        ret;

	ret = hpack_decode_string(src, sizeof(src),
	    scratch, sizeof(scratch), &out, &consumed);
	ASSERT(ret == HIVE_OK);
	ASSERT(consumed == sizeof(src)); /* 1 header + 12 encoded bytes */
	ASSERT(out.len == 15);
	ASSERT(memcmp(out.data, "www.example.com", 15) == 0);
	ASSERT(out.flags & HIVE_BUF_VALID);
	/* Huffman: data points into scratch buffer */
	ASSERT(out.data == scratch);
	return 1;
}

/*
 * test_hpack_string_encode_huffman — encode a string as a shorter Huffman form.
 *
 * "no-cache" (8 ASCII bytes) is known to produce a shorter Huffman encoding
 * (6 bytes, per the RFC 7541 §C.3 example).  The encoder must set bit 7 of
 * the first output byte (Huffman flag) and the total wire bytes must be
 * strictly fewer than 1 (header) + 8 (literal) = 9 bytes.
 *
 * Decode-back round-trip confirms correctness.
 */
int
test_hpack_string_encode_huffman(void)
{
	static const uint8_t src[] = "no-cache"; /* 8 bytes */
	uint8_t    out[64];
	uint8_t    dec_scratch[64];
	hive_buf_t decoded;
	size_t     enc_len;
	size_t     consumed;
	int        ret;

	enc_len = hpack_encode_string(src, 8, out, sizeof(out));
	ASSERT(enc_len > 0);

	/* Huffman flag must be set in the leading byte */
	ASSERT(out[0] & 0x80u);

	/*
	 * Huffman-encoded "no-cache" = 6 bytes, so total wire length is
	 * 1 (header) + 6 = 7 bytes, strictly less than 9 for literal.
	 */
	ASSERT(enc_len < 9u);

	/* Round-trip decode must recover the original string */
	ret = hpack_decode_string(out, enc_len,
	    dec_scratch, sizeof(dec_scratch), &decoded, &consumed);
	ASSERT(ret == HIVE_OK);
	ASSERT(consumed == enc_len);
	ASSERT(decoded.len == 8);
	ASSERT(memcmp(decoded.data, "no-cache", 8) == 0);
	return 1;
}

/*
 * test_hpack_string_scratch_limit — decoded string exceeds scratch_cap.
 *
 * A literal string of length 5 is presented with scratch_cap = 4.
 * The claimed decoded length (5) exceeds the limit (4), so
 * hpack_decode_string() must return HIVE_ERR_COMPRESSION.
 *
 * See ARCHITECTURE.md §4.6 — the limit applies to both Huffman and
 * literal strings.
 */
int
test_hpack_string_scratch_limit(void)
{
	static const uint8_t src[] = {
		0x05, 'h', 'e', 'l', 'l', 'o', /* length=5, literal "hello" */
	};
	uint8_t    scratch[4]; /* capacity = 4, string length = 5 */
	hive_buf_t out;
	size_t     consumed;
	int        ret;

	ret = hpack_decode_string(src, sizeof(src),
	    scratch, 4, &out, &consumed);
	ASSERT(ret == HIVE_ERR_COMPRESSION);
	return 1;
}

/*
 * test_hpack_string_truncated — claimed length exceeds remaining bytes.
 *
 * The length field claims 10 bytes but only 5 string bytes follow.
 * hpack_decode_string() must return HIVE_ERR_COMPRESSION (truncated
 * string per ARCHITECTURE.md §4.6).
 */
int
test_hpack_string_truncated(void)
{
	static const uint8_t src[] = {
		0x0a, 'h', 'e', 'l', 'l', 'o', /* length=10 but only 5 bytes */
	};
	uint8_t    scratch[256];
	hive_buf_t out;
	size_t     consumed;
	int        ret;

	ret = hpack_decode_string(src, sizeof(src),
	    scratch, sizeof(scratch), &out, &consumed);
	ASSERT(ret == HIVE_ERR_COMPRESSION);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Phase 3.4 — hpack_decode_block                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	int begin_count;
	int header_count;
	int complete_count;
	int no_index_count;
	char names[16][64];
	char values[16][128];
	const hive_buf_t *saved_name;
	const hive_buf_t *saved_value;
} hpack_cap_t;

static int
hpack_test_on_begin_headers(hive_session_t *s, uint32_t stream_id, void *ud)
{
	(void)s;
	(void)stream_id;
	((hpack_cap_t *)ud)->begin_count++;
	return HIVE_OK;
}

static int
hpack_test_on_header(hive_session_t *s,
                     uint32_t stream_id,
                     hive_buf_t *name,
                     hive_buf_t *value,
                     uint8_t flags,
                     void *ud)
{
	hpack_cap_t *cap;
	size_t n;
	size_t v;

	(void)s;
	(void)stream_id;
	cap = (hpack_cap_t *)ud;
	if (cap->header_count >= 16)
		return HIVE_ERR_PROTOCOL;

	n = name->len;
	if (n >= sizeof(cap->names[0]))
		n = sizeof(cap->names[0]) - 1;
	v = value->len;
	if (v >= sizeof(cap->values[0]))
		v = sizeof(cap->values[0]) - 1;

	memcpy(cap->names[cap->header_count], name->data, n);
	cap->names[cap->header_count][n] = '\0';
	if (v > 0)
		memcpy(cap->values[cap->header_count], value->data, v);
	cap->values[cap->header_count][v] = '\0';
	if ((flags & HIVE_NV_FLAG_NO_INDEX) != 0)
		cap->no_index_count++;

	cap->saved_name = name;
	cap->saved_value = value;
	cap->header_count++;
	return HIVE_OK;
}

static int
hpack_test_on_headers_complete(hive_session_t *s,
                               uint32_t stream_id,
                               uint8_t flags,
                               void *ud)
{
	(void)s;
	(void)stream_id;
	(void)flags;
	((hpack_cap_t *)ud)->complete_count++;
	return HIVE_OK;
}

static int
hpack_test_session_init(hive_session_t *s, hpack_cap_t *cap, uint32_t max_table)
{
	int ret;

	memset(s, 0, sizeof(*s));
	memset(cap, 0, sizeof(*cap));

	s->mem = test_mem;
	s->opt_max_header_string_size = 8192;
	s->opt_max_header_list_size = 65536;
	s->opt_max_header_count = 100;
	s->opt_max_continuation_size = 65536;
	s->opt_no_http_messaging = 1;
	s->reassembly_stream_id = 1;

	s->callbacks.on_begin_headers = hpack_test_on_begin_headers;
	s->callbacks.on_header = hpack_test_on_header;
	s->callbacks.on_headers_complete = hpack_test_on_headers_complete;
	s->user_data = cap;

	s->hpack_scratch_name = malloc(s->opt_max_header_string_size);
	s->hpack_scratch_value = malloc(s->opt_max_header_string_size);
	s->reassembly_buf = malloc(s->opt_max_continuation_size);
	if (s->hpack_scratch_name == NULL ||
	    s->hpack_scratch_value == NULL ||
	    s->reassembly_buf == NULL)
		return HIVE_ERR_NOMEM;

	ret = hpack_table_init(&s->dec_table, &s->mem, max_table);
	if (ret != HIVE_OK)
		return ret;
	return HIVE_OK;
}

static void
hpack_test_session_free(hive_session_t *s)
{
	hpack_table_free(&s->dec_table, &s->mem);
	free(s->reassembly_buf);
	free(s->hpack_scratch_name);
	free(s->hpack_scratch_value);
}

int
test_hpack_decode_rfc_c3(void)
{
	static const uint8_t block[] = {
		0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77,
		0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65,
		0x2e, 0x63, 0x6f, 0x6d,
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.begin_count == 1);
	ASSERT(cap.header_count == 4);
	ASSERT(cap.complete_count == 1);
	ASSERT(strcmp(cap.names[0], ":method") == 0);
	ASSERT(strcmp(cap.values[0], "GET") == 0);
	ASSERT(strcmp(cap.names[1], ":scheme") == 0);
	ASSERT(strcmp(cap.values[1], "http") == 0);
	ASSERT(strcmp(cap.names[2], ":path") == 0);
	ASSERT(strcmp(cap.values[2], "/") == 0);
	ASSERT(strcmp(cap.names[3], ":authority") == 0);
	ASSERT(strcmp(cap.values[3], "www.example.com") == 0);
	ASSERT(s.dec_table.count == 1);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_decode_rfc_c4(void)
{
	static const uint8_t block[] = {
		0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
		0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
		0xff,
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.header_count == 4);
	ASSERT(strcmp(cap.values[3], "www.example.com") == 0);
	ASSERT(s.dec_table.count == 1);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_decode_rfc_c6(void)
{
	static const uint8_t block[] = {
		0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2,
		0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4,
		0xff,
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.header_count == 4);
	ASSERT(strcmp(cap.names[0], ":method") == 0);
	ASSERT(strcmp(cap.values[0], "GET") == 0);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_index_zero_rejected(void)
{
	static const uint8_t block[] = { 0x80 };
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_COMPRESSION);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_index_out_of_range(void)
{
	static const uint8_t block[] = { 0xff, 0x00 };
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_COMPRESSION);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_size_update_after_header(void)
{
	static const uint8_t block[] = {
		0x82,       /* indexed :method GET */
		0x3f, 0x00, /* size update to 31 after a header */
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_COMPRESSION);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_size_update_exceeds_pending_max(void)
{
	static const uint8_t block[] = { 0x3f, 0x0a };
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);
	s.dec_table.pending_max = 32;

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_COMPRESSION);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_bomb_size_limit(void)
{
	static const uint8_t block[] = {
		0x40, 0x01, 'a', 0x03, 'b', 'b', 'b',
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);
	s.opt_max_header_list_size = 35;

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_PROTOCOL);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_bomb_count_limit(void)
{
	static const uint8_t block[] = {
		0x40, 0x01, 'a', 0x01, '1',
		0x40, 0x01, 'b', 0x01, '2',
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);
	s.opt_max_header_count = 1;

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_ERR_PROTOCOL);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_header_callback_by_pointer(void)
{
	static const uint8_t block[] = {
		0x82,
	};
	hive_session_t s;
	hpack_cap_t cap;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_decode_block(&s, block, sizeof(block), 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.saved_name != NULL);
	ASSERT(cap.saved_value != NULL);
	ASSERT((cap.saved_name->flags & HIVE_BUF_VALID) == 0);
	ASSERT((cap.saved_value->flags & HIVE_BUF_VALID) == 0);

	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_encode_decode_roundtrip_no_huff(void)
{
	static const hive_nv_t nva[] = {
		{(const uint8_t *)":method", (const uint8_t *)"GET", 7, 3, 0},
		{(const uint8_t *)"x", (const uint8_t *)"a", 1, 1, 0},
	};
	hive_session_t s;
	hpack_cap_t cap;
	hpack_table_t enc;
	uint8_t block[256];
	size_t block_len;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);
	ret = hpack_table_init(&enc, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_encode_block(&enc,
	                        &test_mem,
	                        nva,
	                        sizeof(nva) / sizeof(nva[0]),
	                        block,
	                        sizeof(block),
	                        &block_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(block_len > 0);

	ret = hpack_decode_block(&s, block, block_len, 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.header_count == 2);
	ASSERT(strcmp(cap.names[0], ":method") == 0);
	ASSERT(strcmp(cap.values[0], "GET") == 0);
	ASSERT(strcmp(cap.names[1], "x") == 0);
	ASSERT(strcmp(cap.values[1], "a") == 0);

	hpack_table_free(&enc, &test_mem);
	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_encode_decode_roundtrip_huff(void)
{
	static const hive_nv_t nva[] = {
		{(const uint8_t *)"x", (const uint8_t *)"www.example.com", 1, 15, 0},
	};
	hive_session_t s;
	hpack_cap_t cap;
	hpack_table_t enc;
	uint8_t block[256];
	size_t block_len;
	int ret;

	ret = hpack_test_session_init(&s, &cap, 4096);
	ASSERT(ret == HIVE_OK);
	ret = hpack_table_init(&enc, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_encode_block(
	    &enc, &test_mem, nva, 1, block, sizeof(block), &block_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(block_len > 4);
	ASSERT((block[3] & 0x80u) != 0); /* value string uses Huffman form */

	ret = hpack_decode_block(&s, block, block_len, 0, 1);
	ASSERT(ret == HIVE_OK);
	ASSERT(cap.header_count == 1);
	ASSERT(strcmp(cap.names[0], "x") == 0);
	ASSERT(strcmp(cap.values[0], "www.example.com") == 0);

	hpack_table_free(&enc, &test_mem);
	hpack_test_session_free(&s);
	return 1;
}

int
test_hpack_encode_pending_size_update_dual(void)
{
	hpack_table_t enc;
	uint8_t block[16];
	size_t block_len;
	int ret;

	ret = hpack_table_init(&enc, &test_mem, 4096);
	ASSERT(ret == HIVE_OK);

	ret = hpack_table_insert(&enc,
	                        &test_mem,
	                        (const uint8_t *)"name",
	                        4,
	                        (const uint8_t *)"value",
	                        5);
	ASSERT(ret == HIVE_OK);

	enc.pending_min = 64;
	enc.pending_max = 128;
	enc.has_pending = 1;

	ret = hpack_encode_block(&enc,
	                        &test_mem,
	                        NULL,
	                        0,
	                        block,
	                        sizeof(block),
	                        &block_len);
	ASSERT(ret == HIVE_OK);
	ASSERT(block_len == 4);
	ASSERT(block[0] == 0x3fu);
	ASSERT(block[1] == 0x21u);
	ASSERT(block[2] == 0x3fu);
	ASSERT(block[3] == 0x61u);
	ASSERT(enc.max_size == 128);
	ASSERT(enc.size <= 128);
	ASSERT(enc.has_pending == 0);
	ASSERT(enc.pending_min == 128);
	ASSERT(enc.pending_max == 128);

	hpack_table_free(&enc, &test_mem);
	return 1;
}

