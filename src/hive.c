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
#include "hive_send.h"

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

static const uint8_t client_preface_magic[24] = {
    'P', 'R', 'I',  ' ',  '*',  ' ',  'H', 'T', 'T',  'P',  '/',  '2',
    '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'};

static const hive_options_t default_options = {
    4096u,
    1u,
    100u,
    65535u,
    16384u,
    65536u,
    100u,
    65536u,
    3u,
    100u,
    10u,
    512u,
    8192u,
    0u,
    0u,
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

/*
 * Options API — ARCHITECTURE.md §9.5.
 *
 * hive_options_t uses system malloc (calloc/free) directly because options
 * are created once at startup, not per-connection.  This is the only place
 * in the library where system allocator calls are permitted outside the
 * NULL-allocator shim above.  See CODING_STANDARDS.md §2.1.
 */

hive_options_t *
hive_options_new(void)
{
	hive_options_t *opt;

	opt = calloc(1u, sizeof(*opt));
	if (opt == NULL)
		return NULL;

	/* Apply defaults per ARCHITECTURE.md §9.5. */
	opt->opt_header_table_size = 4096u;
	opt->opt_enable_push = 1u;
	opt->opt_max_concurrent_streams = 100u;
	opt->opt_initial_window_size = 65535u;
	opt->opt_max_frame_size = 16384u;
	opt->opt_max_header_list_size = 65536u;
	opt->opt_max_header_count = 100u;
	opt->opt_max_continuation_size = 65536u;
	opt->opt_max_settings_pending = 3u;
	opt->opt_rst_flood_threshold = 100u;
	opt->opt_rst_flood_window_secs = 10u;
	opt->opt_max_send_iov = 512u;
	opt->opt_max_header_string_size = 8192u;
	opt->opt_no_http_messaging = 0u;
	opt->opt_no_auto_ping_ack = 0u;

	return opt;
}

void
hive_options_free(hive_options_t *opt)
{
	free(opt);
}

int
hive_options_set_header_table_size(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v > 65536u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_header_table_size = v;
	return HIVE_OK;
}

int
hive_options_set_enable_push(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v > 1u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_enable_push = v;
	return HIVE_OK;
}

int
hive_options_set_max_concurrent_streams(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 65535u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_concurrent_streams = v;
	return HIVE_OK;
}

int
hive_options_set_initial_window_size(hive_options_t *opt, uint32_t v)
{
	/* Range: 1 – 2^31-1 = 2147483647. */
	if (opt == NULL || v < 1u || v > 2147483647u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_initial_window_size = v;
	return HIVE_OK;
}

int
hive_options_set_max_frame_size(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 16384u || v > 16777215u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_frame_size = v;
	return HIVE_OK;
}

int
hive_options_set_max_header_list_size(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 16777215u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_header_list_size = v;
	return HIVE_OK;
}

int
hive_options_set_max_header_count(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 65535u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_header_count = v;
	return HIVE_OK;
}

int
hive_options_set_max_continuation_size(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 16384u || v > 16777215u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_continuation_size = v;
	return HIVE_OK;
}

int
hive_options_set_max_settings_pending(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 255u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_settings_pending = v;
	return HIVE_OK;
}

int
hive_options_set_rst_stream_flood_threshold(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 65535u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_rst_flood_threshold = v;
	return HIVE_OK;
}

int
hive_options_set_rst_stream_flood_window_secs(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 1u || v > 3600u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_rst_flood_window_secs = v;
	return HIVE_OK;
}

int
hive_options_set_max_send_iov(hive_options_t *opt, uint32_t v)
{
	/*
	 * Upper bound capped at HIVE_SEND_IOV_MAX (compile-time constant 1024).
	 * ARCHITECTURE.md §9.5: exceeding this returns HIVE_ERR_INVALID_ARG.
	 */
	if (opt == NULL || v < 64u || v > HIVE_SEND_IOV_MAX)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_send_iov = v;
	return HIVE_OK;
}

int
hive_options_set_max_header_string_size(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v < 256u || v > 65536u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_max_header_string_size = v;
	return HIVE_OK;
}

int
hive_options_set_no_http_messaging(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v > 1u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_no_http_messaging = (uint8_t)v;
	return HIVE_OK;
}

int
hive_options_set_no_auto_ping_ack(hive_options_t *opt, uint32_t v)
{
	if (opt == NULL || v > 1u)
		return HIVE_ERR_INVALID_ARG;
	opt->opt_no_auto_ping_ack = (uint8_t)v;
	return HIVE_OK;
}

static uint32_t
next_pow2_u32(uint32_t v)
{
	uint32_t p;

	if (v <= 1u)
		return 1u;

	p = 1u;
	while (p < v)
		p <<= 1;
	return p;
}

static uint32_t
hpack_ring_cap_for(uint32_t max_size)
{
	uint32_t cap;

	cap = max_size / 32u;
	if (cap < 4u)
		cap = 4u;
	return next_pow2_u32(cap);
}

static size_t
session_send_buf_cap(uint32_t opt_max_continuation_size)
{
	uint32_t frames;

	frames = (opt_max_continuation_size + 16384u - 1u) / 16384u;
	return (size_t)opt_max_continuation_size + (size_t)frames * 9u + 2048u;
}

static int
stream_is_peer_initiated(const hive_session_t *s, uint32_t stream_id)
{
	if (s->role == HIVE_ROLE_SERVER)
		return (stream_id & 1u) != 0u;
	return (stream_id & 1u) == 0u;
}

uint32_t
stream_hash_fn(uint32_t stream_id, uint32_t hash_mask)
{
	uint32_t bits;

	bits = (uint32_t)__builtin_popcount(hash_mask);
	return (stream_id * 2654435761u) >> (32u - bits);
}

static uint32_t
stream_free_pop(hive_session_t *s)
{
	if (s->stream_free_top == 0)
		return STREAM_HASH_EMPTY;
	s->stream_free_top--;
	return s->stream_free_stack[s->stream_free_top];
}

static void
stream_free_push(hive_session_t *s, uint32_t slot_index)
{
	s->stream_free_stack[s->stream_free_top] = slot_index;
	s->stream_free_top++;
}

static void
stream_hash_compact(hive_session_t *s)
{
	uint32_t i;
	uint32_t hash_table_size;

	hash_table_size = s->stream_hash_mask + 1u;
	for (i = 0; i < hash_table_size; i++) {
		s->stream_hash[i].stream_id = STREAM_HASH_EMPTY;
		s->stream_hash[i].slot_index = 0u;
	}

	for (i = 0; i < s->opt_max_concurrent_streams; i++) {
		const hive_stream_t *st;
		uint32_t h;

		st = &s->stream_slots[i];
		if (st->stream_id == 0)
			continue;

		h = stream_hash_fn(st->stream_id, s->stream_hash_mask);
		for (;;) {
			if (s->stream_hash[h].stream_id == STREAM_HASH_EMPTY) {
				s->stream_hash[h].stream_id = st->stream_id;
				s->stream_hash[h].slot_index = i;
				break;
			}
			h = (h + 1u) & s->stream_hash_mask;
		}
	}

	s->tombstone_count = 0u;
	s->closes_since_compact = 0u;
}

hive_stream_t *
stream_lookup(hive_session_t *s, uint32_t stream_id)
{
	uint32_t h;

	if (s == NULL || s->stream_hash == NULL || s->stream_slots == NULL)
		return NULL;

	h = stream_hash_fn(stream_id, s->stream_hash_mask);
	for (;;) {
		uint32_t id;

		id = s->stream_hash[h].stream_id;
		if (id == stream_id)
			return &s->stream_slots[s->stream_hash[h].slot_index];
		if (id == STREAM_HASH_EMPTY)
			return NULL;
		h = (h + 1u) & s->stream_hash_mask;
	}
}

int
stream_open(hive_session_t *s, uint32_t stream_id, uint8_t state)
{
	hive_stream_t *st;
	uint32_t slot_index;
	uint32_t h;
	uint32_t insert_h;

	if (s == NULL || s->stream_hash == NULL || s->stream_slots == NULL ||
	    s->stream_free_stack == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (stream_lookup(s, stream_id) != NULL)
		return HIVE_ERR_PROTOCOL;

	slot_index = stream_free_pop(s);
	if (slot_index == STREAM_HASH_EMPTY)
		return HIVE_ERR_REFUSED_STREAM;

	st = &s->stream_slots[slot_index];
	memset(st, 0, sizeof(*st));
	st->stream_id = stream_id;
	st->state = state;
	st->send_window = (int32_t)s->remote_settings.initial_window_size;
	st->recv_window = (int32_t)s->local_settings.initial_window_size;
	st->content_length_expected = -1;

	h = stream_hash_fn(stream_id, s->stream_hash_mask);
	insert_h = STREAM_HASH_EMPTY;
	for (;;) {
		uint32_t id;

		id = s->stream_hash[h].stream_id;
		if (id == STREAM_HASH_EMPTY) {
			if (insert_h == STREAM_HASH_EMPTY)
				insert_h = h;
			break;
		}
		if (id == STREAM_HASH_TOMBSTONE &&
		    insert_h == STREAM_HASH_EMPTY)
			insert_h = h;
		h = (h + 1u) & s->stream_hash_mask;
	}

	if (insert_h == STREAM_HASH_EMPTY) {
		stream_free_push(s, slot_index);
		memset(st, 0, sizeof(*st));
		return HIVE_ERR_REFUSED_STREAM;
	}

	if (s->stream_hash[insert_h].stream_id == STREAM_HASH_TOMBSTONE &&
	    s->tombstone_count > 0u)
		s->tombstone_count--;

	s->stream_hash[insert_h].stream_id = stream_id;
	s->stream_hash[insert_h].slot_index = slot_index;
	s->stream_open_count++;
	if (stream_is_peer_initiated(s, stream_id))
		s->peer_stream_open_count++;

	return HIVE_OK;
}

void
stream_close(hive_session_t *s, hive_stream_t *stream)
{
	uint32_t stream_id;
	uint32_t h;
	uint32_t slot_index;
	uint32_t hash_table_size;

	if (s == NULL || stream == NULL || stream->stream_id == 0 ||
	    s->stream_hash == NULL || s->stream_slots == NULL ||
	    s->stream_free_stack == NULL)
		return;

	stream_id = stream->stream_id;
	h = stream_hash_fn(stream_id, s->stream_hash_mask);
	for (;;) {
		if (s->stream_hash[h].stream_id == stream_id)
			break;
		if (s->stream_hash[h].stream_id == STREAM_HASH_EMPTY)
			return;
		h = (h + 1u) & s->stream_hash_mask;
	}

	slot_index = s->stream_hash[h].slot_index;
	s->stream_hash[h].stream_id = STREAM_HASH_TOMBSTONE;
	s->stream_hash[h].slot_index = 0u;
	memset(&s->stream_slots[slot_index],
	       0,
	       sizeof(s->stream_slots[slot_index]));
	stream_free_push(s, slot_index);

	if (s->stream_open_count > 0u)
		s->stream_open_count--;
	if (stream_is_peer_initiated(s, stream_id) &&
	    s->peer_stream_open_count > 0u)
		s->peer_stream_open_count--;

	s->tombstone_count++;
	s->closes_since_compact++;

	hash_table_size = s->stream_hash_mask + 1u;
	if (s->tombstone_count > hash_table_size / 4u &&
	    s->closes_since_compact >= 64u)
		stream_hash_compact(s);
}

static void
session_prealloc_free(hive_session_t *s)
{
	if (s->pending_settings != NULL) {
		s->mem.free(s->pending_settings, s->mem.ctx);
		s->pending_settings = NULL;
	}
	if (s->dec_table.ring != NULL)
		hpack_table_free(&s->dec_table, &s->mem);
	if (s->enc_table.ring != NULL)
		hpack_table_free(&s->enc_table, &s->mem);
	if (s->hpack_scratch_value != NULL) {
		s->mem.free(s->hpack_scratch_value, s->mem.ctx);
		s->hpack_scratch_value = NULL;
	}
	if (s->hpack_scratch_name != NULL) {
		s->mem.free(s->hpack_scratch_name, s->mem.ctx);
		s->hpack_scratch_name = NULL;
	}
	if (s->reassembly_buf != NULL) {
		s->mem.free(s->reassembly_buf, s->mem.ctx);
		s->reassembly_buf = NULL;
	}
	if (s->send_buf != NULL) {
		s->mem.free(s->send_buf, s->mem.ctx);
		s->send_buf = NULL;
	}
	if (s->send_iov != NULL) {
		s->mem.free(s->send_iov, s->mem.ctx);
		s->send_iov = NULL;
	}
	if (s->stream_free_stack != NULL) {
		s->mem.free(s->stream_free_stack, s->mem.ctx);
		s->stream_free_stack = NULL;
	}
	if (s->stream_slots != NULL) {
		s->mem.free(s->stream_slots, s->mem.ctx);
		s->stream_slots = NULL;
	}
	if (s->stream_hash != NULL) {
		s->mem.free(s->stream_hash, s->mem.ctx);
		s->stream_hash = NULL;
	}
}

static int
session_prealloc(hive_session_t *s)
{
	uint32_t hash_table_size;
	uint32_t i;
	int ret;

	hash_table_size = next_pow2_u32(s->opt_max_concurrent_streams * 2u);
	s->stream_hash_mask = hash_table_size - 1u;
	s->send_buf_cap = session_send_buf_cap(s->opt_max_continuation_size);

	ret = HIVE_ERR_NOMEM;

	s->stream_hash =
	    s->mem.calloc(hash_table_size, sizeof(*s->stream_hash), s->mem.ctx);
	if (s->stream_hash == NULL)
		goto cleanup;

	s->stream_slots = s->mem.calloc(s->opt_max_concurrent_streams,
	                                sizeof(*s->stream_slots),
	                                s->mem.ctx);
	if (s->stream_slots == NULL)
		goto cleanup;

	s->stream_free_stack =
	    s->mem.malloc((size_t)s->opt_max_concurrent_streams *
	                      sizeof(*s->stream_free_stack),
	                  s->mem.ctx);
	if (s->stream_free_stack == NULL)
		goto cleanup;

	s->send_iov = s->mem.calloc(
	    s->opt_max_send_iov, sizeof(*s->send_iov), s->mem.ctx);
	if (s->send_iov == NULL)
		goto cleanup;

	s->send_buf = s->mem.malloc(s->send_buf_cap, s->mem.ctx);
	if (s->send_buf == NULL)
		goto cleanup;

	s->reassembly_buf =
	    s->mem.malloc(s->opt_max_continuation_size, s->mem.ctx);
	if (s->reassembly_buf == NULL)
		goto cleanup;

	s->hpack_scratch_name =
	    s->mem.malloc(s->opt_max_header_string_size, s->mem.ctx);
	if (s->hpack_scratch_name == NULL)
		goto cleanup;

	s->hpack_scratch_value =
	    s->mem.malloc(s->opt_max_header_string_size, s->mem.ctx);
	if (s->hpack_scratch_value == NULL)
		goto cleanup;

	s->enc_table.ring = (hpack_entry_t **)s->mem.calloc(
	    hpack_ring_cap_for(s->remote_settings.header_table_size),
	    sizeof(hpack_entry_t *),
	    s->mem.ctx);
	if (s->enc_table.ring == NULL)
		goto cleanup;
	s->enc_table.ring_cap =
	    hpack_ring_cap_for(s->remote_settings.header_table_size);
	s->enc_table.max_size = s->remote_settings.header_table_size;
	s->enc_table.pending_max = s->remote_settings.header_table_size;
	s->enc_table.pending_min = s->remote_settings.header_table_size;

	s->dec_table.ring = (hpack_entry_t **)s->mem.calloc(
	    hpack_ring_cap_for(s->local_settings.header_table_size),
	    sizeof(hpack_entry_t *),
	    s->mem.ctx);
	if (s->dec_table.ring == NULL)
		goto cleanup;
	s->dec_table.ring_cap =
	    hpack_ring_cap_for(s->local_settings.header_table_size);
	s->dec_table.max_size = s->local_settings.header_table_size;
	s->dec_table.pending_max = s->local_settings.header_table_size;
	s->dec_table.pending_min = s->local_settings.header_table_size;

	s->pending_settings = s->mem.calloc(s->opt_max_settings_pending,
	                                    sizeof(*s->pending_settings),
	                                    s->mem.ctx);
	if (s->pending_settings == NULL)
		goto cleanup;

	for (i = 0; i < hash_table_size; i++)
		s->stream_hash[i].stream_id = STREAM_HASH_EMPTY;
	for (i = 0; i < s->opt_max_concurrent_streams; i++)
		s->stream_free_stack[i] = i;
	s->stream_free_top = s->opt_max_concurrent_streams;

	ret = HIVE_OK;

cleanup:
	if (ret != HIVE_OK)
		session_prealloc_free(s);
	return ret;
}

static void
settings_param_write(uint8_t *dst, uint16_t id, uint32_t value)
{
	dst[0] = (uint8_t)((id >> 8) & 0xffu);
	dst[1] = (uint8_t)(id & 0xffu);
	dst[2] = (uint8_t)((value >> 24) & 0xffu);
	dst[3] = (uint8_t)((value >> 16) & 0xffu);
	dst[4] = (uint8_t)((value >> 8) & 0xffu);
	dst[5] = (uint8_t)(value & 0xffu);
}

static uint32_t
session_build_settings_payload(hive_session_t *s, uint8_t out[36])
{
	uint32_t n;

	n = 0;
	settings_param_write(out + n,
	                     (uint16_t)HIVE_SETTINGS_HEADER_TABLE_SIZE,
	                     s->local_settings.header_table_size);
	n += 6u;
	if (s->role == HIVE_ROLE_CLIENT) {
		settings_param_write(out + n,
		                     (uint16_t)HIVE_SETTINGS_ENABLE_PUSH,
		                     s->local_settings.enable_push);
		n += 6u;
	}
	settings_param_write(out + n,
	                     (uint16_t)HIVE_SETTINGS_MAX_CONCURRENT_STREAMS,
	                     s->local_settings.max_concurrent_streams);
	n += 6u;
	settings_param_write(out + n,
	                     (uint16_t)HIVE_SETTINGS_INITIAL_WINDOW_SIZE,
	                     s->local_settings.initial_window_size);
	n += 6u;
	settings_param_write(out + n,
	                     (uint16_t)HIVE_SETTINGS_MAX_FRAME_SIZE,
	                     s->local_settings.max_frame_size);
	n += 6u;
	settings_param_write(out + n,
	                     (uint16_t)HIVE_SETTINGS_MAX_HEADER_LIST_SIZE,
	                     s->local_settings.max_header_list_size);
	n += 6u;

	return n;
}

static int
session_queue_server_preface(hive_session_t *s)
{
	uint8_t payload[36];
	uint32_t payload_len;

	payload_len = session_build_settings_payload(s, payload);
	send_queue_append_ctrl(
	    s, HIVE_FRAME_SETTINGS, 0u, 0u, payload, payload_len);
	return HIVE_OK;
}

static int
session_queue_client_preface(hive_session_t *s)
{
	uint8_t payload[36];
	uint32_t payload_len;

	if (s->send_buf_used + sizeof(client_preface_magic) > s->send_buf_cap)
		return HIVE_ERR_NOMEM;
	if (s->send_iov_count >= (int)s->opt_max_send_iov)
		return HIVE_ERR_NOMEM;

	memcpy(s->send_buf + s->send_buf_used,
	       client_preface_magic,
	       sizeof(client_preface_magic));
	s->send_iov[s->send_iov_count].iov_base =
	    s->send_buf + s->send_buf_used;
	s->send_iov[s->send_iov_count].iov_len = sizeof(client_preface_magic);
	s->send_iov_count++;
	s->send_buf_used += sizeof(client_preface_magic);

	payload_len = session_build_settings_payload(s, payload);
	send_queue_append_ctrl(
	    s, HIVE_FRAME_SETTINGS, 0u, 0u, payload, payload_len);
	return HIVE_OK;
}

static void
session_init_local_settings(hive_session_t *s, const hive_options_t *opt)
{
	s->local_settings.header_table_size = opt->opt_header_table_size;
	s->local_settings.enable_push = opt->opt_enable_push;
	s->local_settings.max_concurrent_streams =
	    opt->opt_max_concurrent_streams;
	s->local_settings.initial_window_size = opt->opt_initial_window_size;
	s->local_settings.max_frame_size = opt->opt_max_frame_size;
	s->local_settings.max_header_list_size = opt->opt_max_header_list_size;
}

static void
session_init_remote_defaults(hive_session_t *s)
{
	s->remote_settings.header_table_size = 4096u;
	s->remote_settings.enable_push = 1u;
	s->remote_settings.max_concurrent_streams = UINT32_MAX;
	s->remote_settings.initial_window_size = 65535u;
	s->remote_settings.max_frame_size = 16384u;
	s->remote_settings.max_header_list_size = UINT32_MAX;
}

static void
session_copy_options(hive_session_t *s, const hive_options_t *opt)
{
	s->opt_header_table_size = opt->opt_header_table_size;
	s->opt_enable_push = opt->opt_enable_push;
	s->opt_max_concurrent_streams = opt->opt_max_concurrent_streams;
	s->opt_initial_window_size = opt->opt_initial_window_size;
	s->opt_max_frame_size = opt->opt_max_frame_size;
	s->opt_max_header_list_size = opt->opt_max_header_list_size;
	s->opt_max_header_count = opt->opt_max_header_count;
	s->opt_max_continuation_size = opt->opt_max_continuation_size;
	s->opt_max_settings_pending = opt->opt_max_settings_pending;
	s->opt_rst_flood_threshold = opt->opt_rst_flood_threshold;
	s->opt_rst_flood_window_secs = opt->opt_rst_flood_window_secs;
	s->opt_max_send_iov = opt->opt_max_send_iov;
	s->opt_max_header_string_size = opt->opt_max_header_string_size;
	s->opt_no_http_messaging = opt->opt_no_http_messaging;
	s->opt_no_auto_ping_ack = opt->opt_no_auto_ping_ack;
}

static hive_session_t *
session_new_common(hive_role_t role,
                   const hive_mem_t *mem,
                   const hive_options_t *opt,
                   const hive_callbacks_t *callbacks,
                   void *user_data)
{
	const hive_mem_t *alloc;
	const hive_options_t *eff_opt;
	hive_session_t *s;
	int ret;

	if (callbacks == NULL || callbacks->send == NULL)
		return NULL;

	alloc = mem;
	if (alloc == NULL) {
		alloc = &null_allocator;
	} else if (alloc->malloc == NULL || alloc->free == NULL ||
	           alloc->calloc == NULL || alloc->realloc == NULL) {
		return NULL;
	}

	eff_opt = (opt != NULL) ? opt : &default_options;

	s = alloc->calloc(1u, sizeof(*s), alloc->ctx);
	if (s == NULL)
		return NULL;

	s->mem = *alloc;
	s->callbacks = *callbacks;
	s->user_data = user_data;
	s->role = (uint8_t)role;

	session_copy_options(s, eff_opt);
	session_init_local_settings(s, eff_opt);
	session_init_remote_defaults(s);

	s->session_state = HIVE_SESSION_OPEN;
	s->send_window = (int32_t)s->remote_settings.initial_window_size;
	s->recv_window = (int32_t)s->local_settings.initial_window_size;
	s->next_stream_id = (role == HIVE_ROLE_CLIENT) ? 1u : 2u;
	s->recv_state = (role == HIVE_ROLE_SERVER) ? RECV_CLIENT_PREFACE
	                                           : RECV_SERVER_PREFACE;

	ret = session_prealloc(s);
	if (ret != HIVE_OK)
		goto fail;

	ret = (role == HIVE_ROLE_SERVER) ? session_queue_server_preface(s)
	                                 : session_queue_client_preface(s);
	if (ret != HIVE_OK)
		goto fail;

	return s;

fail:
	session_prealloc_free(s);
	s->mem.free(s, s->mem.ctx);
	return NULL;
}

hive_session_t *
hive_session_server_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	return session_new_common(
	    HIVE_ROLE_SERVER, mem, opt, callbacks, user_data);
}

hive_session_t *
hive_session_client_new(const hive_mem_t *mem,
                        const hive_options_t *opt,
                        const hive_callbacks_t *callbacks,
                        void *user_data)
{
	return session_new_common(
	    HIVE_ROLE_CLIENT, mem, opt, callbacks, user_data);
}

hive_session_t *
hive_session_server_upgrade(const hive_mem_t *mem,
                            const hive_options_t *opt,
                            const hive_callbacks_t *callbacks,
                            void *user_data,
                            const uint8_t *settings_payload,
                            size_t settings_len)
{
	if (settings_payload == NULL && settings_len != 0)
		return NULL;
	return session_new_common(
	    HIVE_ROLE_SERVER, mem, opt, callbacks, user_data);
}

void
hive_session_free(hive_session_t *session)
{
	if (session == NULL)
		return;
	session_prealloc_free(session);
	session->mem.free(session, session->mem.ctx);
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
	struct iovec eff_iov[HIVE_SEND_IOV_MAX];
	int eff_cnt = 0;
	size_t total, skip, i;
	ssize_t written;

	if (session == NULL)
		return HIVE_ERR_INVALID_ARG;

	/*
	 * Only flush pending DATA sources when no partial batch is outstanding.
	 * While send_partial == 1 the existing iov batch must be fully drained
	 * before new DATA frames may be appended; appending would corrupt the
	 * send_partial_offset accounting.  See ARCHITECTURE.md §6.6.
	 */
	if (!session->send_partial)
		send_queue_flush_data(session);

	if (session->send_iov_count == 0)
		return HIVE_OK;

	/*
	 * Build effective iov by skipping send_partial_offset bytes worth of
	 * already-sent data from the front of the pending batch.
	 */
	skip = session->send_partial_offset;
	total = 0;
	for (i = 0; i < (size_t)session->send_iov_count; i++) {
		size_t entry_len = session->send_iov[i].iov_len;

		total += entry_len;
		if (skip >= entry_len) {
			skip -= entry_len; /* this entry already sent */
		} else {
			eff_iov[eff_cnt].iov_base =
			    (char *)session->send_iov[i].iov_base + skip;
			eff_iov[eff_cnt].iov_len = entry_len - skip;
			eff_cnt++;
			skip = 0;
		}
	}

	written = session->callbacks.send(
	    session, eff_iov, eff_cnt, session->user_data);

	if (written < 0) {
		/* Fatal — session is dead. */
		session->session_state = HIVE_SESSION_CLOSED;
		return HIVE_ERR_PROTOCOL;
	}

	session->send_partial_offset += (size_t)written;

	if (session->send_partial_offset >= total) {
		/* All bytes sent — reset queue for next batch. */
		session->send_iov_count = 0;
		session->send_buf_used = 0;
		session->send_partial_offset = 0;
		session->send_partial = 0;
	} else {
		/* Partial write — retain unsent tail. */
		session->send_partial = 1;
	}

	return HIVE_OK;
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
	if (session == NULL)
		return 0;
	return (session->send_iov_count > 0 || session->send_partial != 0) ? 1
	                                                                   : 0;
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
                                  const hive_nv_t *nva,
                                  size_t nvlen,
                                  int end_stream)
{
	if (session == NULL)
		return HIVE_ERR_INVALID_ARG;
	if (end_stream != 1)
		return HIVE_ERR_INVALID_ARG;
	if (nvlen > 0 && nva == NULL)
		return HIVE_ERR_INVALID_ARG;
	return HIVE_OK;
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
