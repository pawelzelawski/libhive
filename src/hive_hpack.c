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

/* ------------------------------------------------------------------ */
/* String decode — Task 3.3                                            */
/* See ARCHITECTURE.md §4.6.                                           */
/* ------------------------------------------------------------------ */

/*
 * hpack_decode_string — decode one HPACK string field.
 *
 * Detects the Huffman flag in bit 7 of the first byte, decodes the
 * 7-bit string length prefix, validates that the claimed bytes are
 * present in src, then either Huffman-decodes into scratch or returns
 * a direct pointer into the source buffer for literal strings.
 *
 * See ARCHITECTURE.md §4.6 for the full specification.
 */
int
hpack_decode_string(const uint8_t *src,
                    size_t src_len,
                    uint8_t *scratch,
                    size_t scratch_cap,
                    hive_buf_t *out,
                    size_t *consumed)
{
	uint32_t slen;
	size_t hdr_consumed;
	int is_huffman;

	if (src_len < 1)
		return HIVE_ERR_COMPRESSION; /* truncated — no length byte */

	is_huffman = (src[0] >> 7) & 1;

	/* Decode the 7-bit prefix string length (RFC 7541 §5.2) */
	slen = hpack_decode_int(src, src_len, 7, &hdr_consumed);
	if (slen == HPACK_INT_OVERFLOW)
		return HIVE_ERR_COMPRESSION; /* truncated or overflow */

	/* Claimed string bytes must all be present in the block */
	if ((size_t)slen > src_len - hdr_consumed)
		return HIVE_ERR_COMPRESSION; /* truncated string */

	/*
	 * Reject strings whose length exceeds the configured maximum.
	 * For literal strings this is the definitive check.
	 * For Huffman strings, huff_decode() enforces scratch_cap on
	 * the decoded output; we also reject here to fail fast on an
	 * obviously oversized compressed form (encoded >= decoded).
	 * See ARCHITECTURE.md §4.6.
	 */
	if ((size_t)slen > scratch_cap)
		return HIVE_ERR_COMPRESSION;

	if (is_huffman) {
		size_t out_len;
		int ret;

		ret = huff_decode(src + hdr_consumed,
		                  (size_t)slen,
		                  scratch,
		                  scratch_cap,
		                  &out_len);
		if (ret != HIVE_OK)
			return HIVE_ERR_COMPRESSION;
		out->data = scratch;
		out->len = out_len;
	} else {
		/*
		 * Non-Huffman: return a direct pointer into the source
		 * buffer — no copy.  The caller must not modify the
		 * source buffer while this hive_buf_t is in use.
		 * See ARCHITECTURE.md §4.6.
		 */
		out->data = src + hdr_consumed;
		out->len = (size_t)slen;
	}

	out->flags = HIVE_BUF_VALID;
	*consumed = hdr_consumed + (size_t)slen;
	return HIVE_OK;
}

static int
hpack_index_to_header(const hive_session_t *s,
                      uint32_t index,
                      hive_buf_t *name,
                      hive_buf_t *value)
{
	if (index == 0)
		return HIVE_ERR_COMPRESSION;

	if (index <= HPACK_STATIC_TABLE_SIZE) {
		const hive_nv_t *nv;

		nv = &hpack_static_table[index - 1u];
		name->data = nv->name;
		name->len = nv->name_len;
		name->flags = HIVE_BUF_VALID;
		value->data = nv->value;
		value->len = nv->value_len;
		value->flags = HIVE_BUF_VALID;
		return HIVE_OK;
	}

	index -= (HPACK_STATIC_TABLE_SIZE + 1u);
	if (index >= s->dec_table.count)
		return HIVE_ERR_COMPRESSION;

	{
		const hpack_entry_t *e;

		e = hpack_table_get(&s->dec_table, index);
		name->data = HPACK_ENTRY_NAME(e);
		name->len = e->name_len;
		name->flags = HIVE_BUF_VALID;
		value->data = HPACK_ENTRY_VALUE(e);
		value->len = e->value_len;
		value->flags = HIVE_BUF_VALID;
	}

	return HIVE_OK;
}

static int
hpack_index_to_name(const hive_session_t *s, uint32_t index, hive_buf_t *name)
{
	hive_buf_t ignored_value;
	int ret;

	ret = hpack_index_to_header(s, index, name, &ignored_value);
	if (ret != HIVE_OK)
		return ret;
	return HIVE_OK;
}

