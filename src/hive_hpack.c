/*
 * hive_hpack.c — HPACK static table and Huffman tables
 *
 * Implements compile-time tables for Phase 1.4:
 *   - HPACK static table (RFC 7541 Appendix A, 61 entries)
 *   - Huffman decode table (256-entry, RFC 7541 Appendix B)
 *   - Huffman encode table (257 entries including EOS)
 *   - huff_decode() — iterative bit-accumulator Huffman decoder
 *   - huff_encode() — Huffman encoder
 *
 * All tables are compile-time constants. No runtime initialisation.
 * No direct malloc/free — this file uses no allocator at all.
 *
 * See ARCHITECTURE.md §4.3 (static table), §4.4 (Huffman decode),
 * §4.8 (Huffman encode), and CODING_STANDARDS.md §2.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/hive.h"
#include "hive_internal.h"
#include "hive_hpack.h"

/* ------------------------------------------------------------------ */
/* HPACK static table — RFC 7541 Appendix A                            */
/* Array index i corresponds to static table index (i + 1).           */
/* Empty values are represented as NULL with value_len == 0.          */
/* ------------------------------------------------------------------ */

const hive_nv_t hpack_static_table[HPACK_STATIC_TABLE_SIZE] = {
    /* 1  */ {(const uint8_t *)":authority", NULL, 10, 0, 0},
    /* 2  */ {(const uint8_t *)":method", (const uint8_t *)"GET", 7, 3, 0},
    /* 3  */ {(const uint8_t *)":method", (const uint8_t *)"POST", 7, 4, 0},
    /* 4  */ {(const uint8_t *)":path", (const uint8_t *)"/", 5, 1, 0},
    /* 5  */
    {(const uint8_t *)":path", (const uint8_t *)"/index.html", 5, 11, 0},
    /* 6  */ {(const uint8_t *)":scheme", (const uint8_t *)"http", 7, 4, 0},
    /* 7  */ {(const uint8_t *)":scheme", (const uint8_t *)"https", 7, 5, 0},
    /* 8  */ {(const uint8_t *)":status", (const uint8_t *)"200", 7, 3, 0},
    /* 9  */ {(const uint8_t *)":status", (const uint8_t *)"204", 7, 3, 0},
    /* 10 */ {(const uint8_t *)":status", (const uint8_t *)"206", 7, 3, 0},
    /* 11 */ {(const uint8_t *)":status", (const uint8_t *)"304", 7, 3, 0},
    /* 12 */ {(const uint8_t *)":status", (const uint8_t *)"400", 7, 3, 0},
    /* 13 */ {(const uint8_t *)":status", (const uint8_t *)"404", 7, 3, 0},
    /* 14 */ {(const uint8_t *)":status", (const uint8_t *)"500", 7, 3, 0},
    /* 15 */ {(const uint8_t *)"accept-charset", NULL, 14, 0, 0},
    /* 16 */
    {(const uint8_t *)"accept-encoding",
     (const uint8_t *)"gzip, deflate",
     15,
     13,
     0},
    /* 17 */ {(const uint8_t *)"accept-language", NULL, 15, 0, 0},
    /* 18 */ {(const uint8_t *)"accept-ranges", NULL, 13, 0, 0},
    /* 19 */ {(const uint8_t *)"accept", NULL, 6, 0, 0},
    /* 20 */ {(const uint8_t *)"access-control-allow-origin", NULL, 27, 0, 0},
    /* 21 */ {(const uint8_t *)"age", NULL, 3, 0, 0},
    /* 22 */ {(const uint8_t *)"allow", NULL, 5, 0, 0},
    /* 23 */ {(const uint8_t *)"authorization", NULL, 13, 0, 0},
    /* 24 */ {(const uint8_t *)"cache-control", NULL, 13, 0, 0},
    /* 25 */ {(const uint8_t *)"content-disposition", NULL, 19, 0, 0},
    /* 26 */ {(const uint8_t *)"content-encoding", NULL, 16, 0, 0},
    /* 27 */ {(const uint8_t *)"content-language", NULL, 16, 0, 0},
    /* 28 */ {(const uint8_t *)"content-length", NULL, 14, 0, 0},
    /* 29 */ {(const uint8_t *)"content-location", NULL, 16, 0, 0},
    /* 30 */ {(const uint8_t *)"content-range", NULL, 13, 0, 0},
    /* 31 */ {(const uint8_t *)"content-type", NULL, 12, 0, 0},
    /* 32 */ {(const uint8_t *)"cookie", NULL, 6, 0, 0},
    /* 33 */ {(const uint8_t *)"date", NULL, 4, 0, 0},
    /* 34 */ {(const uint8_t *)"etag", NULL, 4, 0, 0},
    /* 35 */ {(const uint8_t *)"expect", NULL, 6, 0, 0},
    /* 36 */ {(const uint8_t *)"expires", NULL, 7, 0, 0},
    /* 37 */ {(const uint8_t *)"from", NULL, 4, 0, 0},
    /* 38 */ {(const uint8_t *)"host", NULL, 4, 0, 0},
    /* 39 */ {(const uint8_t *)"if-match", NULL, 8, 0, 0},
    /* 40 */ {(const uint8_t *)"if-modified-since", NULL, 17, 0, 0},
    /* 41 */ {(const uint8_t *)"if-none-match", NULL, 13, 0, 0},
    /* 42 */ {(const uint8_t *)"if-range", NULL, 8, 0, 0},
    /* 43 */ {(const uint8_t *)"if-unmodified-since", NULL, 19, 0, 0},
    /* 44 */ {(const uint8_t *)"last-modified", NULL, 13, 0, 0},
    /* 45 */ {(const uint8_t *)"link", NULL, 4, 0, 0},
    /* 46 */ {(const uint8_t *)"location", NULL, 8, 0, 0},
    /* 47 */ {(const uint8_t *)"max-forwards", NULL, 12, 0, 0},
    /* 48 */ {(const uint8_t *)"proxy-authenticate", NULL, 18, 0, 0},
    /* 49 */ {(const uint8_t *)"proxy-authorization", NULL, 19, 0, 0},
    /* 50 */ {(const uint8_t *)"range", NULL, 5, 0, 0},
    /* 51 */ {(const uint8_t *)"referer", NULL, 7, 0, 0},
    /* 52 */ {(const uint8_t *)"refresh", NULL, 7, 0, 0},
    /* 53 */ {(const uint8_t *)"retry-after", NULL, 11, 0, 0},
    /* 54 */ {(const uint8_t *)"server", NULL, 6, 0, 0},
    /* 55 */ {(const uint8_t *)"set-cookie", NULL, 10, 0, 0},
    /* 56 */ {(const uint8_t *)"strict-transport-security", NULL, 25, 0, 0},
    /* 57 */ {(const uint8_t *)"transfer-encoding", NULL, 17, 0, 0},
    /* 58 */ {(const uint8_t *)"user-agent", NULL, 10, 0, 0},
    /* 59 */ {(const uint8_t *)"vary", NULL, 4, 0, 0},
    /* 60 */ {(const uint8_t *)"via", NULL, 3, 0, 0},
    /* 61 */ {(const uint8_t *)"www-authenticate", NULL, 16, 0, 0},
};

