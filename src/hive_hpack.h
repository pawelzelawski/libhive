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

/* ------------------------------------------------------------------ */
/* Dynamic table constants                                             */
/* See ARCHITECTURE.md §4.1 and §4.9.                                 */
/* ------------------------------------------------------------------ */

/*
 * Hash index is only allocated when max_size > this threshold.
 * Below it, lookup is a linear scan (the common, default-settings case).
 * See ARCHITECTURE.md §4.1 and §4.9.
 */
#define HPACK_LINEAR_THRESHOLD 16384u

/*
 * Hash slot sentinel values.
 * EMPTY marks slots that have never been used (safe to stop probing).
 * TOMBSTONE marks slots whose entry was evicted (must continue probing).
 * See ARCHITECTURE.md §4.9.
 */
#define HPACK_HASH_EMPTY 0xFFFFFFFFu
#define HPACK_HASH_TOMBSTONE 0xFFFFFFFEu

/*
 * Return values for hpack_table_lookup().
 * EXACT:     name and value both match.
 * NAME_ONLY: only name matches (caller may use as name-index reference).
 * NOT_FOUND: no entry with this name.
 */
#define HPACK_LOOKUP_EXACT 1
#define HPACK_LOOKUP_NAME_ONLY 0
#define HPACK_LOOKUP_NOT_FOUND (-1)

/* ------------------------------------------------------------------ */
/* Dynamic table entry layout                                          */
/* See ARCHITECTURE.md §4.2.                                          */
/* ------------------------------------------------------------------ */

/*
 * hpack_entry_t — single contiguous allocation for one dynamic table entry.
 *
 * Layout:  [ hpack_entry_t (8 bytes) | name bytes | value bytes ]
 *
 * Accessing name/value bytes via the macros below is mandatory;
 * never compute the offset manually outside this header.
 */
typedef struct {
	uint32_t name_len;
	uint32_t value_len;
	/*
	 * name bytes immediately follow at (uint8_t *)(entry + 1)
	 * value bytes immediately follow name
	 */
} hpack_entry_t;

#define HPACK_ENTRY_NAME(e) ((uint8_t *)((e) + 1))
#define HPACK_ENTRY_VALUE(e) ((uint8_t *)((e) + 1) + (e)->name_len)
#define HPACK_ENTRY_RFC_SIZE(e) ((e)->name_len + (e)->value_len + 32u)

/* ------------------------------------------------------------------ */
/* Hash index slot (used only when max_size > HPACK_LINEAR_THRESHOLD) */
/* See ARCHITECTURE.md §4.9.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t name_hash;  /* FNV-1a 32-bit hash of name bytes */
	uint32_t value_hash; /* FNV-1a 32-bit hash of value bytes */
	uint32_t ring_idx;   /* ring array index, or EMPTY/TOMBSTONE sentinel */
} hpack_hash_slot_t;

/* ------------------------------------------------------------------ */
/* Dynamic table structure                                             */
/* Embedded in hive_session_t (enc_table / dec_table).               */
/* See ARCHITECTURE.md §4.1.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	hpack_entry_t **ring; /* pointer ring; ring_cap entries */
	uint32_t ring_cap;    /* next_pow2(max_size/32); min 4 */
	uint32_t ring_head;   /* insertion point (next free slot) */
	uint32_t count;       /* live entries */
	uint32_t size;        /* current RFC size (sum of name+value+32) */
	uint32_t max_size;    /* current ceiling */
	uint32_t pending_max; /* new ceiling awaiting application */
	uint32_t pending_min; /* lowest value reached since last encode */
	uint8_t has_pending;  /* 1 = encoder must emit size update prefix */
	uint8_t _pad[3];

	/* hash index — NULL when max_size <= HPACK_LINEAR_THRESHOLD */
	hpack_hash_slot_t *hash;
	uint32_t hash_mask;
} hpack_table_t;

/* ------------------------------------------------------------------ */
/* Integer varint constants                                            */
/* See ARCHITECTURE.md §4.7.                                          */
/* ------------------------------------------------------------------ */

/*
 * HPACK_INT_OVERFLOW — sentinel returned by hpack_decode_int() on
 * truncated input or integer overflow.  UINT32_MAX is not a valid
 * decoded value because all HPACK integer uses are bounded well below
 * that limit (table sizes, indices, string lengths).  Callers must
 * compare the return value against this constant before use.
 */
#define HPACK_INT_OVERFLOW UINT32_MAX

/* ------------------------------------------------------------------ */
/* Dynamic table API                                                   */
/* ------------------------------------------------------------------ */