static int
hpack_ptr_in_region(const uint8_t *ptr,
                    size_t len,
                    const uint8_t *region,
                    size_t region_len)
{
	uintptr_t p0;
	uintptr_t p1;
	uintptr_t r0;
	uintptr_t r1;

	if (ptr == NULL || len == 0 || region == NULL || region_len == 0)
		return 0;

	p0 = (uintptr_t)ptr;
	p1 = p0 + len;
	r0 = (uintptr_t)region;
	r1 = r0 + region_len;
	if (p1 < p0 || r1 < r0)
		return 0;

	return p0 >= r0 && p1 <= r1;
}

static void
hpack_poison_if_ephemeral(const hive_session_t *s, const hive_buf_t *buf)
{
	if ((buf->flags & HIVE_BUF_VALID) != 0)
		return;

	if (hpack_ptr_in_region(buf->data,
	                        buf->len,
	                        s->reassembly_buf,
	                        s->opt_max_continuation_size)) {
		HIVE_ASAN_POISON(buf->data, buf->len);
		return;
	}
	if (hpack_ptr_in_region(buf->data,
	                        buf->len,
	                        s->hpack_scratch_name,
	                        s->opt_max_header_string_size)) {
		HIVE_ASAN_POISON(buf->data, buf->len);
		return;
	}
	if (hpack_ptr_in_region(buf->data,
	                        buf->len,
	                        s->hpack_scratch_value,
	                        s->opt_max_header_string_size)) {
		HIVE_ASAN_POISON(buf->data, buf->len);
	}
}

static int
hpack_validate_http_messaging_stub(const hive_buf_t *name,
                                   const hive_buf_t *value)
{
	(void)name;
	(void)value;
	/* Full RFC 9113 §8 validation is implemented in Phase 7. */
	return HIVE_OK;
}