/* ------------------------------------------------------------------ */
/* Huffman decode table — RFC 7541 Appendix B                          */
/* 256 entries, 8-bit indexed, 4 bytes per entry = 1 KB total.        */
/* Entry[i] gives the longest complete code starting with bit pattern  */
/* i (8 bits). Entries 0xfe and 0xff have complete=0 (need more       */
/* input). See ARCHITECTURE.md §4.4.                                  */
/* ------------------------------------------------------------------ */

static const huff_entry_t huff_decode_table[256] = {
    /* 0x00 */ {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    {48, 5, 1, 0},
    /* 0x08 */ {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    {49, 5, 1, 0},
    /* 0x10 */ {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    {50, 5, 1, 0},
    /* 0x18 */ {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    {97, 5, 1, 0},
    /* 0x20 */ {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    {99, 5, 1, 0},
    /* 0x28 */ {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    {101, 5, 1, 0},
    /* 0x30 */ {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    {105, 5, 1, 0},
    /* 0x38 */ {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    {111, 5, 1, 0},
    /* 0x40 */ {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    {115, 5, 1, 0},
    /* 0x48 */ {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    {116, 5, 1, 0},
    /* 0x50 */ {32, 6, 1, 0},
    {32, 6, 1, 0},
    {32, 6, 1, 0},
    {32, 6, 1, 0},
    {37, 6, 1, 0},
    {37, 6, 1, 0},
    {37, 6, 1, 0},
    {37, 6, 1, 0},
    /* 0x58 */ {45, 6, 1, 0},
    {45, 6, 1, 0},
    {45, 6, 1, 0},
    {45, 6, 1, 0},
    {46, 6, 1, 0},
    {46, 6, 1, 0},
    {46, 6, 1, 0},
    {46, 6, 1, 0},
    /* 0x60 */ {47, 6, 1, 0},
    {47, 6, 1, 0},
    {47, 6, 1, 0},
    {47, 6, 1, 0},
    {51, 6, 1, 0},
    {51, 6, 1, 0},
    {51, 6, 1, 0},
    {51, 6, 1, 0},
    /* 0x68 */ {52, 6, 1, 0},
    {52, 6, 1, 0},
    {52, 6, 1, 0},
    {52, 6, 1, 0},
    {53, 6, 1, 0},
    {53, 6, 1, 0},
    {53, 6, 1, 0},
    {53, 6, 1, 0},
    /* 0x70 */ {54, 6, 1, 0},
    {54, 6, 1, 0},
    {54, 6, 1, 0},
    {54, 6, 1, 0},
    {55, 6, 1, 0},
    {55, 6, 1, 0},
    {55, 6, 1, 0},
    {55, 6, 1, 0},
    /* 0x78 */ {56, 6, 1, 0},
    {56, 6, 1, 0},
    {56, 6, 1, 0},
    {56, 6, 1, 0},
    {57, 6, 1, 0},
    {57, 6, 1, 0},
    {57, 6, 1, 0},
    {57, 6, 1, 0},
    /* 0x80 */ {61, 6, 1, 0},
    {61, 6, 1, 0},
    {61, 6, 1, 0},
    {61, 6, 1, 0},
    {65, 6, 1, 0},
    {65, 6, 1, 0},
    {65, 6, 1, 0},
    {65, 6, 1, 0},
    /* 0x88 */ {95, 6, 1, 0},
    {95, 6, 1, 0},
    {95, 6, 1, 0},
    {95, 6, 1, 0},
    {98, 6, 1, 0},
    {98, 6, 1, 0},
    {98, 6, 1, 0},
    {98, 6, 1, 0},
    /* 0x90 */ {100, 6, 1, 0},
    {100, 6, 1, 0},
    {100, 6, 1, 0},
    {100, 6, 1, 0},
    {102, 6, 1, 0},
    {102, 6, 1, 0},
    {102, 6, 1, 0},
    {102, 6, 1, 0},
    /* 0x98 */ {103, 6, 1, 0},
    {103, 6, 1, 0},
    {103, 6, 1, 0},
    {103, 6, 1, 0},
    {104, 6, 1, 0},
    {104, 6, 1, 0},
    {104, 6, 1, 0},
    {104, 6, 1, 0},
    /* 0xa0 */ {108, 6, 1, 0},
    {108, 6, 1, 0},
    {108, 6, 1, 0},
    {108, 6, 1, 0},
    {109, 6, 1, 0},
    {109, 6, 1, 0},
    {109, 6, 1, 0},
    {109, 6, 1, 0},
    /* 0xa8 */ {110, 6, 1, 0},
    {110, 6, 1, 0},
    {110, 6, 1, 0},
    {110, 6, 1, 0},
    {112, 6, 1, 0},
    {112, 6, 1, 0},
    {112, 6, 1, 0},
    {112, 6, 1, 0},
    /* 0xb0 */ {114, 6, 1, 0},
    {114, 6, 1, 0},
    {114, 6, 1, 0},
    {114, 6, 1, 0},
    {117, 6, 1, 0},
    {117, 6, 1, 0},
    {117, 6, 1, 0},
    {117, 6, 1, 0},
    /* 0xb8 */ {58, 7, 1, 0},
    {58, 7, 1, 0},
    {66, 7, 1, 0},
    {66, 7, 1, 0},
    {67, 7, 1, 0},
    {67, 7, 1, 0},
    {68, 7, 1, 0},
    {68, 7, 1, 0},
    /* 0xc0 */ {69, 7, 1, 0},
    {69, 7, 1, 0},
    {70, 7, 1, 0},
    {70, 7, 1, 0},
    {71, 7, 1, 0},
    {71, 7, 1, 0},
    {72, 7, 1, 0},
    {72, 7, 1, 0},
    /* 0xc8 */ {73, 7, 1, 0},
    {73, 7, 1, 0},
    {74, 7, 1, 0},
    {74, 7, 1, 0},
    {75, 7, 1, 0},
    {75, 7, 1, 0},
    {76, 7, 1, 0},
    {76, 7, 1, 0},
    /* 0xd0 */ {77, 7, 1, 0},
    {77, 7, 1, 0},
    {78, 7, 1, 0},
    {78, 7, 1, 0},
    {79, 7, 1, 0},
    {79, 7, 1, 0},
    {80, 7, 1, 0},
    {80, 7, 1, 0},
    /* 0xd8 */ {81, 7, 1, 0},
    {81, 7, 1, 0},
    {82, 7, 1, 0},
    {82, 7, 1, 0},
    {83, 7, 1, 0},
    {83, 7, 1, 0},
    {84, 7, 1, 0},
    {84, 7, 1, 0},
    /* 0xe0 */ {85, 7, 1, 0},
    {85, 7, 1, 0},
    {86, 7, 1, 0},
    {86, 7, 1, 0},
    {87, 7, 1, 0},
    {87, 7, 1, 0},
    {89, 7, 1, 0},
    {89, 7, 1, 0},
    /* 0xe8 */ {106, 7, 1, 0},
    {106, 7, 1, 0},
    {107, 7, 1, 0},
    {107, 7, 1, 0},
    {113, 7, 1, 0},
    {113, 7, 1, 0},
    {118, 7, 1, 0},
    {118, 7, 1, 0},
    /* 0xf0 */ {119, 7, 1, 0},
    {119, 7, 1, 0},
    {120, 7, 1, 0},
    {120, 7, 1, 0},
    {121, 7, 1, 0},
    {121, 7, 1, 0},
    {122, 7, 1, 0},
    {122, 7, 1, 0},
    /* 0xf8 */ {38, 8, 1, 0},
    {42, 8, 1, 0},
    {44, 8, 1, 0},
    {59, 8, 1, 0},
    {88, 8, 1, 0},
    {90, 8, 1, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
};

/* ------------------------------------------------------------------ */
/* Huffman encode table — RFC 7541 Appendix B                          */
/* 257 entries: symbols 0–255 plus EOS (symbol 256).                  */
/* Each entry: { code, bits, _pad }.                                   */
/* ------------------------------------------------------------------ */

static const huff_sym_t huff_encode_table[257] = {
    /* 0   */ {0x1ff8u, 13u, {0, 0, 0}},
    /* 1   */ {0x7fffd8u, 23u, {0, 0, 0}},
    /* 2   */ {0xfffffe2u, 28u, {0, 0, 0}},
    /* 3   */ {0xfffffe3u, 28u, {0, 0, 0}},
    /* 4   */ {0xfffffe4u, 28u, {0, 0, 0}},
    /* 5   */ {0xfffffe5u, 28u, {0, 0, 0}},
    /* 6   */ {0xfffffe6u, 28u, {0, 0, 0}},
    /* 7   */ {0xfffffe7u, 28u, {0, 0, 0}},
    /* 8   */ {0xfffffe8u, 28u, {0, 0, 0}},
    /* 9   */ {0xffffeau, 24u, {0, 0, 0}},
    /* 10  */ {0x3ffffffcu, 30u, {0, 0, 0}},
    /* 11  */ {0xfffffe9u, 28u, {0, 0, 0}},
    /* 12  */ {0xfffffeau, 28u, {0, 0, 0}},
    /* 13  */ {0x3ffffffdu, 30u, {0, 0, 0}},
    /* 14  */ {0xfffffebu, 28u, {0, 0, 0}},
    /* 15  */ {0xfffffecu, 28u, {0, 0, 0}},
    /* 16  */ {0xfffffedu, 28u, {0, 0, 0}},
    /* 17  */ {0xfffffeeu, 28u, {0, 0, 0}},
    /* 18  */ {0xfffffefu, 28u, {0, 0, 0}},
    /* 19  */ {0xffffff0u, 28u, {0, 0, 0}},
    /* 20  */ {0xffffff1u, 28u, {0, 0, 0}},
    /* 21  */ {0xffffff2u, 28u, {0, 0, 0}},
    /* 22  */ {0x3ffffefeu, 30u, {0, 0, 0}},
    /* 23  */ {0xffffff3u, 28u, {0, 0, 0}},
    /* 24  */ {0xffffff4u, 28u, {0, 0, 0}},
    /* 25  */ {0xffffff5u, 28u, {0, 0, 0}},
    /* 26  */ {0xffffff6u, 28u, {0, 0, 0}},
    /* 27  */ {0xffffff7u, 28u, {0, 0, 0}},
    /* 28  */ {0xffffff8u, 28u, {0, 0, 0}},
    /* 29  */ {0xffffff9u, 28u, {0, 0, 0}},
    /* 30  */ {0xffffffau, 28u, {0, 0, 0}},
    /* 31  */ {0xffffffbu, 28u, {0, 0, 0}},
    /* 32  */ {0x14u, 6u, {0, 0, 0}},
    /* 33  */ {0x3f8u, 10u, {0, 0, 0}},
    /* 34  */ {0x3f9u, 10u, {0, 0, 0}},
    /* 35  */ {0xffau, 12u, {0, 0, 0}},
    /* 36  */ {0x1ff9u, 13u, {0, 0, 0}},
    /* 37  */ {0x15u, 6u, {0, 0, 0}},
    /* 38  */ {0xf8u, 8u, {0, 0, 0}},
    /* 39  */ {0x7fau, 11u, {0, 0, 0}},
    /* 40  */ {0x3fau, 10u, {0, 0, 0}},
    /* 41  */ {0x3fbu, 10u, {0, 0, 0}},
    /* 42  */ {0xf9u, 8u, {0, 0, 0}},
    /* 43  */ {0x7fbu, 11u, {0, 0, 0}},
    /* 44  */ {0xfau, 8u, {0, 0, 0}},
    /* 45  */ {0x16u, 6u, {0, 0, 0}},
    /* 46  */ {0x17u, 6u, {0, 0, 0}},
    /* 47  */ {0x18u, 6u, {0, 0, 0}},
    /* 48  */ {0x0u, 5u, {0, 0, 0}},
    /* 49  */ {0x1u, 5u, {0, 0, 0}},
    /* 50  */ {0x2u, 5u, {0, 0, 0}},
    /* 51  */ {0x19u, 6u, {0, 0, 0}},
    /* 52  */ {0x1au, 6u, {0, 0, 0}},
    /* 53  */ {0x1bu, 6u, {0, 0, 0}},
    /* 54  */ {0x1cu, 6u, {0, 0, 0}},
    /* 55  */ {0x1du, 6u, {0, 0, 0}},
    /* 56  */ {0x1eu, 6u, {0, 0, 0}},
    /* 57  */ {0x1fu, 6u, {0, 0, 0}},
    /* 58  */ {0x5cu, 7u, {0, 0, 0}},
    /* 59  */ {0xfbu, 8u, {0, 0, 0}},
    /* 60  */ {0x7ffcu, 15u, {0, 0, 0}},
    /* 61  */ {0x20u, 6u, {0, 0, 0}},
    /* 62  */ {0xffbu, 12u, {0, 0, 0}},
    /* 63  */ {0x3fcu, 10u, {0, 0, 0}},
    /* 64  */ {0x1ffau, 13u, {0, 0, 0}},
    /* 65  */ {0x21u, 6u, {0, 0, 0}},
    /* 66  */ {0x5du, 7u, {0, 0, 0}},
    /* 67  */ {0x5eu, 7u, {0, 0, 0}},
    /* 68  */ {0x5fu, 7u, {0, 0, 0}},
    /* 69  */ {0x60u, 7u, {0, 0, 0}},
    /* 70  */ {0x61u, 7u, {0, 0, 0}},
    /* 71  */ {0x62u, 7u, {0, 0, 0}},
    /* 72  */ {0x63u, 7u, {0, 0, 0}},
    /* 73  */ {0x64u, 7u, {0, 0, 0}},
    /* 74  */ {0x65u, 7u, {0, 0, 0}},
    /* 75  */ {0x66u, 7u, {0, 0, 0}},
    /* 76  */ {0x67u, 7u, {0, 0, 0}},
    /* 77  */ {0x68u, 7u, {0, 0, 0}},
    /* 78  */ {0x69u, 7u, {0, 0, 0}},
    /* 79  */ {0x6au, 7u, {0, 0, 0}},
    /* 80  */ {0x6bu, 7u, {0, 0, 0}},
    /* 81  */ {0x6cu, 7u, {0, 0, 0}},
    /* 82  */ {0x6du, 7u, {0, 0, 0}},
    /* 83  */ {0x6eu, 7u, {0, 0, 0}},
    /* 84  */ {0x6fu, 7u, {0, 0, 0}},
    /* 85  */ {0x70u, 7u, {0, 0, 0}},
    /* 86  */ {0x71u, 7u, {0, 0, 0}},
    /* 87  */ {0x72u, 7u, {0, 0, 0}},
    /* 88  */ {0xfcu, 8u, {0, 0, 0}},
    /* 89  */ {0x73u, 7u, {0, 0, 0}},
    /* 90  */ {0xfdu, 8u, {0, 0, 0}},
    /* 91  */ {0x1ffbu, 13u, {0, 0, 0}},
    /* 92  */ {0x7fff0u, 19u, {0, 0, 0}},
    /* 93  */ {0x1ffcu, 13u, {0, 0, 0}},
    /* 94  */ {0x3ffcu, 14u, {0, 0, 0}},
    /* 95  */ {0x22u, 6u, {0, 0, 0}},
    /* 96  */ {0x7ffdu, 15u, {0, 0, 0}},
    /* 97  */ {0x3u, 5u, {0, 0, 0}},
    /* 98  */ {0x23u, 6u, {0, 0, 0}},
    /* 99  */ {0x4u, 5u, {0, 0, 0}},
    /* 100 */ {0x24u, 6u, {0, 0, 0}},
    /* 101 */ {0x5u, 5u, {0, 0, 0}},
    /* 102 */ {0x25u, 6u, {0, 0, 0}},
    /* 103 */ {0x26u, 6u, {0, 0, 0}},
    /* 104 */ {0x27u, 6u, {0, 0, 0}},
    /* 105 */ {0x6u, 5u, {0, 0, 0}},
    /* 106 */ {0x74u, 7u, {0, 0, 0}},
    /* 107 */ {0x75u, 7u, {0, 0, 0}},
    /* 108 */ {0x28u, 6u, {0, 0, 0}},
    /* 109 */ {0x29u, 6u, {0, 0, 0}},
    /* 110 */ {0x2au, 6u, {0, 0, 0}},
    /* 111 */ {0x7u, 5u, {0, 0, 0}},
    /* 112 */ {0x2bu, 6u, {0, 0, 0}},
    /* 113 */ {0x76u, 7u, {0, 0, 0}},
    /* 114 */ {0x2cu, 6u, {0, 0, 0}},
    /* 115 */ {0x8u, 5u, {0, 0, 0}},
    /* 116 */ {0x9u, 5u, {0, 0, 0}},
    /* 117 */ {0x2du, 6u, {0, 0, 0}},
    /* 118 */ {0x77u, 7u, {0, 0, 0}},
    /* 119 */ {0x78u, 7u, {0, 0, 0}},
    /* 120 */ {0x79u, 7u, {0, 0, 0}},
    /* 121 */ {0x7au, 7u, {0, 0, 0}},
    /* 122 */ {0x7bu, 7u, {0, 0, 0}},
    /* 123 */ {0x7ffeu, 15u, {0, 0, 0}},
    /* 124 */ {0x7fcu, 11u, {0, 0, 0}},
    /* 125 */ {0x3ffdu, 14u, {0, 0, 0}},
    /* 126 */ {0x1ffdu, 13u, {0, 0, 0}},
    /* 127 */ {0xffffffcu, 28u, {0, 0, 0}},
    /* 128 */ {0xfffe6u, 20u, {0, 0, 0}},
    /* 129 */ {0x3fffd2u, 22u, {0, 0, 0}},
    /* 130 */ {0xfffe7u, 20u, {0, 0, 0}},
    /* 131 */ {0xfffe8u, 20u, {0, 0, 0}},
    /* 132 */ {0x3fffd3u, 22u, {0, 0, 0}},
    /* 133 */ {0x3fffd4u, 22u, {0, 0, 0}},
    /* 134 */ {0x3fffd5u, 22u, {0, 0, 0}},
    /* 135 */ {0x7fffd9u, 23u, {0, 0, 0}},
    /* 136 */ {0x3fffd6u, 22u, {0, 0, 0}},
    /* 137 */ {0x7fffdau, 23u, {0, 0, 0}},
    /* 138 */ {0x7fffdbu, 23u, {0, 0, 0}},
    /* 139 */ {0x7fffdcu, 23u, {0, 0, 0}},
    /* 140 */ {0x7fffddu, 23u, {0, 0, 0}},
    /* 141 */ {0x7fffdeu, 23u, {0, 0, 0}},
    /* 142 */ {0xffffebu, 24u, {0, 0, 0}},
    /* 143 */ {0x7fffdfu, 23u, {0, 0, 0}},
    /* 144 */ {0xffffecu, 24u, {0, 0, 0}},
    /* 145 */ {0xffffedu, 24u, {0, 0, 0}},
    /* 146 */ {0x3fffd7u, 22u, {0, 0, 0}},
    /* 147 */ {0x7fffe0u, 23u, {0, 0, 0}},
    /* 148 */ {0xffffeeu, 24u, {0, 0, 0}},
    /* 149 */ {0x7fffe1u, 23u, {0, 0, 0}},
    /* 150 */ {0x7fffe2u, 23u, {0, 0, 0}},
    /* 151 */ {0x7fffe3u, 23u, {0, 0, 0}},
    /* 152 */ {0x7fffe4u, 23u, {0, 0, 0}},
    /* 153 */ {0x1fffdcu, 21u, {0, 0, 0}},
    /* 154 */ {0x3fffd8u, 22u, {0, 0, 0}},
    /* 155 */ {0x7fffe5u, 23u, {0, 0, 0}},
    /* 156 */ {0x3fffd9u, 22u, {0, 0, 0}},
    /* 157 */ {0x7fffe6u, 23u, {0, 0, 0}},
    /* 158 */ {0x7fffe7u, 23u, {0, 0, 0}},
    /* 159 */ {0xffffefu, 24u, {0, 0, 0}},
    /* 160 */ {0x3fffdau, 22u, {0, 0, 0}},
    /* 161 */ {0x1fffddu, 21u, {0, 0, 0}},
    /* 162 */ {0xfffe9u, 20u, {0, 0, 0}},
    /* 163 */ {0x3fffdbu, 22u, {0, 0, 0}},
    /* 164 */ {0x3fffdcu, 22u, {0, 0, 0}},
    /* 165 */ {0x7fffe8u, 23u, {0, 0, 0}},
    /* 166 */ {0x7fffe9u, 23u, {0, 0, 0}},
    /* 167 */ {0x1fffdeu, 21u, {0, 0, 0}},
    /* 168 */ {0x7fffeau, 23u, {0, 0, 0}},
    /* 169 */ {0x3fffddu, 22u, {0, 0, 0}},
    /* 170 */ {0x3fffdeu, 22u, {0, 0, 0}},
    /* 171 */ {0xfffff0u, 24u, {0, 0, 0}},
    /* 172 */ {0x1fffdfu, 21u, {0, 0, 0}},
    /* 173 */ {0x3fffdfu, 22u, {0, 0, 0}},
    /* 174 */ {0x7fffebu, 23u, {0, 0, 0}},
    /* 175 */ {0x7fffecu, 23u, {0, 0, 0}},
    /* 176 */ {0x1fffe0u, 21u, {0, 0, 0}},
    /* 177 */ {0x1fffe1u, 21u, {0, 0, 0}},
    /* 178 */ {0x3fffe0u, 22u, {0, 0, 0}},
    /* 179 */ {0x1fffe2u, 21u, {0, 0, 0}},
    /* 180 */ {0x7fffedu, 23u, {0, 0, 0}},
    /* 181 */ {0x3fffe1u, 22u, {0, 0, 0}},
    /* 182 */ {0x7fffeeu, 23u, {0, 0, 0}},
    /* 183 */ {0x7fffefu, 23u, {0, 0, 0}},
    /* 184 */ {0xfffeau, 20u, {0, 0, 0}},
    /* 185 */ {0x3fffe2u, 22u, {0, 0, 0}},
    /* 186 */ {0x3fffe3u, 22u, {0, 0, 0}},
    /* 187 */ {0x3fffe4u, 22u, {0, 0, 0}},
    /* 188 */ {0x7ffff0u, 23u, {0, 0, 0}},
    /* 189 */ {0x3fffe5u, 22u, {0, 0, 0}},
    /* 190 */ {0x3fffe6u, 22u, {0, 0, 0}},
    /* 191 */ {0x7ffff1u, 23u, {0, 0, 0}},
    /* 192 */ {0x3ffffe0u, 26u, {0, 0, 0}},
    /* 193 */ {0x3ffffe1u, 26u, {0, 0, 0}},
    /* 194 */ {0xfffebu, 20u, {0, 0, 0}},
    /* 195 */ {0x7fff1u, 19u, {0, 0, 0}},
    /* 196 */ {0x3fffe7u, 22u, {0, 0, 0}},
    /* 197 */ {0x7ffff2u, 23u, {0, 0, 0}},
    /* 198 */ {0x3fffe8u, 22u, {0, 0, 0}},
    /* 199 */ {0x1ffffecu, 25u, {0, 0, 0}},
    /* 200 */ {0x3ffffe2u, 26u, {0, 0, 0}},
    /* 201 */ {0x3ffffe3u, 26u, {0, 0, 0}},
    /* 202 */ {0x3ffffe4u, 26u, {0, 0, 0}},
    /* 203 */ {0x7ffffdeu, 27u, {0, 0, 0}},
    /* 204 */ {0x7ffffdfu, 27u, {0, 0, 0}},
    /* 205 */ {0x3ffffe5u, 26u, {0, 0, 0}},
    /* 206 */ {0xfffff1u, 24u, {0, 0, 0}},
    /* 207 */ {0x1ffffedu, 25u, {0, 0, 0}},
    /* 208 */ {0x7fff2u, 19u, {0, 0, 0}},
    /* 209 */ {0x1fffe3u, 21u, {0, 0, 0}},
    /* 210 */ {0x3ffffe6u, 26u, {0, 0, 0}},
    /* 211 */ {0x7ffffe0u, 27u, {0, 0, 0}},
    /* 212 */ {0x7ffffe1u, 27u, {0, 0, 0}},
    /* 213 */ {0x3ffffe7u, 26u, {0, 0, 0}},
    /* 214 */ {0x7ffffe2u, 27u, {0, 0, 0}},
    /* 215 */ {0xfffff2u, 24u, {0, 0, 0}},
    /* 216 */ {0x1fffe4u, 21u, {0, 0, 0}},
    /* 217 */ {0x1fffe5u, 21u, {0, 0, 0}},
    /* 218 */ {0x3ffffe8u, 26u, {0, 0, 0}},
    /* 219 */ {0x3ffffe9u, 26u, {0, 0, 0}},
    /* 220 */ {0xffffffdu, 28u, {0, 0, 0}},
    /* 221 */ {0x7ffffe3u, 27u, {0, 0, 0}},
    /* 222 */ {0x7ffffe4u, 27u, {0, 0, 0}},
    /* 223 */ {0x7ffffe5u, 27u, {0, 0, 0}},
    /* 224 */ {0xfffecu, 20u, {0, 0, 0}},
    /* 225 */ {0xfffff3u, 24u, {0, 0, 0}},
    /* 226 */ {0xfffedu, 20u, {0, 0, 0}},
    /* 227 */ {0x1fffe6u, 21u, {0, 0, 0}},
    /* 228 */ {0x3fffe9u, 22u, {0, 0, 0}},
    /* 229 */ {0x1fffe7u, 21u, {0, 0, 0}},
    /* 230 */ {0x1fffe8u, 21u, {0, 0, 0}},
    /* 231 */ {0x7ffff3u, 23u, {0, 0, 0}},
    /* 232 */ {0x3fffeau, 22u, {0, 0, 0}},
    /* 233 */ {0x3fffebu, 22u, {0, 0, 0}},
    /* 234 */ {0x1ffffeeu, 25u, {0, 0, 0}},
    /* 235 */ {0x1ffffefu, 25u, {0, 0, 0}},
    /* 236 */ {0xfffff4u, 24u, {0, 0, 0}},
    /* 237 */ {0xfffff5u, 24u, {0, 0, 0}},
    /* 238 */ {0x3ffffeau, 26u, {0, 0, 0}},
    /* 239 */ {0x7ffff4u, 23u, {0, 0, 0}},
    /* 240 */ {0x3ffffebu, 26u, {0, 0, 0}},
    /* 241 */ {0x7ffffe6u, 27u, {0, 0, 0}},
    /* 242 */ {0x3ffffecu, 26u, {0, 0, 0}},
    /* 243 */ {0x3ffffedu, 26u, {0, 0, 0}},
    /* 244 */ {0x7ffffe7u, 27u, {0, 0, 0}},
    /* 245 */ {0x7ffffe8u, 27u, {0, 0, 0}},
    /* 246 */ {0x7ffffe9u, 27u, {0, 0, 0}},
    /* 247 */ {0x7ffffeau, 27u, {0, 0, 0}},
    /* 248 */ {0x7ffffebu, 27u, {0, 0, 0}},
    /* 249 */ {0xffffffeu, 28u, {0, 0, 0}}, /* corrected: 0xffffffe */
    /* 250 */ {0x7ffffecu, 27u, {0, 0, 0}},
    /* 251 */ {0x7ffffedu, 27u, {0, 0, 0}},
    /* 252 */ {0x7ffffeeu, 27u, {0, 0, 0}},
    /* 253 */ {0x7ffffefu, 27u, {0, 0, 0}},
    /* 254 */ {0x7fffff0u, 27u, {0, 0, 0}},
    /* 255 */ {0x3ffffeeu, 26u, {0, 0, 0}},
    /* 256 EOS */ {0x3fffffffu, 30u, {0, 0, 0}},
};

/*
 * huff_decode_long - slow path for HPACK Huffman codes longer than 8 bits.
 *
 * Invoked from huff_decode() when the 8-bit fast-path table returns
 * complete == 0 (the 0xfe and 0xff prefix slots). HPACK Huffman codes
 * longer than 8 bits all begin with the bit pattern 11111110 or
 * 11111111 (RFC 7541 Appendix B). The shortest such code is 10 bits
 * long, the longest (EOS) is 30 bits.
 *
 * Linear-searches the encode table for the unique code that matches
 * the most-significant L bits of `acc`, for L = 10..30. Because
 * Huffman codes are prefix-free, a match at length L is THE match -
 * no longer code can share the same L-bit prefix.
 *
 * Returns:
 *    > 0  number of bits consumed; *out_sym set to the decoded symbol
 *    = 0  need more input bits before any length is decidable
 *    < 0  EOS encountered (HIVE_ERR_COMPRESSION at caller per
 *         RFC 7541 5.2 - EOS in non-terminal position is a decoding error)
 *
 * See ARCHITECTURE.md 4.4. Performance: linear scan over the 257-entry
 * encode table; only invoked when input contains symbols whose Huffman
 * code is longer than 8 bits (rare in typical HTTP/2 header traffic).
 */
static int
huff_decode_long(uint64_t acc, int nbits, uint8_t *out_sym)
{
	int L;
	int s;
	uint32_t code;
	for (L = 10; L <= 30; L++) {
		if (nbits < L)
			return 0;
		code =
		    (uint32_t)((acc >> (nbits - L)) & (((uint64_t)1 << L) - 1));
		for (s = 0; s < 257; s++) {
			if (huff_encode_table[s].bits != (uint8_t)L)
				continue;
			if (huff_encode_table[s].code != code)
				continue;
			if (s == 256)
				return -1; /* EOS: non-terminal */
			*out_sym = (uint8_t)s;
			return L;
		}
	}
	return 0;
}
int
huff_decode(const uint8_t *src,
            size_t src_len,
            uint8_t *scratch,
            size_t max_len,
            size_t *out_len)
{
	uint64_t acc;
	size_t i;
	int nbits;
	size_t out;
	if (scratch == NULL || out_len == NULL)
		return HIVE_ERR_INVALID_ARG;
	acc = 0;
	nbits = 0;
	out = 0;
	for (i = 0; i < src_len; i++) {
		acc = (acc << 8) | (uint64_t)src[i];
		nbits += 8;
		while (nbits >= 8) {
			const huff_entry_t *e;
			uint8_t idx;
			idx = (uint8_t)(acc >> (nbits - 8));
			e = &huff_decode_table[idx];
			if (e->complete) {
				if (e->eos)
					return HIVE_ERR_COMPRESSION;
				if (out >= max_len)
					return HIVE_ERR_COMPRESSION;
				scratch[out++] = e->sym;
				nbits -= e->bits_consumed;
			} else {
				int bits_used;
				uint8_t sym;
				bits_used = huff_decode_long(acc, nbits, &sym);
				if (bits_used < 0)
					return HIVE_ERR_COMPRESSION;
				if (bits_used == 0)
					break; /* need more input */
				if (out >= max_len)
					return HIVE_ERR_COMPRESSION;
				scratch[out++] = sym;
				nbits -= bits_used;
			}
			if (nbits == 0)
				acc = 0;
			else
				acc &= ((uint64_t)1 << nbits) - 1;
		}
	}
	/*
	 * Padding validation per RFC 7541 5.2:
	 * leftover bits must be 0..7 and all-ones (high-order EOS bits).
	 */
	if (nbits > 7)
		return HIVE_ERR_COMPRESSION;
	if (nbits > 0 && acc != ((uint64_t)1 << nbits) - 1)
		return HIVE_ERR_COMPRESSION;
	*out_len = out;
	return HIVE_OK;
}
int
huff_encode(const uint8_t *src,
            size_t src_len,
            uint8_t *out,
            size_t out_cap,
            size_t *out_len)
{
	uint64_t acc;
	size_t i;
	int nbits;
	size_t pos;
	if (out == NULL || out_len == NULL)
		return HIVE_ERR_INVALID_ARG;
	acc = 0;
	nbits = 0;
	pos = 0;
	for (i = 0; i < src_len; i++) {
		const huff_sym_t *sym;
		sym = &huff_encode_table[src[i]];
		acc = (acc << sym->bits) | sym->code;
		nbits += sym->bits;
		while (nbits >= 8) {
			nbits -= 8;
			/*
			 * Output buffer overflow is a caller-sizing error,
			 * not a compression error. HIVE_ERR_NOMEM is the
			 * closest fit in the existing error enum until a
			 * dedicated HIVE_ERR_BUFFER_TOO_SMALL is introduced.
			 */
			if (pos >= out_cap)
				return HIVE_ERR_NOMEM;
			out[pos++] = (uint8_t)((acc >> nbits) & 0xffu);
			if (nbits == 0)
				acc = 0;
			else
				acc &= ((uint64_t)1 << nbits) - 1;
		}
	}
	if (nbits > 0) {
		uint8_t b;
		if (pos >= out_cap)
			return HIVE_ERR_NOMEM;
		b = (uint8_t)((acc << (8 - nbits)) & 0xffu);
		b |= (uint8_t)((1u << (8 - nbits)) - 1u);
		out[pos++] = b;
	}
	*out_len = pos;
	return HIVE_OK;
}

/* ------------------------------------------------------------------ */
/* Dynamic table — Task 3.1                                            */
/* See ARCHITECTURE.md §4.1, §4.2, §8.1.                              */
/* ------------------------------------------------------------------ */

/*
 * hpack_ring_cap — compute ring buffer capacity from max_size.
 *
 * Returns next_power_of_two(max_size / 32), minimum 4.
 * The minimum 4 avoids degenerate tables at very small max_size values.
 * Power-of-two is required for the (ring_head - n) & (ring_cap - 1)
 * modulo idiom used throughout the ring buffer code.
 */
static uint32_t
hpack_ring_cap(uint32_t max_size)
{
	uint32_t n;

	n = max_size / 32u;
	if (n < 4u)
		return 4u;

	/* round up to next power of two */
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	n++;
	return n;
}

int
hpack_table_init(hpack_table_t *t, const hive_mem_t *mem, uint32_t max_size)
{
	uint32_t cap;

	memset(t, 0, sizeof(*t));
	cap = hpack_ring_cap(max_size);

	t->ring = (hpack_entry_t **)mem->calloc(
	    cap, sizeof(hpack_entry_t *), mem->ctx);
	if (t->ring == NULL)
		return HIVE_ERR_NOMEM;

	t->ring_cap = cap;
	t->max_size = max_size;
	t->pending_max = max_size;
	t->pending_min = max_size;
	/* hash, hash_mask, count, size, ring_head, has_pending: all 0 */
	return HIVE_OK;
}

void
hpack_table_free(hpack_table_t *t, const hive_mem_t *mem)
{
	uint32_t i;
	uint32_t idx;

	for (i = 0; i < t->count; i++) {
		idx = (t->ring_head - t->count + i) & (t->ring_cap - 1);
		if (t->ring[idx] != NULL)
			mem->free(t->ring[idx], mem->ctx);
	}
	if (t->hash != NULL)
		mem->free(t->hash, mem->ctx);
	mem->free((void *)t->ring, mem->ctx);
	memset(t, 0, sizeof(*t));
}

void
hpack_table_evict_to(hpack_table_t *t, const hive_mem_t *mem, uint32_t new_max)
{
	uint32_t oldest_idx;
	hpack_entry_t *oldest;

	while (t->count > 0 && t->size > new_max) {
		oldest_idx = (t->ring_head - t->count) & (t->ring_cap - 1);
		oldest = t->ring[oldest_idx];
		t->size -= HPACK_ENTRY_RFC_SIZE(oldest);
		t->ring[oldest_idx] = NULL;
		t->count--;
		mem->free(oldest, mem->ctx);
	}
}

int
hpack_table_insert(hpack_table_t *t,
                   const hive_mem_t *mem,
                   const uint8_t *name,
                   uint32_t name_len,
                   const uint8_t *value,
                   uint32_t value_len)
{
	uint64_t alloc64;
	uint32_t rfc_size;
	hpack_entry_t *entry;

	/* Integer overflow guards per ARCHITECTURE.md §4.2. */
	alloc64 = (uint64_t)name_len + (uint64_t)value_len;
	if (alloc64 > (uint64_t)(SIZE_MAX - sizeof(hpack_entry_t)))
		return HIVE_ERR_COMPRESSION;
	if (alloc64 > (uint64_t)(UINT32_MAX - 32u))
		return HIVE_ERR_COMPRESSION;

	rfc_size = (uint32_t)(alloc64 + 32u);

	/*
	 * Evict oldest entries until there is room for the new entry,
	 * or until the table is empty.  If rfc_size > max_size the loop
	 * empties the table and the entry is then not inserted below
	 * (RFC 7541 §4.4 oversized entry rule).
	 */
	while (t->count > 0 && t->size + rfc_size > t->max_size) {
		uint32_t oldest_idx;
		hpack_entry_t *oldest;

		oldest_idx = (t->ring_head - t->count) & (t->ring_cap - 1);
		oldest = t->ring[oldest_idx];
		t->size -= HPACK_ENTRY_RFC_SIZE(oldest);
		t->ring[oldest_idx] = NULL;
		t->count--;
		mem->free(oldest, mem->ctx);
	}

	/*
	 * Oversized entry: rfc_size still exceeds max_size after full
	 * eviction.  Table is now empty; do not insert.
	 * See ARCHITECTURE.md §4.2.
	 */
	if (rfc_size > t->max_size)
		return HIVE_OK;

	entry = (hpack_entry_t *)mem->malloc(
	    sizeof(hpack_entry_t) + (size_t)name_len + (size_t)value_len,
	    mem->ctx);
	if (entry == NULL)
		return HIVE_ERR_NOMEM;

	/*
	 * SECURITY: always copy name and value bytes into the allocated entry.
	 * Never store a pointer into caller memory — the source may be
	 * reassembly_buf, a stack scratch buffer, or a static table region
	 * that is invalidated or reused after this call returns.
	 * See ARCHITECTURE.md §8.1 and CODING_STANDARDS.md §3.2.
	 */
	entry->name_len = name_len;
	entry->value_len = value_len;
	if (name_len > 0)
		memcpy(HPACK_ENTRY_NAME(entry), name, name_len);
	if (value_len > 0)
		memcpy(HPACK_ENTRY_VALUE(entry), value, value_len);

	t->ring[t->ring_head] = entry;
	t->ring_head = (t->ring_head + 1u) & (t->ring_cap - 1u);
	t->count++;
	t->size += rfc_size;

	return HIVE_OK;
}

int
hpack_table_lookup(const hpack_table_t *t,
                   const uint8_t *name,
                   uint32_t name_len,
                   const uint8_t *value,
                   uint32_t value_len,
                   uint32_t *out_dyn_idx)
{
	uint32_t i;
	uint32_t name_only_idx;

	name_only_idx = UINT32_MAX; /* sentinel: no name-only match yet */

	/*
	 * Linear scan from newest entry backward.
	 * i=0 is the newest; i=count-1 is the oldest.
	 * The first exact match found is the newest and is returned
	 * immediately.  For name-only matches, the first (newest) is kept.
	 */
	for (i = 0; i < t->count; i++) {
		uint32_t idx;
		const hpack_entry_t *e;

		idx = (t->ring_head - 1u - i) & (t->ring_cap - 1u);
		e = t->ring[idx];

		if (e->name_len != name_len)
			continue;
		if (memcmp(HPACK_ENTRY_NAME(e), name, name_len) != 0)
			continue;

		/* Name matches — check value */
		if (e->value_len == value_len &&
		    (value_len == 0 ||
		     memcmp(HPACK_ENTRY_VALUE(e), value, value_len) == 0)) {
			/* Exact match — return immediately (newest preferred)
			 */
			*out_dyn_idx = i;
			return HPACK_LOOKUP_EXACT;
		}

		/* Name-only: record first (newest) occurrence */
		if (name_only_idx == UINT32_MAX)
			name_only_idx = i;
	}

	if (name_only_idx != UINT32_MAX) {
		*out_dyn_idx = name_only_idx;
		return HPACK_LOOKUP_NAME_ONLY;
	}

	return HPACK_LOOKUP_NOT_FOUND;
}

const hpack_entry_t *
hpack_table_get(const hpack_table_t *t, uint32_t dyn_idx)
{
	uint32_t idx;

	idx = (t->ring_head - 1u - dyn_idx) & (t->ring_cap - 1u);
	return t->ring[idx];
}

/* ------------------------------------------------------------------ */
/* Integer varint encode/decode — Task 3.2                            */
/* See ARCHITECTURE.md §4.7 and RFC 7541 §5.1.                        */
/* ------------------------------------------------------------------ */

/*
 * hpack_decode_int — decode HPACK varint from src[0..len).
 *
 * See ARCHITECTURE.md §4.7 for the exact algorithm.
 * Returns the decoded value, or HPACK_INT_OVERFLOW on any error
 * (truncated input or integer overflow).
 *
 * The 64-bit intermediate `tmp` detects overflow before 32-bit
 * truncation.  The m > 28 guard prevents reading more than 5
 * continuation bytes regardless of the values they carry.
 */
uint32_t
hpack_decode_int(const uint8_t *src,
                 size_t len,
                 int prefix_bits,
                 size_t *consumed)
{
	uint32_t prefix_max;
	uint32_t val;
	uint32_t m;

	prefix_max = (1u << prefix_bits) - 1u;

	if (len == 0)
		return HPACK_INT_OVERFLOW; /* truncated */

	val = src[0] & prefix_max;
	*consumed = 1;

	if (val < prefix_max)
		return val; /* fits in prefix — single byte */

	/* Multi-byte continuation */
	m = 0;
	while (*consumed < len) {
		uint8_t b;
		uint64_t tmp;

		b = src[(*consumed)++];
		/* Use 64-bit intermediate to detect overflow before m=28 */
		tmp = (uint64_t)val + ((uint64_t)(b & 0x7Fu) << m);
		if (tmp > (uint64_t)UINT32_MAX)
			return HPACK_INT_OVERFLOW;
		val = (uint32_t)tmp;
		m += 7;
		if (!(b & 0x80u))
			return val; /* complete — continuation bit clear */
		if (m > 28)
			return HPACK_INT_OVERFLOW; /* overflow guard */
	}
	return HPACK_INT_OVERFLOW; /* truncated — ran out of input bytes */
}

/*
 * hpack_encode_int — encode val with N-bit prefix into out[0..out_cap).
 *
 * See RFC 7541 §5.1.  prefix_top provides the upper (8 - prefix_bits)
 * bits that are ORed into the first byte without modification.
 *
 * Returns bytes written (>= 1), or 0 if the buffer is too small.
 */
size_t
hpack_encode_int(uint8_t *out,
                 size_t out_cap,
                 uint8_t prefix_top,
                 int prefix_bits,
                 uint32_t val)
{
	uint32_t prefix_max;
	size_t pos;

	if (out_cap == 0)
		return 0;

	prefix_max = (1u << prefix_bits) - 1u;
	pos = 0;

	if (val < prefix_max) {
		/* Value fits in the prefix — single byte */
		out[pos++] = prefix_top | (uint8_t)val;
		return pos;
	}

	/* Value does not fit: fill prefix bits, then encode remainder */
	out[pos++] = prefix_top | (uint8_t)prefix_max;
	val -= prefix_max;

	while (val >= 128u) {
		if (pos >= out_cap)
			return 0; /* buffer too small */
		out[pos++] = (uint8_t)((val & 0x7Fu) | 0x80u);
		val >>= 7;
	}
	if (pos >= out_cap)
		return 0; /* buffer too small */
	out[pos++] = (uint8_t)val;
	return pos;
}
