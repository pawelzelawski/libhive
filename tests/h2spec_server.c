/*
 * h2spec_server.c — minimal TLS HTTP/2 conformance server for h2spec.
 *
 * Built on libhive.a and libtls. The server accepts HTTP/2-over-TLS (ALPN h2),
 * feeds decrypted bytes to hive_session_recv(), and drains queued frames via
 * hive_session_send().
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <tls.h>

#include "../include/hive.h"

#define H2SPEC_DEFAULT_PORT 8443
#define H2SPEC_MAX_CONNS 128
#define H2SPEC_READ_BUFSZ 65536

static const char embedded_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIURQzyeB0u91ElzcFDvukxaO/EgBgwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyOTA3MjY0M1oXDTM2MDQy\n"
    "NjA3MjY0M1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAr7XDjto/9mse1dIhDRhsb0s5aljYjMFRYAM7hUkMpMZk\n"
    "tICJJqnSR7LOhsdPLezmFaJGAr4Nme791xztBXvw8HdVqtxoKYMdgnAqtcdMgHD1\n"
    "PdDhDUHqBWi5Ag7QbmErQoiaBPUVPjwwQQrH+yK/2ID71PXp+MMRcMFll/m8E0Ac\n"
    "FfGvUHhWLS5iR02WA+AX2PRHauyPKnKKVzby0g8eZKUrJWomMI7HM5q1dCa8fewq\n"
    "mu5kjMQiihMxA/yLeWgwxDbdAlwlYSqEZEks9LpNmPLQ5eYtr3PNoXJkiMsz3dS6\n"
    "SXnvgQmOB/8xnyy8q23+lle6tWNKwkbWWnt5veq0HQIDAQABo1MwUTAdBgNVHQ4E\n"
    "FgQUF2GaUIlL544lOIZNx3pqx+pAG/UwHwYDVR0jBBgwFoAUF2GaUIlL544lOIZN\n"
    "x3pqx+pAG/UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAcZ49\n"
    "usup0MIDIG8OhXXaE0VZf5gXgDmXVvQxSYHBRzNOnetYB6swpMeK4ZG/Ft9z0jIq\n"
    "NV/hIX3g3m6DrV3M7ejy+1nsNyGXBGwNQyeMk+g7G8+L/MLh+O0gDoRSqMJzudDV\n"
    "KdubtndVviQ/xk1wOpCqNav0to2m/gD64B4bMWpOW+SnrKCHKVw1zC48t2rouU63\n"
    "U7UeHknLYTDALR5yLy+G3vfAvxZh+wdpN/rLHAoZnMx+XnAWJTAuSm84J4rlg0sQ\n"
    "edN7L6rSgE+gOy2gnNuygUvEeRCV2zAJRm/+kiAXNVVLzTCMVYJ5x1LRgbRG27+/\n"
    "zh+FHFA6rVbjPcQ9jg==\n"
    "-----END CERTIFICATE-----\n";

static const char embedded_key_pem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCvtcOO2j/2ax7V\n"
    "0iENGGxvSzlqWNiMwVFgAzuFSQykxmS0gIkmqdJHss6Gx08t7OYVokYCvg2Z7v3X\n"
    "HO0Fe/Dwd1Wq3Ggpgx2CcCq1x0yAcPU90OENQeoFaLkCDtBuYStCiJoE9RU+PDBB\n"
    "Csf7Ir/YgPvU9en4wxFwwWWX+bwTQBwV8a9QeFYtLmJHTZYD4BfY9Edq7I8qcopX\n"
    "NvLSDx5kpSslaiYwjsczmrV0Jrx97Cqa7mSMxCKKEzED/It5aDDENt0CXCVhKoRk\n"
    "SSz0uk2Y8tDl5i2vc82hcmSIyzPd1LpJee+BCY4H/zGfLLyrbf6WV7q1Y0rCRtZa\n"
    "e3m96rQdAgMBAAECggEALg+cACq+cb6LCspW17P7WQGDP5miyuMyjdTLGZmYsuk/\n"
    "JQI88eG6ugjpkvNUkllzx2AOslFYB25bJLT0fWeMNb1Su8Ojmp5Ach0tVRG1wEXc\n"
    "RCQpmuwYiOp29U8k3IqkaICev4Xv16y3iZVl7zOgzwSg/6anewrH26MXGsvgvbq0\n"
    "cZuSy4CdlFTPo82gEpI9hrRiwcAZfAO20KbNDsNk2cBajgjkgUQuyJCVS3BCsQJp\n"
    "SYgTG4mA23t6fczNW4j0sVUX+N8Cjog+glLUR8CVI0w4Zf+Y8W5XUz/F6wgHvcwy\n"
    "PWk5+ZDcpU8Df3XJo9leW868UP90ZaSR/YZ/oA+hkQKBgQDyY2RPqZ5ZR5m8x7Kg\n"
    "mcX74rh8iKFIBsWZ2lejMtuCovnpmlMeZdNOcrVaRg3SSoHlpTb9RyyOGyHmy3Np\n"
    "rAKCcEttSGRu4HUzcVvDY3BPFGpTwNAQwF1Zy/JqcKLbP4z/A4kmWKO9IYB9SVWi\n"
    "OZRmNbmRmeTfGLhni1G4rv1Q8wKBgQC5k8vVNHrX7ZdyxCGbOvrvTeIBv2IO3w2i\n"
    "hSIaEvI6dyVSIyYDShih3odVKON6Tgqwi9bOJ4rg1M0UrV7TW7KRJbMmkBS/qKkz\n"
    "d1kUDoEspzPa/QhM9aejE4+x7ncaAbNvjUtz8Ro0d3UYnMAFJEKVvL/lCxsopp8R\n"
    "YNQV9MWqrwKBgQDkbw9mlHCLq5MT+xA5kzKnhLBhjVKSUu9/Y+sb/x4pK/djVPHo\n"
    "wAY49Jo9jbAQ8+8fwmjkomM3OhLlM/B9MoLa84HiaEtew2MxLDBTIDAEFzVt4VU1\n"
    "tFVF/5NjBOw2vNngrDBhV0BZSm2Rpb9yt9lHynIs6mBscRu5We+WojRSSQKBgD83\n"
    "ZqdBUlt+FypEP8J2bAba/BNmU4wHVci4G27QZ22dKrx5NrjGI+/4MxfCbwM51JBh\n"
    "gpIFjFycgSP7DyNmyESDmCyZxkent8PNcy3O5xgD+TkvGwXEZQ+7WSbeufnE/JAS\n"
    "jNJ5HlkjHGN++jaGLJx/iMsIZn8Ji4RK/NRh5ngHAoGAcIFNerRxRdBbehMNTp3U\n"
    "Lm5mpESwbJgSE7u9r8yAY0tTCEx3kESkPSep2oSFp1P9Fpzw/uMkZK3ToO0nyHWM\n"
    "I9ODTZ2UgsYZ1k6MGiK8RwMD1/Wj1dN0irmj5xo2NcOdtUG5Jyq+gTLaaiA4eW1Q\n"
    "PecTRMChC59/EyMItzz320Q=\n"
    "-----END PRIVATE KEY-----\n";

typedef struct conn_ctx {
	int fd;
	struct tls *tls_conn;
	hive_session_t *session;
	uint8_t active;
	uint8_t handshake_done;
	uint8_t drain_and_close;
} conn_ctx_t;

static int
set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

static void
conn_reset(conn_ctx_t *c)
{
	if (c->session != NULL) {
		hive_session_free(c->session);
		c->session = NULL;
	}
	if (c->tls_conn != NULL) {
		(void)tls_close(c->tls_conn);
		tls_free(c->tls_conn);
		c->tls_conn = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	c->active = 0;
	c->handshake_done = 0;
	c->drain_and_close = 0;
}

static ssize_t
hive_send_cb(hive_session_t *session, const struct iovec *iov, int iovcnt,
    void *user_data)
{
	conn_ctx_t *c;
	ssize_t total;
	int i;

	(void)session;
	c = user_data;
	total = 0;
	for (i = 0; i < iovcnt; i++) {
		const uint8_t *p;
		size_t left;

		p = iov[i].iov_base;
		left = iov[i].iov_len;
		while (left > 0) {
			ssize_t n;

			n = tls_write(c->tls_conn, p, left);
			if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT)
				return total;
			if (n < 0) {
				fprintf(stderr, "tls_write: %s\n",
				    tls_error(c->tls_conn));
				return -1;
			}
			if (n == 0)
				return total;
			total += n;
			p += n;
			left -= (size_t)n;
		}
	}
	return total;
}

static int
hive_on_headers_complete(hive_session_t *session, uint32_t stream_id,
    uint8_t flags, void *user_data)
{
	static const uint8_t n_status[] = ":status";
	static const uint8_t v_status[] = "200";
	static const uint8_t n_len[] = "content-length";
	static const uint8_t v_len[] = "0";
	hive_nv_t nva[2];
	conn_ctx_t *c;
	int rc;

	(void)flags;
	c = user_data;
	if (hive_stream_get_user_data(session, stream_id) != NULL)
		return HIVE_OK;
	(void)hive_stream_set_user_data(session, stream_id, (void *)1);

	nva[0].name = n_status;
	nva[0].value = v_status;
	nva[0].name_len = sizeof(n_status) - 1u;
	nva[0].value_len = sizeof(v_status) - 1u;
	nva[0].flags = 0u;
	nva[1].name = n_len;
	nva[1].value = v_len;
	nva[1].name_len = sizeof(n_len) - 1u;
	nva[1].value_len = sizeof(v_len) - 1u;
	nva[1].flags = 0u;

	rc = hive_submit_response(session, stream_id, nva, 2u, NULL);
	if (rc != HIVE_OK)
		fprintf(stderr, "hive_submit_response(stream=%u) failed: %d\n",
		    stream_id, rc);
	if (rc == HIVE_ERR_PROTOCOL || rc == HIVE_ERR_SESSION_CLOSED)
		c->drain_and_close = 1;
	return HIVE_OK;
}

static int
hive_on_goaway(hive_session_t *session, uint32_t last_stream_id,
    uint32_t error_code, const uint8_t *debug_data, size_t debug_len,
    void *user_data)
{
	conn_ctx_t *c;

	(void)session;
	(void)last_stream_id;
	(void)error_code;
	(void)debug_data;
	(void)debug_len;
	c = user_data;
	c->drain_and_close = 1;
	return HIVE_OK;
}

static int
hive_on_connection_error(hive_session_t *session, int hive_err,
    uint32_t h2_error_code, void *user_data)
{
	conn_ctx_t *c;

	(void)session;
	fprintf(stderr, "connection error: hive_err=%d h2_err=0x%x\n",
	    hive_err, h2_error_code);
	c = user_data;
	c->drain_and_close = 1;
	return HIVE_OK;
}

static int
conn_init_hive(conn_ctx_t *c)
{
	hive_callbacks_t cb;

	memset(&cb, 0, sizeof(cb));
	cb.on_headers_complete = hive_on_headers_complete;
	cb.on_goaway = hive_on_goaway;
	cb.on_connection_error = hive_on_connection_error;
	cb.send = hive_send_cb;

	c->session = hive_session_server_new(NULL, NULL, &cb, c);
	if (c->session == NULL) {
		fprintf(stderr, "hive_session_server_new failed\n");
		return -1;
	}
	return 0;
}

static int
conn_do_handshake(conn_ctx_t *c)
{
	int rc;

	if (c->handshake_done)
		return 0;
	rc = tls_handshake(c->tls_conn);
	if (rc == 0) {
		c->handshake_done = 1;
		if (conn_init_hive(c) != 0)
			return -1;
		return 0;
	}
	if (rc == TLS_WANT_POLLIN || rc == TLS_WANT_POLLOUT)
		return 0;
	fprintf(stderr, "tls_handshake failed: %s\n", tls_error(c->tls_conn));
	return -1;
}

static int
conn_process_io(conn_ctx_t *c, int can_read, int can_write)
{
	if (!c->handshake_done) {
		if (conn_do_handshake(c) != 0)
			return -1;
		if (!c->handshake_done)
			return 0;
	}

	if (can_read && !c->drain_and_close && hive_session_want_read(c->session)) {
		for (;;) {
			uint8_t buf[H2SPEC_READ_BUFSZ];
			ssize_t n;
			size_t off;

			n = tls_read(c->tls_conn, buf, sizeof(buf));
			if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT)
				break;
			if (n < 0) {
				fprintf(stderr, "tls_read: %s\n",
				    tls_error(c->tls_conn));
				return -1;
			}
			if (n == 0)
				return -1;
			off = 0u;
			while (off < (size_t)n) {
				ssize_t consumed;

				consumed = hive_session_recv(c->session, buf + off,
				    (size_t)n - off);
				if (consumed < 0) {
					c->drain_and_close = 1;
					break;
				}
				if (consumed == 0)
					break;
				off += (size_t)consumed;
			}
			if (c->drain_and_close)
				break;
		}
	}

	if ((can_write || hive_session_want_write(c->session)) &&
	    hive_session_send(c->session) != HIVE_OK)
		return -1;

	if (c->drain_and_close && !hive_session_want_write(c->session))
		return -1;
	if (!hive_session_want_read(c->session) && !hive_session_want_write(c->session))
		return -1;
	return 0;
}

static int
create_listener(uint16_t port)
{
	struct sockaddr_in sa;
	int fd;
	int one;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;
	if (listen(fd, 128) < 0)
		goto fail;
	if (set_nonblock(fd) < 0)
		goto fail;
	return fd;

fail:
	close(fd);
	return -1;
}

static int
parse_port(int argc, char **argv, uint16_t *port_out)
{
	long v;
	char *end;
	int i;

	*port_out = H2SPEC_DEFAULT_PORT;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--port") != 0 || i + 1 >= argc)
			continue;
		errno = 0;
		v = strtol(argv[i + 1], &end, 10);
		if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
		    v <= 0 || v > 65535)
			return -1;
		*port_out = (uint16_t)v;
		i++;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct tls *tls_srv;
	struct tls_config *tls_cfg;
	conn_ctx_t conns[H2SPEC_MAX_CONNS];
	uint16_t port;
	int listen_fd;
	int i;

	signal(SIGPIPE, SIG_IGN);
	if (parse_port(argc, argv, &port) != 0) {
		fprintf(stderr, "usage: %s [--port N]\n", argv[0]);
		return 1;
	}
	if (tls_init() < 0) {
		fprintf(stderr, "tls_init failed\n");
		return 1;
	}
	tls_cfg = tls_config_new();
	if (tls_cfg == NULL) {
		fprintf(stderr, "tls_config_new failed\n");
		return 1;
	}
	if (tls_config_set_alpn(tls_cfg, "h2") != 0)
		goto tls_fail;
	if (tls_config_set_cert_mem(tls_cfg, (const uint8_t *)embedded_cert_pem,
	    strlen(embedded_cert_pem)) != 0)
		goto tls_fail;
	if (tls_config_set_key_mem(tls_cfg, (const uint8_t *)embedded_key_pem,
	    strlen(embedded_key_pem)) != 0)
		goto tls_fail;

	tls_srv = tls_server();
	if (tls_srv == NULL)
		goto tls_fail;
	if (tls_configure(tls_srv, tls_cfg) != 0) {
		fprintf(stderr, "tls_configure: %s\n", tls_error(tls_srv));
		tls_free(tls_srv);
		goto tls_fail;
	}
	tls_config_free(tls_cfg);

	listen_fd = create_listener(port);
	if (listen_fd < 0) {
		fprintf(stderr, "listen setup failed on port %u\n", port);
		tls_free(tls_srv);
		return 1;
	}
	for (i = 0; i < H2SPEC_MAX_CONNS; i++) {
		conns[i].fd = -1;
		conns[i].tls_conn = NULL;
		conns[i].session = NULL;
		conns[i].active = 0;
		conns[i].handshake_done = 0;
		conns[i].drain_and_close = 0;
	}

	fprintf(stderr, "h2spec_server listening on 0.0.0.0:%u (TLS ALPN h2)\n",
	    port);

	for (;;) {
		struct pollfd pfds[1 + H2SPEC_MAX_CONNS];
		int map[1 + H2SPEC_MAX_CONNS];
		int nfds;
		int j;

		pfds[0].fd = listen_fd;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		map[0] = -1;
		nfds = 1;
		for (i = 0; i < H2SPEC_MAX_CONNS; i++) {
			if (!conns[i].active)
				continue;
			pfds[nfds].fd = conns[i].fd;
			pfds[nfds].events = 0;
			if (!conns[i].handshake_done) {
				pfds[nfds].events = POLLIN | POLLOUT;
			} else {
				if (!conns[i].drain_and_close &&
				    hive_session_want_read(conns[i].session))
					pfds[nfds].events |= POLLIN;
				if (hive_session_want_write(conns[i].session))
					pfds[nfds].events |= POLLOUT;
				if (pfds[nfds].events == 0)
					pfds[nfds].events = POLLIN;
			}
			pfds[nfds].revents = 0;
			map[nfds] = i;
			nfds++;
		}

		if (poll(pfds, (nfds_t)nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			break;
		}

		if (pfds[0].revents & POLLIN) {
			for (;;) {
				int cfd;
				struct tls *tls_conn;
				int slot;
				int acc_rc;

				cfd = accept(pfds[0].fd, NULL, NULL);
				if (cfd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					fprintf(stderr, "accept failed: %s\n",
					    strerror(errno));
					break;
				}
				if (set_nonblock(cfd) != 0) {
					close(cfd);
					continue;
				}
				slot = -1;
				for (i = 0; i < H2SPEC_MAX_CONNS; i++) {
					if (!conns[i].active) {
						slot = i;
						break;
					}
				}
				if (slot < 0) {
					close(cfd);
					continue;
				}
				tls_conn = NULL;
				acc_rc = tls_accept_socket(tls_srv, &tls_conn, cfd);
				if (acc_rc != 0 && acc_rc != TLS_WANT_POLLIN &&
				    acc_rc != TLS_WANT_POLLOUT) {
					fprintf(stderr, "tls_accept_socket: %s\n",
					    tls_error(tls_srv));
					if (tls_conn != NULL)
						tls_free(tls_conn);
					close(cfd);
					continue;
				}
				conns[slot].fd = cfd;
				conns[slot].tls_conn = tls_conn;
				conns[slot].session = NULL;
				conns[slot].active = 1;
				conns[slot].handshake_done = (acc_rc == 0);
				conns[slot].drain_and_close = 0;
				if (conns[slot].handshake_done &&
				    conn_init_hive(&conns[slot]) != 0)
					conn_reset(&conns[slot]);
			}
		}

		for (j = 1; j < nfds; j++) {
			int idx;
			int rd;
			int wr;

			idx = map[j];
			if (idx < 0 || !conns[idx].active)
				continue;
			if (pfds[j].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				conn_reset(&conns[idx]);
				continue;
			}
			rd = (pfds[j].revents & POLLIN) != 0;
			wr = (pfds[j].revents & POLLOUT) != 0;
			if (conn_process_io(&conns[idx], rd, wr) != 0)
				conn_reset(&conns[idx]);
		}
	}

	for (i = 0; i < H2SPEC_MAX_CONNS; i++)
		if (conns[i].active)
			conn_reset(&conns[i]);
	close(listen_fd);
	tls_free(tls_srv);
	return 1;

tls_fail:
	fprintf(stderr, "tls config failed\n");
	tls_config_free(tls_cfg);
	return 1;
}