/*
 * hpack_table_init — allocate and initialise a dynamic table.
 *
 * Allocates the ring pointer array via mem->calloc.  hash is left NULL
 * (hash index is task 3.6, only used above HPACK_LINEAR_THRESHOLD).
 * pending_max and pending_min are initialised to max_size.
 *
 * Returns HIVE_OK or HIVE_ERR_NOMEM.
 */
int
hpack_table_init(hpack_table_t *t, const hive_mem_t *mem, uint32_t max_size);

/*
 * hpack_table_free — free all live entries and the ring array.
 *
 * After this call *t is zeroed.  Callers must not use *t again without
 * a fresh call to hpack_table_init().
 */
void hpack_table_free(hpack_table_t *t, const hive_mem_t *mem);

/*
 * hpack_table_evict_to — evict oldest entries until size <= new_max.
 *
 * No-op if size is already within new_max.  Used both by insert (to
 * make room) and by the dynamic table size update path.
 */
void
hpack_table_evict_to(hpack_table_t *t, const hive_mem_t *mem, uint32_t new_max);

/*
 * hpack_table_insert — copy and insert a new entry into the table.
 *
 * Evicts oldest entries as needed.  If rfc_size > max_size, the entire
 * table is evicted and the entry is NOT inserted (RFC 7541 §4.4).
 *
 * SECURITY: always copies name/value bytes into the allocated entry.
 * See ARCHITECTURE.md §8.1.
 *
 * Returns HIVE_OK, HIVE_ERR_NOMEM, or HIVE_ERR_COMPRESSION (overflow).
 */
int hpack_table_insert(hpack_table_t *t,
                       const hive_mem_t *mem,
                       const uint8_t *name,
                       uint32_t name_len,
                       const uint8_t *value,
                       uint32_t value_len);

/*
 * hpack_table_lookup — linear scan for name (and optionally value).
 *
 * Scans from newest entry backward.  Returns HPACK_LOOKUP_EXACT,
 * HPACK_LOOKUP_NAME_ONLY, or HPACK_LOOKUP_NOT_FOUND.  On a match,
 * *out_dyn_idx is set to the 0-based index from newest (0 = newest).
 *
 * Exact matches end the scan immediately (newest is preferred).
 * Name-only: first (newest) name match is returned.
 */
int hpack_table_lookup(const hpack_table_t *t,
                       const uint8_t *name,
                       uint32_t name_len,
                       const uint8_t *value,
                       uint32_t value_len,
                       uint32_t *out_dyn_idx);

/*
 * hpack_table_get — retrieve entry at 0-based dynamic index from newest.
 *
 * 0 = newest, count-1 = oldest.  Caller must ensure dyn_idx < count.
 * Returns a pointer into the allocated entry (do not free directly).
 */
const hpack_entry_t *hpack_table_get(const hpack_table_t *t, uint32_t dyn_idx);

/* ------------------------------------------------------------------ */
/* Integer varint encode/decode — Task 3.2                            */
/* See ARCHITECTURE.md §4.7.                                          */
/* ------------------------------------------------------------------ */

/*
 * hpack_decode_int — decode an HPACK varint from src[0..len).
 *
 * prefix_bits: number of low-order bits in src[0] used for the value
 * (N in RFC 7541 §5.1; 1..8).  The upper (8 - prefix_bits) bits of
 * src[0] are the representation tag and are masked off before reading
 * the prefix value.
 *
 * On success: *consumed is set to the number of bytes read (>= 1) and
 * the decoded value is returned.  On error (truncated input or integer
 * overflow): returns HPACK_INT_OVERFLOW.  Callers must compare the
 * return value against HPACK_INT_OVERFLOW before using it.
 *
 * Overflow guard: at most 5 continuation bytes are processed.  A sixth
 * continuation byte (m > 28 after m += 7) returns HPACK_INT_OVERFLOW
 * even if the 64-bit intermediate has not yet exceeded UINT32_MAX.
 * See ARCHITECTURE.md §4.7.
 */
uint32_t hpack_decode_int(const uint8_t *src,
                          size_t len,
                          int prefix_bits,
                          size_t *consumed);

/*
 * hpack_encode_int — encode val with an N-bit prefix into out[0..out_cap).
 *
 * prefix_top: the upper (8 - prefix_bits) bits to OR into the first
 * byte (e.g., 0x80 for indexed, 0x40 for literal with indexing, 0x20
 * for dynamic table size update, 0x00 for literal without indexing).
 * prefix_bits: N (1..8).
 * val: the integer value to encode.
 *
 * Returns the number of bytes written, or 0 if out_cap is insufficient.
 * A return of 0 indicates a buffer-too-small condition; callers must
 * size the output buffer appropriately (worst case: 1 + ceil(32/7) = 6
 * bytes for any 32-bit value with a 1-bit prefix).
 */
