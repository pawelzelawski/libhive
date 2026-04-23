/*
 * hive.c — session lifecycle and public API entry points
 *
 * See ARCHITECTURE.md §1 and §9 for full documentation.
 * See CODING_STANDARDS.md §2.1 — this is the ONLY file in src/ that may
 * call malloc/free/calloc/realloc directly (in the NULL-allocator shim).
 */

#include <stdlib.h>
#include <string.h>

#include "../include/hive.h"
#include "hive_frame.h"
#include "hive_internal.h"

/*
 * NULL-allocator shim.
 * This is the ONLY permitted direct use of malloc/free/calloc/realloc
 * in the library source. All other src/ files use s->mem.* exclusively.
 * See CODING_STANDARDS.md §2.1.
 */
static void *
null_alloc_malloc(size_t size, void *ctx)
{
	(void)ctx;
	return malloc(size);
}

static void
null_alloc_free(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);
}

static void *
null_alloc_calloc(size_t nmemb, size_t size, void *ctx)
{
	(void)ctx;
	return calloc(nmemb, size);
}

static void *
null_alloc_realloc(void *ptr, size_t size, void *ctx)
{
	(void)ctx;
	return realloc(ptr, size);
}

static const hive_mem_t null_allocator = {
    null_alloc_malloc,
    null_alloc_free,
    null_alloc_calloc,
    null_alloc_realloc,
    NULL,
};

struct hive_hpack_encoder {
	hive_mem_t mem;
	hpack_table_t table;
};

struct hive_hpack_decoder {
	hive_mem_t mem;
	hpack_table_t table;
	uint8_t *scratch_name;
	uint8_t *scratch_value;
	size_t scratch_name_cap;
	size_t scratch_value_cap;
};

static int
standalone_ensure_scratch(hive_hpack_decoder_t *dec,
                          uint8_t **scratch,
                          size_t *scratch_cap,
                          size_t need)
{
	uint8_t *p;
	size_t cap;

	if (need <= *scratch_cap)
		return HIVE_OK;

	cap = (*scratch_cap == 0) ? 256u : *scratch_cap;
	while (cap < need) {
		if (cap > (SIZE_MAX / 2u)) {
			cap = need;
			break;
		}
		cap <<= 1;
	}

	p = dec->mem.realloc(*scratch, cap, dec->mem.ctx);
	if (p == NULL)
		return HIVE_ERR_NOMEM;

	*scratch = p;
	*scratch_cap = cap;
	return HIVE_OK;
}

static int
standalone_decode_string(hive_hpack_decoder_t *dec,
                         const uint8_t *src,
                         size_t src_len,
                         int is_name,
                         hive_buf_t *out,
                         size_t *consumed)
{
	uint8_t **scratch;
	size_t *scratch_cap;
	uint32_t slen;
	size_t int_consumed;
	size_t need;
	int ret;

	slen = hpack_decode_int(src, src_len, 7, &int_consumed);
	if (slen == HPACK_INT_OVERFLOW)
		return HIVE_ERR_COMPRESSION;
	if ((size_t)slen > src_len - int_consumed)
		return HIVE_ERR_COMPRESSION;

	if ((size_t)slen > (SIZE_MAX - 32u) / 2u)
		return HIVE_ERR_NOMEM;
	need = (size_t)slen * 2u + 32u;

	if (is_name) {
		scratch = &dec->scratch_name;
		scratch_cap = &dec->scratch_name_cap;
	} else {
		scratch = &dec->scratch_value;
		scratch_cap = &dec->scratch_value_cap;
	}

	ret = standalone_ensure_scratch(dec, scratch, scratch_cap, need);
	if (ret != HIVE_OK)
		return ret;

	ret = hpack_decode_string(
	    src, src_len, *scratch, *scratch_cap, out, consumed);
	if (ret != HIVE_OK)
		return HIVE_ERR_COMPRESSION;
	return HIVE_OK;
}

static int
standalone_index_to_header(const hive_hpack_decoder_t *dec,
                           uint32_t index,
                           hive_nv_t *nv)
{
	const hpack_entry_t *e;

	if (index == 0)
		return HIVE_ERR_COMPRESSION;

	if (index <= HPACK_STATIC_TABLE_SIZE) {
		nv->name = hpack_static_table[index - 1u].name;
		nv->name_len = hpack_static_table[index - 1u].name_len;
		nv->value = hpack_static_table[index - 1u].value;
		nv->value_len = hpack_static_table[index - 1u].value_len;
		nv->flags = 0;
		return HIVE_OK;
	}

	index -= HPACK_STATIC_TABLE_SIZE + 1u;
	if (index >= dec->table.count)
		return HIVE_ERR_COMPRESSION;

	e = hpack_table_get(&dec->table, index);
	nv->name = HPACK_ENTRY_NAME(e);
	nv->name_len = e->name_len;
	nv->value = HPACK_ENTRY_VALUE(e);
	nv->value_len = e->value_len;
	nv->flags = 0;
	return HIVE_OK;
}

