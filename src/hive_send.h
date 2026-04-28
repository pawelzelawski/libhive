/*
 * hive_send.h — send queue helpers (internal)
 *
 * Declares frame_hdr_write(), send_queue_append_ctrl(), and
 * send_queue_flush_data(). These are internal to the library only.
 *
 * See ARCHITECTURE.md §6.2 for frame_hdr_write() pseudocode.
 * See ARCHITECTURE.md §6.1 for the send buffer layout.
 * See ARCHITECTURE.md §6.3 for send_queue_append_ctrl() pseudocode.
 * See ARCHITECTURE.md §6.4 for send_queue_append_headers() pseudocode.
 * See ARCHITECTURE.md §6.6 for drain / partial-send model.
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
uint8_t *frame_hdr_write(hive_session_t *s,
                         uint32_t length,
                         uint8_t type,
                         uint8_t flags,
                         uint32_t stream_id);

/*
 * send_queue_append_ctrl — serialise one control frame into send_buf and
 * append one iovec entry pointing at the complete frame.
 *
 * Writes the 9-byte frame header followed by `payload_len` bytes from
 * `payload` (which may be NULL when payload_len == 0) contiguously into
 * send_buf starting at send_buf_used.  Appends one iovec entry covering the
 * full frame (9 + payload_len bytes) and advances send_buf_used.
 *
 * Control frames: SETTINGS, SETTINGS ACK, PING, PING ACK, RST_STREAM,
 * WINDOW_UPDATE, GOAWAY.  See ARCHITECTURE.md §6.3.
 *
 * Returns HIVE_OK on success. Returns HIVE_ERR_NOMEM when send_buf has
 * insufficient space. Returns HIVE_ERR_WOULDBLOCK if an internal overflow
 * flush was partial and the existing batch must be drained before appending.
 */
int send_queue_append_ctrl(hive_session_t *s,
                           uint8_t type,
                           uint8_t flags,
                           uint32_t stream_id,
                           const uint8_t *payload,
                           uint32_t payload_len);

/*
 * send_queue_append_headers — queue one HEADERS block, splitting into
 * CONTINUATION frames when encoded HPACK bytes exceed
 * remote_settings.max_frame_size.
 *
 * The encoded HPACK payload is written contiguously at encode_start.
 * In split mode, CONTINUATION frame headers are written after the payload and
 * iovecs are emitted in wire order (header, chunk, header, chunk, ...)
 * without shifting payload bytes.
 *
 * end_stream controls END_STREAM on the first HEADERS frame.
 * END_HEADERS is set on the last frame in the sequence.
 *
 * Returns HIVE_OK on success, HIVE_ERR_INVALID_ARG for bad arguments,
 * HIVE_ERR_NOMEM when send buffer/iovec capacity is insufficient, or an
 * encoder error propagated from hpack_encode_block().
 */
int send_queue_append_headers(hive_session_t *s,
                              uint32_t stream_id,
                              const hive_nv_t *nva,
                              size_t nvlen,
                              uint8_t end_stream);

/*
 * send_queue_append_push_promise — queue one PUSH_PROMISE frame.
 *
 * Payload layout: 4-byte promised stream ID (R bit clear) followed by an
 * HPACK-encoded request header block. This helper emits a single
 * PUSH_PROMISE+END_HEADERS frame and returns HIVE_ERR_NOMEM if the encoded
 * payload would exceed remote_settings.max_frame_size.
 */
int send_queue_append_push_promise(hive_session_t *s,
                                   uint32_t stream_id,
                                   uint32_t promised_stream_id,
                                   const hive_nv_t *nva,
                                   size_t nvlen);

/*
 * send_queue_flush_data — drive pending data_source streams into the send
 * queue within flow control limits.
 *
 * Phase 4 stub: does nothing.  Phase 6 provides the real implementation.
 * Called by hive_session_send() before building the effective iovec.
 * See ARCHITECTURE.md §6.5 and §6.6.
 */
void send_queue_flush_data(hive_session_t *s);

#endif /* HIVE_SEND_H */