size_t hpack_encode_int(uint8_t *out,
                        size_t out_cap,
                        uint8_t prefix_top,
                        int prefix_bits,
                        uint32_t val);

/* ------------------------------------------------------------------ */
/* String encode/decode — Task 3.3                                     */
/* See ARCHITECTURE.md §4.6.                                           */
/* ------------------------------------------------------------------ */

/*
 * hpack_decode_string — decode one HPACK string field.
 *
 * src:        points to the first byte of the encoded string (the
 *             byte that carries the Huffman flag and the 7-bit length
 *             prefix per RFC 7541 §5.2).
 * src_len:    remaining bytes in the header block from src onward.
 * scratch:    target buffer for Huffman-decoded output.
 * scratch_cap: maximum allowed decoded length (opt_max_header_string_size).
 *             For non-Huffman strings the claimed length is also checked
 *             against this limit so callers see consistent sizing behaviour.
 * out:        filled on success; out->data and out->len are set.
 *             For Huffman strings, out->data points into scratch.
 *             For literal strings, out->data points directly into src
 *             (no copy — pointer into the source buffer).
 *             out->flags is set to HIVE_BUF_VALID on success.
 * consumed:   set to the total bytes read from src (header + string).
 *
 * Returns HIVE_OK on success, HIVE_ERR_COMPRESSION on any encoding
 * or size error (truncated input, decoded length exceeds scratch_cap,
 * Huffman decoding error). See ARCHITECTURE.md §4.6.
 */
int hpack_decode_string(const uint8_t *src,
                        size_t src_len,
                        uint8_t *scratch,
                        size_t scratch_cap,
                        hive_buf_t *out,
                        size_t *consumed);

/*
 * hpack_encode_string — encode one string into an HPACK wire block.
 *
 * Huffman-encodes the string when the result is strictly shorter than
 * the literal form; otherwise emits a literal.  Writes the 7-bit length
 * varint (with the Huffman flag in bit 7) followed by the string bytes.
 *
 * src, src_len: input string (may be zero length).
 * out, out_cap: output buffer.
 *
 * Returns the number of bytes written (>= 1) on success, or 0 if
 * out_cap is insufficient (caller must size the buffer appropriately —
 * worst case: 6 header bytes for a 32-bit length varint + string bytes).
 *
 * See ARCHITECTURE.md §4.8.
 */
size_t hpack_encode_string(const uint8_t *src,
                           size_t src_len,
                           uint8_t *out,
                           size_t out_cap);

/* ------------------------------------------------------------------ */
/* Full HPACK block decode — Task 3.4                                 */
/* See ARCHITECTURE.md §4.5 and §8.2.                                 */
/* ------------------------------------------------------------------ */

/*
 * hpack_decode_block — decode one complete HPACK header block.
 *
 * suppress_callbacks:
 *   0 = fire on_begin_headers/on_header/on_headers_complete callbacks.
 *   1 = decode fully for table synchronization but suppress callbacks.
 *
 * error_stream_id identifies the stream associated with this block for
 * stream-error bookkeeping by higher layers.
 *
 * Returns HIVE_OK on success, HIVE_ERR_COMPRESSION on HPACK decoding
 * failures, and HIVE_ERR_PROTOCOL on decoded-header policy failures
 * (bomb limits / messaging checks).
 */
int hpack_decode_block(hive_session_t *s,
                       const uint8_t *data,
                       size_t len,
                       int suppress_callbacks,
                       uint32_t error_stream_id);

/* ------------------------------------------------------------------ */
/* Full HPACK block encode — Task 3.5                                 */
/* See ARCHITECTURE.md §4.8.                                           */
/* ------------------------------------------------------------------ */

/*
 * hpack_encode_block — encode one complete header block.
 *
 * table points to the encoder dynamic table (session enc_table or
 * standalone encoder table). mem is used for dynamic table insertions.
 *
 * Returns HIVE_OK on success, HIVE_ERR_NOMEM/HIVE_ERR_INVALID_ARG on
 * argument/allocation failures, and sets *out_len to bytes written.
 */
int hpack_encode_block(hpack_table_t *table,
                       const hive_mem_t *mem,
                       const hive_nv_t *nva,
                       size_t nvlen,
                       uint8_t *out,
                       size_t out_cap,
                       size_t *out_len);

/* ------------------------------------------------------------------ */
/* Huffman decode table entry.                                         */
/* 4 bytes per entry — 256 entries = 1 KB total.                      */
/* See ARCHITECTURE.md §4.4 and CODING_STANDARDS.md §1.3.             */
/* ------------------------------------------------------------------ */

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
