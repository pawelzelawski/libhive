/*
 * hive_internal.h — internal shared types and forward declarations
 *
 * Not included by embedders. Internal to the library only.
 * See ARCHITECTURE.md §2 for the session struct layout.
 * See CODING_STANDARDS.md §1.3 for the _Static_assert policy.
 */

#ifndef HIVE_INTERNAL_H
#define HIVE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include "../include/hive.h"
#include "hive_frame.h"
#include "hive_hpack.h"

/* ------------------------------------------------------------------ */
/* ASan helpers (HIVE_DEBUG builds only)                               */
/* See TECH_STACK.md §7.2 and CODING_STANDARDS.md §3.5.               */
/* ------------------------------------------------------------------ */

#if defined(HIVE_DEBUG) && defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define HIVE_ASAN_POISON(ptr, size) __asan_poison_memory_region((ptr), (size))
#define HIVE_ASAN_UNPOISON(ptr, size)                                          \
	__asan_unpoison_memory_region((ptr), (size))
#else
#define HIVE_ASAN_POISON(ptr, size) ((void)0)
#define HIVE_ASAN_UNPOISON(ptr, size) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Send path compile-time cap (ARCHITECTURE.md §6.6).                 */
/* opt_max_send_iov must never exceed HIVE_SEND_IOV_MAX.              */
/* The default is 512; this cap is 1024.                              */
/* ------------------------------------------------------------------ */

#define HIVE_SEND_IOV_MAX 1024

/* ------------------------------------------------------------------ */
/* Session state enumeration (ARCHITECTURE.md §2.3).                  */
/* ------------------------------------------------------------------ */

typedef enum {
	HIVE_SESSION_OPEN = 0,
	HIVE_SESSION_GOAWAY_SENT = 1, /* we sent GOAWAY, draining in-flight */
	HIVE_SESSION_GOAWAY_RECV = 2, /* peer sent GOAWAY, no new streams   */
	HIVE_SESSION_CLOSED = 3,      /* session is dead, free it           */
} hive_session_state_t;

/* ------------------------------------------------------------------ */
/* Compile-time struct size checks                                     */
/* These are placeholders in Phase 1. Real assertions activate as     */
/* struct definitions land in Phase 2 onward.                         */
/* See CODING_STANDARDS.md §1.3.                                      */
/* ------------------------------------------------------------------ */

_Static_assert(1 == 1, "placeholder — hive_stream_t size check (Phase 2+)");
_Static_assert(1 == 1,
               "placeholder — stream_hash_entry_t size check (Phase 2+)");
_Static_assert(sizeof(hpack_entry_t) == 8,
               "hpack_entry_t size changed — update ARCHITECTURE.md §4.2");
_Static_assert(1 == 1, "placeholder — hive_settings_t size check (Phase 4+)");
/* huff_entry_t size check is active in src/hive_hpack.h next to the type. */

/* ------------------------------------------------------------------ */
/* Phase 2 minimal internal session/options structs (Task 2.5).       */
/* ------------------------------------------------------------------ */

struct hive_options {
	uint32_t placeholder;
};

struct hive_session {
	/* Region A subset */
	hive_mem_t mem;
	hive_role_t role;
	hive_callbacks_t callbacks;
	void *user_data;

	/* Region B subset (Phase 2 options used by frame parser) */
	uint32_t opt_max_frame_size;
	uint32_t opt_max_continuation_size;
	uint32_t opt_max_header_list_size;
	uint32_t opt_max_header_count;
	uint32_t opt_max_header_string_size;
	uint8_t opt_no_http_messaging;

	/* Region E subset used by Task 2.4 inbound frame-size validation */
	hive_settings_t local_settings;
	hpack_table_t enc_table;
	hpack_table_t dec_table;

	/* Region G — receive state machine fields */
	uint8_t frame_hdr_buf[9];
	uint8_t frame_hdr_count;
	uint8_t preface_count;
	uint8_t recv_state;
	frame_hdr_t cur_frame;
	uint32_t payload_remaining;
	uint32_t pad_remaining;
	uint8_t pad_length_received;
	uint8_t pad_validated;
	uint8_t fc_accounted;
	uint8_t ctrl_staging[8];
	uint8_t ctrl_staging_count;
	uint32_t reassembly_stream_id;
	uint32_t reassembly_promised_stream_id;
	uint8_t reassembly_type;
	uint8_t reassembly_end_stream;
	uint8_t reassembly_active;
	uint32_t reassembly_len;
	uint8_t *reassembly_buf;
	uint8_t *hpack_scratch_name;
	uint8_t *hpack_scratch_value;
	hive_buf_t hpack_name_handle;
	hive_buf_t hpack_value_handle;
	uint8_t priority_payload_len;

	/* GOAWAY staging values (used before callback in RECV_GOAWAY_DEBUG) */
	uint32_t goaway_last_stream_id_recv;
	uint32_t goaway_error_code_recv;

	/* Region C — connection state (minimal, Task 4.0+) */
	uint8_t session_state; /* hive_session_state_t enum value */

	/* Region J — send buffer (ARCHITECTURE.md §6.1) */
	uint8_t *send_buf;
	size_t send_buf_used;
	size_t send_buf_cap;
	struct iovec *send_iov;
	uint32_t send_iov_count;
	size_t send_partial_offset; /* bytes already sent from current batch */
	uint8_t send_partial;       /* 1 = unsent tail from previous send    */

	/* Test-visible error latch for Phase 2 parser tests */
	int last_err;
	uint32_t last_h2_err;
	uint8_t closed;
};

#endif /* HIVE_INTERNAL_H */