static int
standalone_index_to_name(const hive_hpack_decoder_t *dec,
                         uint32_t index,
                         hive_buf_t *name)
{
	hive_nv_t nv;
	int ret;

	ret = standalone_index_to_header(dec, index, &nv);
	if (ret != HIVE_OK)
		return ret;

	name->data = nv.name;
	name->len = nv.name_len;
	name->flags = HIVE_BUF_VALID;
	return HIVE_OK;
}

/*
 * Stub implementations — Phase 1 skeleton only.
 * Full implementations land in Phase 4 onward.
 */

hive_options_t *
hive_options_new(void)
{
	return NULL;
}

void
hive_options_free(hive_options_t *opt)
{
	(void)opt;
}

hive_session_t *
hive_session_server_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	(void)null_allocator;
	return NULL;
}

hive_session_t *
hive_session_client_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	return NULL;
}

hive_session_t *
hive_session_server_upgrade(const hive_mem_t *mem,
                            const hive_options_t *opt,
                            const hive_callbacks_t *callbacks,
                            void *user_data,
                            const char *http2_settings_b64,
                            uint32_t upgraded_stream_id)
{
	(void)mem;
	(void)opt;
	(void)callbacks;
	(void)user_data;
	(void)http2_settings_b64;
	(void)upgraded_stream_id;
	return NULL;
}

void
hive_session_free(hive_session_t *session)
{
	(void)session;
}

ssize_t
hive_session_recv(hive_session_t *session, const uint8_t *data, size_t len)
{
	if (session == NULL) {
		return -1;
	}
	return frame_recv_process(session, data, len);
}