int
hpack_decode_block(hive_session_t *s,
                   const uint8_t *data,
                   size_t len,
                   int suppress_callbacks,
                   uint32_t error_stream_id)
{
	size_t pos;
	uint64_t decoded_size;
	uint32_t decoded_count;
	int size_update_phase;
	int stream_error_pending;
	int cb_ret;

	(void)error_stream_id;

	if (s == NULL || (len > 0 && data == NULL))
		return HIVE_ERR_INVALID_ARG;

	pos = 0;
	decoded_size = 0;
	decoded_count = 0;
	size_update_phase = 1;
	stream_error_pending = suppress_callbacks ? 1 : 0;

	if (!suppress_callbacks && s->callbacks.on_begin_headers != NULL) {
		cb_ret = s->callbacks.on_begin_headers(
		    s, s->reassembly_stream_id, s->user_data);
		if (cb_ret == HIVE_ERR_COMPRESSION)
			return HIVE_ERR_COMPRESSION;
		if (cb_ret != HIVE_OK)
			stream_error_pending = 1;
	}

	while (pos < len) {
		hive_buf_t *name_buf;
		hive_buf_t *value_buf;
		size_t consumed;
		uint32_t idx;
		uint8_t header_flags;
		int ret;

		name_buf = &s->hpack_name_handle;
		value_buf = &s->hpack_value_handle;
		memset(name_buf, 0, sizeof(*name_buf));
		memset(value_buf, 0, sizeof(*value_buf));
		header_flags = 0;

		if (data[pos] & 0x80u) {
			/* Indexed header field representation (RFC 7541 §6.1).
			 */
			size_update_phase = 0;
			idx = hpack_decode_int(
			    data + pos, len - pos, 7, &consumed);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;

			ret =
			    hpack_index_to_header(s, idx, name_buf, value_buf);
			if (ret != HIVE_OK)
				return HIVE_ERR_COMPRESSION;
		} else if (data[pos] & 0x40u) {
			/* Literal with incremental indexing (RFC 7541 §6.2.1).
			 */
			size_update_phase = 0;
			idx = hpack_decode_int(
			    data + pos, len - pos, 6, &consumed);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;

			if (idx == 0) {
				ret = hpack_decode_string(
				    data + pos,
				    len - pos,
				    s->hpack_scratch_name,
				    s->opt_max_header_string_size,
				    name_buf,
				    &consumed);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
				pos += consumed;
			} else {
				ret = hpack_index_to_name(s, idx, name_buf);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
			}

			ret = hpack_decode_string(data + pos,
			                          len - pos,
			                          s->hpack_scratch_value,
			                          s->opt_max_header_string_size,
			                          value_buf,
			                          &consumed);
			if (ret != HIVE_OK)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;

			ret = hpack_table_insert(&s->dec_table,
			                         &s->mem,
			                         name_buf->data,
			                         (uint32_t)name_buf->len,
			                         value_buf->data,
			                         (uint32_t)value_buf->len);
			if (ret != HIVE_OK)
				return ret;
		} else if (data[pos] & 0x20u) {
			/* Dynamic table size update (RFC 7541 §6.3). */
			if (!size_update_phase)
				return HIVE_ERR_COMPRESSION;

			idx = hpack_decode_int(
			    data + pos, len - pos, 5, &consumed);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;

			if (idx > s->dec_table.pending_max)
				return HIVE_ERR_COMPRESSION;
			hpack_table_evict_to(&s->dec_table, &s->mem, idx);
			s->dec_table.max_size = idx;
			continue;
		} else {
			/* Literal without indexing / never indexed (§6.2.2 /
			 * §6.2.3). */
			size_update_phase = 0;
			if ((data[pos] & 0xf0u) == 0x10u)
				header_flags = HIVE_NV_FLAG_NO_INDEX;

			idx = hpack_decode_int(
			    data + pos, len - pos, 4, &consumed);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;

			if (idx == 0) {
				ret = hpack_decode_string(
				    data + pos,
				    len - pos,
				    s->hpack_scratch_name,
				    s->opt_max_header_string_size,
				    name_buf,
				    &consumed);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
				pos += consumed;
			} else {
				ret = hpack_index_to_name(s, idx, name_buf);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
			}

			ret = hpack_decode_string(data + pos,
			                          len - pos,
			                          s->hpack_scratch_value,
			                          s->opt_max_header_string_size,
			                          value_buf,
			                          &consumed);
			if (ret != HIVE_OK)
				return HIVE_ERR_COMPRESSION;
			pos += consumed;
		}

		decoded_size += name_buf->len + value_buf->len + 32u;
		decoded_count++;
		/* SECURITY: enforce HPACK bomb limits incrementally per entry.
		 */
		if (decoded_size > s->opt_max_header_list_size)
			stream_error_pending = 1;
		if (decoded_count > s->opt_max_header_count)
			stream_error_pending = 1;

		if (s->opt_no_http_messaging == 0 && !stream_error_pending) {
			ret = hpack_validate_http_messaging_stub(name_buf,
			                                         value_buf);
			if (ret != HIVE_OK)
				stream_error_pending = 1;
		}

		if (!suppress_callbacks && !stream_error_pending &&
		    s->callbacks.on_header != NULL) {
			cb_ret = s->callbacks.on_header(s,
			                                s->reassembly_stream_id,
			                                name_buf,
			                                value_buf,
			                                header_flags,
			                                s->user_data);
			name_buf->flags &= (uint8_t)~HIVE_BUF_VALID;
			value_buf->flags &= (uint8_t)~HIVE_BUF_VALID;
			hpack_poison_if_ephemeral(s, name_buf);
			hpack_poison_if_ephemeral(s, value_buf);

			if (cb_ret == HIVE_ERR_COMPRESSION)
				return HIVE_ERR_COMPRESSION;
			if (cb_ret != HIVE_OK)
				stream_error_pending = 1;
		} else {
			name_buf->flags &= (uint8_t)~HIVE_BUF_VALID;
			value_buf->flags &= (uint8_t)~HIVE_BUF_VALID;
			hpack_poison_if_ephemeral(s, name_buf);
			hpack_poison_if_ephemeral(s, value_buf);
		}
	}

	if (stream_error_pending)
		return HIVE_ERR_PROTOCOL;

	if (!suppress_callbacks && s->callbacks.on_headers_complete != NULL) {
		cb_ret =
		    s->callbacks.on_headers_complete(s,
		                                     s->reassembly_stream_id,
		                                     s->reassembly_end_stream,
		                                     s->user_data);
		if (cb_ret == HIVE_ERR_COMPRESSION)
			return HIVE_ERR_COMPRESSION;
		if (cb_ret != HIVE_OK)
			return HIVE_ERR_PROTOCOL;
	}

	return HIVE_OK;
}

/* ------------------------------------------------------------------ */
/* String encode — Task 3.3                                            */
/* See ARCHITECTURE.md §4.8.                                           */
/* ------------------------------------------------------------------ */

/*
 * hpack_encode_string — encode one string into an HPACK wire block.
 *
 * Computes the Huffman-encoded byte length by summing bit-lengths from
 * the encode table, then Huffman-encodes if strictly shorter, otherwise
 * writes the literal form.  Both paths use hpack_encode_int() for the
 * 7-bit length prefix.
 *
 * See ARCHITECTURE.md §4.8 and RFC 7541 §5.2.
 */
