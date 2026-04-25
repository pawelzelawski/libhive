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
int test_hpack_hash_threshold_activation(void);
int test_hpack_hash_insert_nomem(void);
int test_hpack_hash_tombstone_probe_chain(void);
int test_hpack_hash_rebuild_on_recross(void);

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

/* Phase 3.7 — standalone API */
int test_hpack_standalone_encoder_decoder(void);

/* --- test_session.c ------------------------------------------------------- */
int test_send_control_frame_queued(void);
int test_send_partial_write(void);
int test_send_fatal_error(void);
int test_options_defaults(void);
int test_options_set_valid(void);
int test_options_set_invalid(void);
int test_session_server_new_null_alloc(void);
int test_session_client_new_null_alloc(void);
int test_session_new_custom_alloc(void);
int test_session_free_all_allocations(void);
int test_session_new_alloc_failure(void);
int test_options_set_max_concurrent(void);
int test_stream_open_lookup_close(void);
int test_stream_hash_collision(void);
int test_stream_free_stack(void);
int test_stream_compaction(void);
int test_recv_headers_opens_new_stream(void);
int test_recv_get_request_headers(void);
int test_stream_id_monotonicity(void);
int test_settings_recv_and_ack(void);
int test_settings_recv_ack(void);
int test_settings_unsolicited_ack(void);
int test_settings_invalid_window_size(void);
int test_settings_invalid_frame_size(void);
int test_settings_header_table_size_updates_encoder(void);
int test_settings_header_table_size_pending_min(void);
int test_settings_initial_window_retroactive_adjust(void);
int test_settings_initial_window_retroactive_overflow(void);
int test_server_preface_valid(void);
int test_server_preface_invalid(void);
int test_client_preface_first_frame_not_settings(void);
int test_client_preface_settings_with_ack(void);

/* --- test_flow.c ---------------------------------------------------------- */
int test_window_update_connection(void);
int test_window_update_stream(void);
int test_window_update_zero_increment_connection(void);
int test_window_update_zero_increment_stream(void);
int test_window_update_overflow(void);

int
main(void)
{
	/* Phase 4.0 — minimal send queue */
	RUN(send_control_frame_queued);
	RUN(send_partial_write);
	RUN(send_fatal_error);

	/* Phase 4.2 — options API */
	RUN(options_defaults);
	RUN(options_set_valid);
	RUN(options_set_invalid);

	/* Phase 4.3 — session creation and teardown */
	RUN(session_server_new_null_alloc);
	RUN(session_client_new_null_alloc);
	RUN(session_new_custom_alloc);
	RUN(session_free_all_allocations);
	RUN(session_new_alloc_failure);
	RUN(options_set_max_concurrent);
	RUN(stream_open_lookup_close);
	RUN(stream_hash_collision);
	RUN(stream_free_stack);
	RUN(stream_compaction);
	RUN(recv_headers_opens_new_stream);
	RUN(recv_get_request_headers);
	RUN(stream_id_monotonicity);
	RUN(settings_recv_and_ack);
	RUN(settings_recv_ack);
	RUN(settings_unsolicited_ack);
	RUN(settings_invalid_window_size);
	RUN(settings_invalid_frame_size);
	RUN(settings_header_table_size_updates_encoder);
	RUN(settings_header_table_size_pending_min);
	RUN(settings_initial_window_retroactive_adjust);
	RUN(settings_initial_window_retroactive_overflow);
	RUN(server_preface_valid);
	RUN(server_preface_invalid);
	RUN(client_preface_first_frame_not_settings);
	RUN(client_preface_settings_with_ack);

	/* Phase 5.1 — WINDOW_UPDATE receive */
	RUN(window_update_connection);
	RUN(window_update_stream);
	RUN(window_update_zero_increment_connection);
	RUN(window_update_zero_increment_stream);
	RUN(window_update_overflow);

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
	RUN(hpack_hash_threshold_activation);
	RUN(hpack_hash_insert_nomem);
	RUN(hpack_hash_tombstone_probe_chain);
	RUN(hpack_hash_rebuild_on_recross);

	/* Phase 3.2 — integer varint encode/decode */
	RUN(hpack_int_decode_1byte);
	RUN(hpack_int_decode_multibyte);
	RUN(hpack_int_decode_truncated);
	RUN(hpack_int_decode_overflow);
	RUN(hpack_int_encode_decode_roundtrip);

	/* Phase 3.3 — string encode/decode */
	RUN(hpack_string_decode_literal);
	RUN(hpack_string_decode_huffman);
	RUN(hpack_string_encode_huffman);
	RUN(hpack_string_scratch_limit);
	RUN(hpack_string_truncated);

	/* Phase 3.4 — full decoder */
	RUN(hpack_decode_rfc_c3);
	RUN(hpack_decode_rfc_c4);
	RUN(hpack_decode_rfc_c6);
	RUN(hpack_index_zero_rejected);
	RUN(hpack_index_out_of_range);
	RUN(hpack_size_update_after_header);
	RUN(hpack_size_update_exceeds_pending_max);
	RUN(hpack_bomb_size_limit);
	RUN(hpack_bomb_count_limit);
	RUN(hpack_header_callback_by_pointer);

	/* Phase 3.5 — full encoder */
	RUN(hpack_encode_decode_roundtrip_no_huff);
	RUN(hpack_encode_decode_roundtrip_huff);
	RUN(hpack_encode_pending_size_update_dual);

	/* Phase 3.7 — standalone API */
	RUN(hpack_standalone_encoder_decoder);

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
