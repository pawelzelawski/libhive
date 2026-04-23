/*
 * hive_send.h — send queue helpers (internal)
 *
 * Declares frame_hdr_write(), a session-coupled thin wrapper around
 * frame_hdr_write_at() that writes directly into send_buf and advances
 * send_buf_used.
 *
 * See ARCHITECTURE.md §6.2 for the canonical frame_hdr_write() pseudocode.
 * See ARCHITECTURE.md §6.1 for the send buffer layout.
 *
 * Note: hive_send.c is wired into the Makefile in Task 2.5 once the
 * hive_session_t struct definition is in place.
 *
 * Not included by embedders — internal to the library only.
 */

#ifndef HIVE_SEND_H
#define HIVE_SEND_H

#include <stdint.h>

#include "../include/hive.h"
#include "hive_frame_bare.h"

/*
 * frame_hdr_write — write a 9-byte frame header at send_buf + send_buf_used
 * and advance send_buf_used by 9.
 *
 * Returns a pointer to the first byte of the written header so callers can
 * back-patch the frame header after encoding the payload (HEADERS split path,
 * DATA path).  See ARCHITECTURE.md §6.2.
 *
 * The caller must ensure send_buf_used + 9 <= send_buf_cap before calling.
 */
uint8_t *frame_hdr_write(hive_session_t *s, uint32_t length,
    uint8_t type, uint8_t flags, uint32_t stream_id);

#endif /* HIVE_SEND_H */