size_t
hpack_encode_string(const uint8_t *src,
                    size_t src_len,
                    uint8_t *out,
                    size_t out_cap)
{
	size_t i;
	uint32_t total_bits;
	uint32_t huff_len;
	uint8_t hdr_buf[8];
	size_t hdr_len;
	size_t pos;

	/* Compute the Huffman-encoded byte length without writing */
	total_bits = 0;
	for (i = 0; i < src_len; i++)
		total_bits += (uint32_t)huff_encode_table[src[i]].bits;
	huff_len = (total_bits + 7u) / 8u;

	if (huff_len < (uint32_t)src_len) {
		/*
		 * Huffman encoding is strictly shorter — use it.
		 * Encode the length with the Huffman flag (bit 7 = 1).
		 */
		size_t enc_len;
		int ret;

		hdr_len = hpack_encode_int(
		    hdr_buf, sizeof(hdr_buf), 0x80u, 7, huff_len);
		if (hdr_len == 0)
			return 0; /* buffer sizing error (cannot happen) */

		if (hdr_len + (size_t)huff_len > out_cap)
			return 0; /* output buffer too small */

		memcpy(out, hdr_buf, hdr_len);
		pos = hdr_len;

		ret = huff_encode(
		    src, src_len, out + pos, out_cap - pos, &enc_len);
		if (ret != HIVE_OK)
			return 0;

		pos += enc_len;
		return pos;
	}

	/*
	 * Literal form — no Huffman.
	 * Encode the length with the Huffman flag cleared (bit 7 = 0).
	 */
	hdr_len = hpack_encode_int(
	    hdr_buf, sizeof(hdr_buf), 0x00u, 7, (uint32_t)src_len);
	if (hdr_len == 0)
		return 0;

	if (hdr_len + src_len > out_cap)
		return 0; /* output buffer too small */

	memcpy(out, hdr_buf, hdr_len);
	pos = hdr_len;

	if (src_len > 0)
		memcpy(out + pos, src, src_len);

	pos += src_len;
	return pos;
}

static int
hpack_nv_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len)
{
	if (a_len != b_len)
		return 0;
	if (a_len == 0)
		return 1;
	return memcmp(a, b, a_len) == 0;
}