int
hive_session_send(hive_session_t *session)
{
	(void)session;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_session_want_read(hive_session_t *session)
{
	(void)session;
	return 0;
}

int
hive_session_want_write(hive_session_t *session)
{
	(void)session;
	return 0;
}

int
hive_submit_response(hive_session_t *session,
                     uint32_t stream_id,
                     const hive_nv_t *nva,
                     size_t nvlen,
                     hive_data_source_t *data_source)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	(void)data_source;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_trailers(hive_session_t *session,
                     uint32_t stream_id,
                     const hive_nv_t *nva,
                     size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_interim_response(hive_session_t *session,
                             uint32_t stream_id,
                             const hive_nv_t *nva,
                             size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_push_promise(hive_session_t *session,
                         uint32_t stream_id,
                         const hive_nv_t *nva,
                         size_t nvlen,
                         uint32_t *promised_stream_id_out)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	(void)promised_stream_id_out;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_request(hive_session_t *session,
                    const hive_nv_t *nva,
                    size_t nvlen,
                    hive_data_source_t *data_source,
                    uint32_t *stream_id_out)
{
	(void)session;
	(void)nva;
	(void)nvlen;
	(void)data_source;
	(void)stream_id_out;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_rst_stream(hive_session_t *session,
                       uint32_t stream_id,
                       uint32_t error_code)
{
	(void)session;
	(void)stream_id;
	(void)error_code;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_goaway_prepare(hive_session_t *session)
{
	(void)session;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_goaway_final(hive_session_t *session,
                         uint32_t error_code,
                         const uint8_t *debug_data,
                         size_t debug_len)
{
	(void)session;
	(void)error_code;
	(void)debug_data;
	(void)debug_len;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_ping(hive_session_t *session, const uint8_t opaque[8])
{
	(void)session;
	(void)opaque;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_submit_ping_ack(hive_session_t *session, const uint8_t opaque[8])
{
	(void)session;
	(void)opaque;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_session_feed_upgrade_headers(hive_session_t *session,
                                  uint32_t stream_id,
                                  const hive_nv_t *nva,
                                  size_t nvlen)
{
	(void)session;
	(void)stream_id;
	(void)nva;
	(void)nvlen;
	return HIVE_ERR_SESSION_CLOSED;
}

int
hive_buf_retain(hive_session_t *session, hive_buf_t *buf)
{
	(void)session;
	(void)buf;
	return HIVE_ERR_SESSION_CLOSED;
}

void
hive_buf_free(hive_session_t *session, hive_buf_t *buf)
{
	(void)session;
	(void)buf;
}

int32_t
hive_session_get_remote_window_size(hive_session_t *session)
{
	(void)session;
	return 0;
}

hive_settings_t
hive_session_get_local_settings(hive_session_t *session)
{
	hive_settings_t s = {0, 0, 0, 0, 0, 0};
	(void)session;
	return s;
}

hive_settings_t
hive_session_get_remote_settings(hive_session_t *session)
{
	hive_settings_t s = {0, 0, 0, 0, 0, 0};
	(void)session;
	return s;
}

int
hive_stream_get_state(hive_session_t *session, uint32_t stream_id)
{
	(void)session;
	(void)stream_id;
	return -1;
}

int
hive_stream_set_user_data(hive_session_t *session,
                          uint32_t stream_id,
                          void *user_data)
{
	(void)session;
	(void)stream_id;
	(void)user_data;
	return HIVE_ERR_SESSION_CLOSED;
}

void *
hive_stream_get_user_data(hive_session_t *session, uint32_t stream_id)
{
	(void)session;
	(void)stream_id;
	return NULL;
}

int
hive_options_set_header_table_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_enable_push(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_concurrent_streams(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_initial_window_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_frame_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_header_list_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_continuation_size(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_max_send_iov(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_settings_flood_limit(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_rst_flood_threshold(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_options_set_rst_flood_window_secs(hive_options_t *opt, uint32_t v)
{
	(void)opt;
	(void)v;
	return HIVE_ERR_INVALID_ARG;
}

int
hive_hpack_encoder_new(hive_hpack_encoder_t **enc, size_t max_table_size)
{
	hive_hpack_encoder_t *p;
	int ret;

	if (enc == NULL || max_table_size > UINT32_MAX)
		return HIVE_ERR_INVALID_ARG;
	*enc = NULL;

	p = null_allocator.calloc(1, sizeof(*p), null_allocator.ctx);
	if (p == NULL)
		return HIVE_ERR_NOMEM;
	p->mem = null_allocator;

	ret = hpack_table_init(&p->table, &p->mem, (uint32_t)max_table_size);
	if (ret != HIVE_OK) {
		null_allocator.free(p, null_allocator.ctx);
		return ret;
	}

	*enc = p;
	return HIVE_OK;
}

void
hive_hpack_encoder_free(hive_hpack_encoder_t *enc)
{
	if (enc == NULL)
		return;
	hpack_table_free(&enc->table, &enc->mem);
	enc->mem.free(enc, enc->mem.ctx);
}

int
hive_hpack_encode(hive_hpack_encoder_t *enc,
                  const hive_nv_t *nva,
                  size_t nvlen,
                  uint8_t *out,
                  size_t *out_len)
{
	size_t written;
	int ret;

	if (enc == NULL || out == NULL || out_len == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (nvlen > 0 && nva == NULL)
		return HIVE_ERR_INVALID_ARG;

	ret = hpack_encode_block(
	    &enc->table, &enc->mem, nva, nvlen, out, *out_len, &written);
	if (ret != HIVE_OK)
		return ret;
	*out_len = written;
	return HIVE_OK;
}

int
hive_hpack_decoder_new(hive_hpack_decoder_t **dec, size_t max_table_size)
{
	hive_hpack_decoder_t *p;
	int ret;

	if (dec == NULL || max_table_size > UINT32_MAX)
		return HIVE_ERR_INVALID_ARG;
	*dec = NULL;

	p = null_allocator.calloc(1, sizeof(*p), null_allocator.ctx);
	if (p == NULL)
		return HIVE_ERR_NOMEM;
	p->mem = null_allocator;

	ret = hpack_table_init(&p->table, &p->mem, (uint32_t)max_table_size);
	if (ret != HIVE_OK) {
		null_allocator.free(p, null_allocator.ctx);
		return ret;
	}

	*dec = p;
	return HIVE_OK;
}

void
hive_hpack_decoder_free(hive_hpack_decoder_t *dec)
{
	if (dec == NULL)
		return;
	dec->mem.free(dec->scratch_name, dec->mem.ctx);
	dec->mem.free(dec->scratch_value, dec->mem.ctx);
	hpack_table_free(&dec->table, &dec->mem);
	dec->mem.free(dec, dec->mem.ctx);
}

int
hive_hpack_decode(hive_hpack_decoder_t *dec,
                  const uint8_t *in,
                  size_t in_len,
                  size_t *consumed,
                  hive_nv_t *nv_out)
{
	size_t pos;

	if (dec == NULL || consumed == NULL || nv_out == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (in_len > 0 && in == NULL)
		return HIVE_ERR_INVALID_ARG;

	*consumed = 0;
	if (in_len == 0)
		return HIVE_HPACK_DECODE_DONE;

	pos = 0;
	while (pos < in_len) {
		hive_buf_t name_buf;
		hive_buf_t value_buf;
		size_t n;
		size_t v;
		uint32_t idx;
		uint8_t hdr_flags;
		int ret;

		memset(&name_buf, 0, sizeof(name_buf));
		memset(&value_buf, 0, sizeof(value_buf));
		hdr_flags = 0;

		if (in[pos] & 0x80u) {
			idx = hpack_decode_int(in + pos, in_len - pos, 7, &n);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += n;

			ret = standalone_index_to_header(dec, idx, nv_out);
			if (ret != HIVE_OK)
				return HIVE_ERR_COMPRESSION;
			*consumed = pos;
			return HIVE_HPACK_DECODE_EMIT;
		} else if (in[pos] & 0x40u) {
			idx = hpack_decode_int(in + pos, in_len - pos, 6, &n);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += n;

			if (idx == 0) {
				ret = standalone_decode_string(dec,
				                               in + pos,
				                               in_len - pos,
				                               1,
				                               &name_buf,
				                               &n);
				if (ret != HIVE_OK)
					return ret;
				pos += n;
			} else {
				ret = standalone_index_to_name(
				    dec, idx, &name_buf);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
			}

			ret = standalone_decode_string(
			    dec, in + pos, in_len - pos, 0, &value_buf, &v);
			if (ret != HIVE_OK)
				return ret;
			pos += v;

			ret = hpack_table_insert(&dec->table,
			                         &dec->mem,
			                         name_buf.data,
			                         (uint32_t)name_buf.len,
			                         value_buf.data,
			                         (uint32_t)value_buf.len);
			if (ret != HIVE_OK)
				return ret;

			if (dec->table.count > 0) {
				const hpack_entry_t *e;

				e = hpack_table_get(&dec->table, 0);
				nv_out->name = HPACK_ENTRY_NAME(e);
				nv_out->name_len = e->name_len;
				nv_out->value = HPACK_ENTRY_VALUE(e);
				nv_out->value_len = e->value_len;
			} else {
				nv_out->name = name_buf.data;
				nv_out->name_len = name_buf.len;
				nv_out->value = value_buf.data;
				nv_out->value_len = value_buf.len;
			}
			nv_out->flags = 0;
			*consumed = pos;
			return HIVE_HPACK_DECODE_EMIT;
		} else if (in[pos] & 0x20u) {
			idx = hpack_decode_int(in + pos, in_len - pos, 5, &n);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			if (idx > dec->table.pending_max)
				return HIVE_ERR_COMPRESSION;
			hpack_table_evict_to(&dec->table, &dec->mem, idx);
			dec->table.max_size = idx;
			pos += n;
			continue;
		} else {
			if ((in[pos] & 0xf0u) == 0x10u)
				hdr_flags = HIVE_NV_FLAG_NO_INDEX;

			idx = hpack_decode_int(in + pos, in_len - pos, 4, &n);
			if (idx == HPACK_INT_OVERFLOW)
				return HIVE_ERR_COMPRESSION;
			pos += n;

			if (idx == 0) {
				ret = standalone_decode_string(dec,
				                               in + pos,
				                               in_len - pos,
				                               1,
				                               &name_buf,
				                               &n);
				if (ret != HIVE_OK)
					return ret;
				pos += n;
			} else {
				ret = standalone_index_to_name(
				    dec, idx, &name_buf);
				if (ret != HIVE_OK)
					return HIVE_ERR_COMPRESSION;
			}

			ret = standalone_decode_string(
			    dec, in + pos, in_len - pos, 0, &value_buf, &v);
			if (ret != HIVE_OK)
				return ret;
			pos += v;

			nv_out->name = name_buf.data;
			nv_out->name_len = name_buf.len;
			nv_out->value = value_buf.data;
			nv_out->value_len = value_buf.len;
			nv_out->flags = hdr_flags;
			*consumed = pos;
			return HIVE_HPACK_DECODE_EMIT;
		}
	}

	*consumed = pos;
	return HIVE_HPACK_DECODE_DONE;
}
