/*
 * run_tests.c — test binary entry point
 *
 * Calls RUN() for every test case across all test files.
 * Returns 0 if all pass, 1 if any fail.
 * See TECH_STACK.md §6.1.
 */

#include <stdio.h>

#include "test_harness.h"

/* --- test_compat.c -------------------------------------------------------- */
int test_strlcpy_basic(void);
int test_strlcpy_truncation(void);
int test_strlcpy_empty_src(void);
int test_strlcat_basic(void);
int test_strlcat_full_dst(void);

/* --- test_frame.c --------------------------------------------------------- */
int test_frame_hdr_write_data(void);
int test_frame_hdr_write_settings(void);
int test_frame_hdr_write_headers(void);

/* --- test_hpack.c --------------------------------------------------------- */
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
main(void)
{
	/* Phase 2.2 — frame header serialisation */
	RUN(frame_hdr_write_data);
	RUN(frame_hdr_write_settings);
	RUN(frame_hdr_write_headers);

	/* Phase 1.3 — platform compat layer */
	RUN(strlcpy_basic);
	RUN(strlcpy_truncation);
	RUN(strlcpy_empty_src);
	RUN(strlcat_basic);
	RUN(strlcat_full_dst);

	/* Phase 1.4 - HPACK static and Huffman tables */
	RUN(static_table_size);
	RUN(static_table_index1);
	RUN(static_table_index2);
	RUN(static_table_index61);
	RUN(huffman_decode_empty);
	RUN(huffman_decode_www);
	RUN(huffman_encode_decode_roundtrip);
	RUN(huffman_roundtrip_long_codes);
	RUN(huffman_eos_rejected);
	RUN(huffman_invalid_padding);
	RUN(huffman_decode_truncated_long_code);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
