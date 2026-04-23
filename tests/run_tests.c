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
int test_recv_settings_ack(void);
int test_recv_ping(void);
int test_recv_window_update(void);
int test_recv_rst_stream(void);
int test_recv_data_full(void);
int test_recv_data_padded(void);
int test_recv_headers_end_headers(void);
int test_recv_headers_priority_prefix(void);
int test_recv_priority_ignored(void);
int test_recv_unknown_type(void);
int test_recv_split_frame_header(void);
int test_recv_split_data_payload(void);
int test_recv_split_settings_param(void);
int test_recv_headers_plus_continuation(void);
int test_recv_continuation_lockout(void);
int test_recv_frame_too_large(void);
int test_recv_data_on_stream_zero(void);
int test_recv_settings_nonzero_stream(void);
int test_recv_ping_wrong_length(void);
int test_recv_rst_stream_wrong_length(void);
int test_recv_settings_bad_length_nonzero_ack(void);
int test_recv_unknown_frame_mid_stream(void);
int test_recv_split_ping_payload(void);
int test_recv_split_window_update_payload(void);
int test_recv_split_rst_stream_payload(void);
int test_recv_split_priority_payload(void);
int test_recv_split_goaway_payload(void);
int test_recv_split_push_promise_payload(void);
int test_recv_split_continuation_payload(void);
int test_recv_settings_bad_length_non_ack(void);
int test_recv_window_update_wrong_length(void);
int test_recv_priority_wrong_length(void);
int test_recv_goaway_too_short(void);
int test_recv_push_promise_wrong_length_unpadded(void);
int test_recv_push_promise_wrong_length_padded(void);

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

/* Phase 3.1 — dynamic table */
int test_hpack_table_insert_basic(void);
int test_hpack_table_evict_on_insert(void);
int test_hpack_table_evict_to_zero(void);
int test_hpack_table_rfc_size(void);
int test_hpack_table_oversized_entry(void);
int test_hpack_always_copy(void);

int
main(void)
{
	/* Phase 2.2 — frame header serialisation */
	RUN(frame_hdr_write_data);
	RUN(frame_hdr_write_settings);
	RUN(frame_hdr_write_headers);
	RUN(recv_settings_ack);
	RUN(recv_ping);
	RUN(recv_window_update);
	RUN(recv_rst_stream);
	RUN(recv_data_full);
	RUN(recv_data_padded);
	RUN(recv_headers_end_headers);
	RUN(recv_headers_priority_prefix);
	RUN(recv_priority_ignored);
	RUN(recv_unknown_type);
	RUN(recv_split_frame_header);
	RUN(recv_split_data_payload);
	RUN(recv_split_settings_param);
	RUN(recv_split_ping_payload);
	RUN(recv_split_window_update_payload);
	RUN(recv_split_rst_stream_payload);
	RUN(recv_split_priority_payload);
	RUN(recv_split_goaway_payload);
	RUN(recv_split_push_promise_payload);
	RUN(recv_split_continuation_payload);
	RUN(recv_headers_plus_continuation);
	RUN(recv_continuation_lockout);
	RUN(recv_frame_too_large);
	RUN(recv_data_on_stream_zero);
	RUN(recv_settings_nonzero_stream);
	RUN(recv_settings_bad_length_nonzero_ack);
	RUN(recv_settings_bad_length_non_ack);
	RUN(recv_ping_wrong_length);
	RUN(recv_rst_stream_wrong_length);
	RUN(recv_window_update_wrong_length);
	RUN(recv_priority_wrong_length);
	RUN(recv_goaway_too_short);
	RUN(recv_push_promise_wrong_length_unpadded);
	RUN(recv_push_promise_wrong_length_padded);
	RUN(recv_unknown_frame_mid_stream);

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

	/* Phase 3.1 — hpack_table_t: dynamic table */
	RUN(hpack_table_insert_basic);
	RUN(hpack_table_evict_on_insert);
	RUN(hpack_table_evict_to_zero);
	RUN(hpack_table_rfc_size);
	RUN(hpack_table_oversized_entry);
	RUN(hpack_always_copy);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