static int
hpack_static_lookup(const hive_nv_t *nv,
                    uint32_t *exact_idx,
                    uint32_t *name_idx)
{
	uint32_t i;

	*exact_idx = 0;
	*name_idx = 0;
	for (i = 0; i < HPACK_STATIC_TABLE_SIZE; i++) {
		const hive_nv_t *st;

		st = &hpack_static_table[i];
		if (!hpack_nv_equal(
		        st->name, st->name_len, nv->name, nv->name_len))
			continue;

		if (*name_idx == 0)
			*name_idx = i + 1u;
		if (hpack_nv_equal(
		        st->value, st->value_len, nv->value, nv->value_len)) {
			*exact_idx = i + 1u;
			return HPACK_LOOKUP_EXACT;
		}
	}

	if (*name_idx != 0)
		return HPACK_LOOKUP_NAME_ONLY;
	return HPACK_LOOKUP_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* Full HPACK block encode — Task 3.5                                 */
/* See ARCHITECTURE.md §4.8 and RFC 7541 §6.1/§6.2/§6.3.             */
/* ------------------------------------------------------------------ */

int
hpack_encode_block(hpack_table_t *table,
                   const hive_mem_t *mem,
                   const hive_nv_t *nva,
                   size_t nvlen,
                   uint8_t *out,
                   size_t out_cap,
                   size_t *out_len)
{
	size_t pos;
	size_t i;

	if (table == NULL || mem == NULL || out == NULL || out_len == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (nvlen > 0 && nva == NULL)
		return HIVE_ERR_INVALID_ARG;

	pos = 0;

	if (table->has_pending) {
		size_t n;
		uint32_t min_sz;
		uint32_t max_sz;

		min_sz = table->pending_min;
		max_sz = table->pending_max;

		n = hpack_encode_int(
		    out + pos, out_cap - pos, 0x20u, 5, min_sz);
		if (n == 0)
			return HIVE_ERR_NOMEM;
		pos += n;

		if (max_sz != min_sz) {
			n = hpack_encode_int(
			    out + pos, out_cap - pos, 0x20u, 5, max_sz);
			if (n == 0)
				return HIVE_ERR_NOMEM;
			pos += n;
		}

		hpack_table_evict_to(table, mem, min_sz);
		table->max_size = min_sz;
		if (max_sz != min_sz) {
			hpack_table_evict_to(table, mem, max_sz);
			table->max_size = max_sz;
		}
		table->has_pending = 0;
		table->pending_min = table->pending_max;
	}

	for (i = 0; i < nvlen; i++) {
		const hive_nv_t *nv;
		uint32_t static_exact;
		uint32_t static_name;
		uint32_t dyn_idx;
		uint32_t idx;
		size_t n;
		size_t slen;
		int st_match;
		int dyn_match;

		nv = &nva[i];
		if (nv->name == NULL || nv->name_len > UINT32_MAX ||
		    nv->value_len > UINT32_MAX)
			return HIVE_ERR_INVALID_ARG;

		st_match = hpack_static_lookup(nv, &static_exact, &static_name);
		if (st_match == HPACK_LOOKUP_EXACT) {
			n = hpack_encode_int(
			    out + pos, out_cap - pos, 0x80u, 7, static_exact);
			if (n == 0)
				return HIVE_ERR_NOMEM;
			pos += n;
			continue;
		}

		dyn_match = hpack_table_lookup(table,
		                               nv->name,
		                               (uint32_t)nv->name_len,
		                               nv->value,
		                               (uint32_t)nv->value_len,
		                               &dyn_idx);
		if (dyn_match == HPACK_LOOKUP_EXACT) {
			idx = HPACK_STATIC_TABLE_SIZE + 1u + dyn_idx;
			n = hpack_encode_int(
			    out + pos, out_cap - pos, 0x80u, 7, idx);
			if (n == 0)
				return HIVE_ERR_NOMEM;
			pos += n;
			continue;
		}

		if (st_match == HPACK_LOOKUP_NAME_ONLY) {
			idx = static_name;
			n = hpack_encode_int(
			    out + pos, out_cap - pos, 0x40u, 6, idx);
			if (n == 0)
				return HIVE_ERR_NOMEM;
			pos += n;

			slen = hpack_encode_string(
			    nv->value, nv->value_len, out + pos, out_cap - pos);
			if (slen == 0)
				return HIVE_ERR_NOMEM;
			pos += slen;

			if ((nv->flags & HIVE_NV_FLAG_NO_INDEX) == 0) {
				int ret;

				ret =
				    hpack_table_insert(table,
				                       mem,
				                       nv->name,
				                       (uint32_t)nv->name_len,
				                       nv->value,
				                       (uint32_t)nv->value_len);
				if (ret != HIVE_OK)
					return ret;
			}
			continue;
		}

		if (dyn_match == HPACK_LOOKUP_NAME_ONLY) {
			idx = HPACK_STATIC_TABLE_SIZE + 1u + dyn_idx;
			n = hpack_encode_int(
			    out + pos, out_cap - pos, 0x40u, 6, idx);
			if (n == 0)
				return HIVE_ERR_NOMEM;
			pos += n;

			slen = hpack_encode_string(
			    nv->value, nv->value_len, out + pos, out_cap - pos);
			if (slen == 0)
				return HIVE_ERR_NOMEM;
			pos += slen;

			if ((nv->flags & HIVE_NV_FLAG_NO_INDEX) == 0) {
				int ret;

				ret =
				    hpack_table_insert(table,
				                       mem,
				                       nv->name,
				                       (uint32_t)nv->name_len,
				                       nv->value,
				                       (uint32_t)nv->value_len);
				if (ret != HIVE_OK)
					return ret;
			}
			continue;
		}

		/* Literal fallback (RFC 7541 §6.2.2, never indexed when
		 * requested). */
		n = hpack_encode_int(
		    out + pos,
		    out_cap - pos,
		    (nv->flags & HIVE_NV_FLAG_NO_INDEX) ? 0x10u : 0x00u,
		    4,
		    0);
		if (n == 0)
			return HIVE_ERR_NOMEM;
		pos += n;

		slen = hpack_encode_string(
		    nv->name, nv->name_len, out + pos, out_cap - pos);
		if (slen == 0)
			return HIVE_ERR_NOMEM;
		pos += slen;

		slen = hpack_encode_string(
		    nv->value, nv->value_len, out + pos, out_cap - pos);
		if (slen == 0)
			return HIVE_ERR_NOMEM;
		pos += slen;
	}

	*out_len = pos;
	return HIVE_OK;
}
