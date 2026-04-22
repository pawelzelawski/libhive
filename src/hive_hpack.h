/*
 * hive_hpack.h — HPACK internal types and declarations
 *
 * Covers the static table (RFC 7541 Appendix A), Huffman decode table
 * (RFC 7541 Appendix B, 256-entry), and Huffman encode table (257 entries).
 * All tables are compile-time constants; zero runtime initialisation.
 *
 * See ARCHITECTURE.md §4.3 (static table), §4.4 (Huffman decode),
 * §4.8 (Huffman encode) for design rationale.
 * Not included by embedders — internal to the library only.
 */

#ifndef HIVE_HPACK_H
#define HIVE_HPACK_H

#include <stddef.h>
#include <stdint.h>

#include "../include/hive.h"

/*
 * Huffman decode table entry.
 * 4 bytes per entry — 256 entries = 1 KB total.
 * See ARCHITECTURE.md §4.4 and CODING_STANDARDS.md §1.3.
 */
typedef struct {
	uint8_t sym;           /* decoded symbol */
	uint8_t bits_consumed; /* input bits consumed by this lookup (1–8) */
	uint8_t complete;      /* 1 = a complete symbol was produced */
	uint8_t eos; /* 1 = EOS symbol encountered (error mid-string) */
} huff_entry_t;

_Static_assert(
    sizeof(huff_entry_t) == 4,
    "huff_entry_t size changed — Huffman table is 1KB at 4 bytes per entry");

/*
 * Huffman encode table entry.
 * symbol → (code, bit-length) mapping.
 */
typedef struct {
	uint32_t code;
	uint8_t bits;
	uint8_t _pad[3]; /* maintain alignment */
} huff_sym_t;

/* Number of entries in the HPACK static table (RFC 7541 Appendix A). */
#define HPACK_STATIC_TABLE_SIZE 61

/*
 * HPACK static table — 61 entries, indices 1–61.
 * Array index i holds static table index (i + 1).
 *
 * Convention for entries with no default value (e.g. ":authority",
 * "accept-charset", "www-authenticate"): `value == NULL` and
 * `value_len == 0`. Consumers must treat these as "name only — caller
 * supplies value at encode/lookup time"; do NOT dereference `value`
 * in that case. The static-table tests in tests/test_hpack.c assert
 * this convention for indices 1, 15, 17–61.
 */
extern const hive_nv_t hpack_static_table[HPACK_STATIC_TABLE_SIZE];

/*
 * huff_decode — Huffman decode src into scratch buffer.
 *
 * src may be NULL when src_len == 0 (zero-length decode succeeds with
 * *out_len == 0). When src_len > 0, src must be non-NULL. scratch and
 * out_len must always be non-NULL.
 *
 * Returns HIVE_OK on success, HIVE_ERR_COMPRESSION on any encoding error
 * (invalid padding, excess leftover bits, EOS mid-string, decoded string
 * exceeding max_len). On success, *out_len is set to the decoded byte count.
 *
 * See ARCHITECTURE.md §4.4 for the decode loop pseudocode (fast path +
 * huff_decode_long slow path for codes longer than 8 bits).
 */
int huff_decode(const uint8_t *src,
                size_t src_len,
                uint8_t *scratch,
                size_t max_len,
                size_t *out_len);

/*
 * huff_encode — Huffman encode src into out buffer.
 *
 * Returns HIVE_OK on success, HIVE_ERR_NOMEM if out_cap is insufficient
 * (output-buffer-too-small; the caller must size `out` for the worst
 * case — every input byte expanding to up to 30/8 ≈ 4 output bytes
 * worst case for the longest Huffman codes), HIVE_ERR_INVALID_ARG on
 * NULL out/out_len. On success, *out_len is set to the number of bytes
 * written.
 */
int huff_encode(const uint8_t *src,
                size_t src_len,
                uint8_t *out,
                size_t out_cap,
                size_t *out_len);

#endif /* HIVE_HPACK_H */
