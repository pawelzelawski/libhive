/*
 * test_hpack.c - HPACK static table, Huffman and dynamic table tests
 *
 * Phase 1.4: static table and Huffman tests.
 * Phase 3.1: dynamic table (hpack_table_t) tests.
 *
 * See DEVELOPMENT.md tasks 1.4 and 3.1.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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

/* Phase 3.1 — dynamic table */
int test_hpack_table_insert_basic(void);
int test_hpack_table_evict_on_insert(void);
int test_hpack_table_evict_to_zero(void);
int test_hpack_table_rfc_size(void);
int test_hpack_table_oversized_entry(void);
int test_hpack_always_copy(void);

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

