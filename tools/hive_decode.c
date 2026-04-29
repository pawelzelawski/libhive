/*
 * hive_decode.c - offline HTTP/2 frame decoder
 *
 * Reads raw HTTP/2 bytes from a file or stdin and pretty-prints each frame.
 * Built with src/hive_frame_bare.c only (no session/HPACK dependencies).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hive_frame_bare.h"

#define FRAME_HEADER_LEN 9u

enum read_status {
	READ_OK = 0,
	READ_EOF,
	READ_TRUNC,
	READ_ERR,
};

static const char *
frame_type_name(uint8_t type)
{
	switch (type) {
	case HIVE_FRAME_DATA:
		return "DATA";
	case HIVE_FRAME_HEADERS:
		return "HEADERS";
	case HIVE_FRAME_PRIORITY:
		return "PRIORITY";
	case HIVE_FRAME_RST_STREAM:
		return "RST_STREAM";
	case HIVE_FRAME_SETTINGS:
		return "SETTINGS";
	case HIVE_FRAME_PUSH_PROMISE:
		return "PUSH_PROMISE";
	case HIVE_FRAME_PING:
		return "PING";
	case HIVE_FRAME_GOAWAY:
		return "GOAWAY";
	case HIVE_FRAME_WINDOW_UPDATE:
		return "WINDOW_UPDATE";
	case HIVE_FRAME_CONTINUATION:
		return "CONTINUATION";
	default:
		return "UNKNOWN";
	}
}

static const char *
settings_name(uint16_t id)
{
	switch (id) {
	case 0x1:
		return "HEADER_TABLE_SIZE";
	case 0x2:
		return "ENABLE_PUSH";
	case 0x3:
		return "MAX_CONCURRENT_STREAMS";
	case 0x4:
		return "INITIAL_WINDOW_SIZE";
	case 0x5:
		return "MAX_FRAME_SIZE";
	case 0x6:
		return "MAX_HEADER_LIST_SIZE";
	default:
		return "UNKNOWN";
	}
}

static const char *
error_code_name(uint32_t code)
{
	switch (code) {
	case HIVE_H2_NO_ERROR:
		return "NO_ERROR";
	case HIVE_H2_PROTOCOL_ERROR:
		return "PROTOCOL_ERROR";
	case HIVE_H2_INTERNAL_ERROR:
		return "INTERNAL_ERROR";
	case HIVE_H2_FLOW_CONTROL_ERROR:
		return "FLOW_CONTROL_ERROR";
	case HIVE_H2_SETTINGS_TIMEOUT:
		return "SETTINGS_TIMEOUT";
	case HIVE_H2_STREAM_CLOSED:
		return "STREAM_CLOSED";
	case HIVE_H2_FRAME_SIZE_ERROR:
		return "FRAME_SIZE_ERROR";
	case HIVE_H2_REFUSED_STREAM:
		return "REFUSED_STREAM";
	case HIVE_H2_CANCEL:
		return "CANCEL";
	case HIVE_H2_COMPRESSION_ERROR:
		return "COMPRESSION_ERROR";
	case HIVE_H2_CONNECT_ERROR:
		return "CONNECT_ERROR";
	case HIVE_H2_ENHANCE_YOUR_CALM:
		return "ENHANCE_YOUR_CALM";
	case HIVE_H2_INADEQUATE_SECURITY:
		return "INADEQUATE_SECURITY";
	case HIVE_H2_HTTP_1_1_REQUIRED:
		return "HTTP_1_1_REQUIRED";
	default:
		return "UNKNOWN";
	}
}

static uint32_t
read_u32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t
read_u16_be(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static enum read_status
read_exact(FILE *fp, uint8_t *buf, size_t len, size_t *bytes_read)
{
	size_t off;

	off = 0;
	while (off < len) {
		size_t n;

		n = fread(buf + off, 1, len - off, fp);
		if (n == 0) {
			if (ferror(fp)) {
				*bytes_read = off;
				return READ_ERR;
			}
			*bytes_read = off;
			if (off == 0) {
				return READ_EOF;
			}
			return READ_TRUNC;
		}
		off += n;
	}
	*bytes_read = off;
	return READ_OK;
}

static void
append_flag(char *dst, size_t dstsz, const char *flag)
{
	size_t len;

	len = strlen(dst);
	if (len >= dstsz) {
		return;
	}
	if (len > 0) {
		(void)snprintf(dst + len, dstsz - len, "|%s", flag);
	} else {
		(void)snprintf(dst + len, dstsz - len, "%s", flag);
	}
}

static void
format_flags(uint8_t type, uint8_t flags, char *out, size_t outsz)
{
	uint8_t known;
	uint8_t unknown;

	out[0] = '\0';
	known = 0;

	switch (type) {
	case HIVE_FRAME_DATA:
		if ((flags & HIVE_FLAG_END_STREAM) != 0) {
			append_flag(out, outsz, "END_STREAM");
			known |= HIVE_FLAG_END_STREAM;
		}
		if ((flags & HIVE_FLAG_PADDED) != 0) {
			append_flag(out, outsz, "PADDED");
			known |= HIVE_FLAG_PADDED;
		}
		break;
	case HIVE_FRAME_HEADERS:
		if ((flags & HIVE_FLAG_END_STREAM) != 0) {
			append_flag(out, outsz, "END_STREAM");
			known |= HIVE_FLAG_END_STREAM;
		}
		if ((flags & HIVE_FLAG_END_HEADERS) != 0) {
			append_flag(out, outsz, "END_HEADERS");
			known |= HIVE_FLAG_END_HEADERS;
		}
		if ((flags & HIVE_FLAG_PADDED) != 0) {
			append_flag(out, outsz, "PADDED");
			known |= HIVE_FLAG_PADDED;
		}
		if ((flags & HIVE_FLAG_PRIORITY) != 0) {
			append_flag(out, outsz, "PRIORITY");
			known |= HIVE_FLAG_PRIORITY;
		}
		break;
	case HIVE_FRAME_SETTINGS:
	case HIVE_FRAME_PING:
		if ((flags & HIVE_FLAG_ACK) != 0) {
			append_flag(out, outsz, "ACK");
			known |= HIVE_FLAG_ACK;
		}
		break;
	case HIVE_FRAME_PUSH_PROMISE:
		if ((flags & HIVE_FLAG_END_HEADERS) != 0) {
			append_flag(out, outsz, "END_HEADERS");
			known |= HIVE_FLAG_END_HEADERS;
		}
		if ((flags & HIVE_FLAG_PADDED) != 0) {
			append_flag(out, outsz, "PADDED");
			known |= HIVE_FLAG_PADDED;
		}
		break;
	case HIVE_FRAME_CONTINUATION:
		if ((flags & HIVE_FLAG_END_HEADERS) != 0) {
			append_flag(out, outsz, "END_HEADERS");
			known |= HIVE_FLAG_END_HEADERS;
		}
		break;
	default:
		break;
	}

	unknown = (uint8_t)(flags & (uint8_t)(~known));
	if (unknown != 0) {
		char extra[32];

		(void)snprintf(extra, sizeof(extra), "UNKNOWN(0x%02x)", unknown);
		append_flag(out, outsz, extra);
	}
}

static void
print_payload_fields(const frame_hdr_t *h, const uint8_t *payload)
{
	uint32_t i;

	switch (h->type) {
	case HIVE_FRAME_SETTINGS:
		if ((h->flags & HIVE_FLAG_ACK) != 0) {
			printf("  ACK (no payload expected)\n");
			break;
		}
		if ((h->length % 6u) != 0) {
			printf("  malformed SETTINGS payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		for (i = 0; i < h->length; i += 6u) {
			uint16_t id;
			uint32_t value;

			id = read_u16_be(payload + i);
			value = read_u32_be(payload + i + 2);
			printf("  %s: %" PRIu32 "\n", settings_name(id), value);
		}
		break;
	case HIVE_FRAME_PING:
		if (h->length != 8u) {
			printf("  malformed PING payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		if ((h->flags & HIVE_FLAG_ACK) != 0) {
			printf("  ACK\n");
		}
		printf("  opaque: ");
		for (i = 0; i < 8; i++) {
			printf("%02x", payload[i]);
		}
		printf("\n");
		break;
	case HIVE_FRAME_GOAWAY:
		if (h->length < 8u) {
			printf("  malformed GOAWAY payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		printf("  last stream id: %u\n",
		       (unsigned)(read_u32_be(payload) & 0x7fffffffu));
		printf("  error code: %s\n", error_code_name(read_u32_be(payload + 4)));
		printf("  debug data: ");
		for (i = 8; i < h->length; i++) {
			printf("%02x", payload[i]);
		}
		printf("\n");
		break;
	case HIVE_FRAME_WINDOW_UPDATE:
		if (h->length != 4u) {
			printf("  malformed WINDOW_UPDATE payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		printf("  window size increment: %" PRIu32 "\n",
		       read_u32_be(payload) & 0x7fffffffu);
		break;
	case HIVE_FRAME_RST_STREAM:
		if (h->length != 4u) {
			printf("  malformed RST_STREAM payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		printf("  error code: %s\n", error_code_name(read_u32_be(payload)));
		break;
	case HIVE_FRAME_HEADERS:
		printf("  compressed header block: %zu bytes (HPACK not decoded)\n",
		       (size_t)h->length);
		break;
	case HIVE_FRAME_CONTINUATION:
		printf("  compressed continuation: %zu bytes (HPACK not decoded)\n",
		       (size_t)h->length);
		break;
	case HIVE_FRAME_PRIORITY:
		if (h->length != 5u) {
			printf("  malformed PRIORITY payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		printf("  stream dependency: %u\n",
		       (unsigned)(read_u32_be(payload) & 0x7fffffffu));
		printf("  exclusive: %u\n", (unsigned)((payload[0] >> 7) & 0x1u));
		printf("  weight: %u\n", (unsigned)payload[4] + 1u);
		break;
	case HIVE_FRAME_PUSH_PROMISE:
		if (h->length < 4u) {
			printf("  malformed PUSH_PROMISE payload: %zu bytes\n",
			       (size_t)h->length);
			break;
		}
		printf("  promised stream id: %u\n",
		       (unsigned)(read_u32_be(payload) & 0x7fffffffu));
		printf("  compressed header block: %zu bytes (HPACK not decoded)\n",
		       (size_t)(h->length - 4u));
		break;
	default:
		printf("  <payload not displayed>\n");
		break;
	}
}

int
main(int argc, char **argv)
{
	uint8_t header_buf[FRAME_HEADER_LEN];
	uint8_t *payload;
	frame_hdr_t hdr;
	enum read_status status;
	FILE *fp;
	size_t bytes_read;
	uint64_t frame_no;
	char flags_text[96];

	if (argc > 2) {
		fprintf(stderr, "usage: %s [file]\n", argv[0]);
		fprintf(stderr, "       %s -\n", argv[0]);
		return 1;
	}

	if (argc == 2 && strcmp(argv[1], "-") != 0) {
		fp = fopen(argv[1], "rb");
		if (fp == NULL) {
			fprintf(stderr, "fopen(%s): %s\n", argv[1], strerror(errno));
			return 1;
		}
	} else {
		fp = stdin;
	}

	frame_no = 0;
	payload = NULL;

	for (;;) {
		status = read_exact(fp, header_buf, FRAME_HEADER_LEN, &bytes_read);
		if (status != READ_OK) {
			if (status == READ_EOF) {
				break;
			}
			fprintf(stderr,
			        "decode error: incomplete frame header (%zu/%u bytes)\n",
			        bytes_read, FRAME_HEADER_LEN);
			if (fp != stdin) {
				(void)fclose(fp);
			}
			return 1;
		}

		frame_hdr_parse(header_buf, &hdr);

		if (hdr.length > 0) {
			payload = (uint8_t *)malloc((size_t)hdr.length);
			if (payload == NULL) {
				fprintf(stderr, "malloc(%zu) failed\n", (size_t)hdr.length);
				if (fp != stdin) {
					(void)fclose(fp);
				}
				return 1;
			}

			status = read_exact(fp, payload, (size_t)hdr.length, &bytes_read);
			if (status != READ_OK) {
				fprintf(stderr,
				        "decode error: truncated payload for frame %" PRIu64
				        " (%zu/%zu bytes)\n",
				        frame_no + 1u, bytes_read, (size_t)hdr.length);
				free(payload);
				if (fp != stdin) {
					(void)fclose(fp);
				}
				return 1;
			}
		} else {
			payload = NULL;
		}

		frame_no++;
		format_flags(hdr.type, hdr.flags, flags_text, sizeof(flags_text));

		printf("--- frame %" PRIu64 " ---\n", frame_no);
		printf("type:     0x%x  %s\n", hdr.type, frame_type_name(hdr.type));
		if (flags_text[0] != '\0') {
			printf("flags:    0x%02x %s\n", hdr.flags, flags_text);
		} else {
			printf("flags:    0x%02x\n", hdr.flags);
		}
		if (hdr.stream_id == 0) {
			printf("stream:   0 (connection)\n");
		} else {
			printf("stream:   %u\n", hdr.stream_id);
		}
		printf("length:   %u bytes\n", hdr.length);
		print_payload_fields(&hdr, payload);
		printf("\n");

		free(payload);
		payload = NULL;
	}

	if (fp != stdin) {
		(void)fclose(fp);
	}
	return 0;
}


