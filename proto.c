/*-
 * Copyright (c) 2003-2006, Maxime Henrion <mux@FreeBSD.org>
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
 *
 * $FreeBSD: projects/csup/proto.c,v 1.63 2006/02/10 18:18:47 mux Exp $
 */

#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "detailer.h"
#include "diff.h"
#include "fattr.h"
#include "keyword.h"
#include "lister.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "queue.h"
#include "stream.h"
#include "threads.h"
#include "updater.h"

#define	PROTO_MAJ	17
#define	PROTO_MIN	0
#define	PROTO_SWVER	"CSUP_0_1"

static int		 proto_greet(struct config *);
static int		 proto_negproto(struct config *);
static int		 proto_login(struct config *);
static int		 proto_fileattr(struct config *);
static int		 proto_xchgcoll(struct config *);
static struct mux	*proto_mux(struct config *);

static int		 proto_escape(struct stream *, const char *);
static void		 proto_unescape(char *);

/* Connect to the CVSup server. */
int
proto_connect(struct config *config, int family, uint16_t port)
{
	char addrbuf[128];
	/* This is large enough to hold sizeof("cvsup") or any port number. */
	char servname[8];
	struct addrinfo *res, *ai, hints;
	const char *addr;
	fd_set connfd;
	int error, ok, rv, s;

	s = -1;
	if (port != 0)
		snprintf(servname, sizeof(servname), "%d", port);
	else {
		strncpy(servname, "cvsup", sizeof(servname) - 1);
		servname[sizeof(servname) - 1] = '\0';
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(config->host, servname, &hints, &res);
	/*
	 * Try with the hardcoded port number for OSes that don't
	 * have cvsup defined in the /etc/services file.
	 */
	if (error == EAI_SERVICE) {
		strncpy(servname, "5999", sizeof(servname) - 1);
		servname[sizeof(servname) - 1] = '\0';
		error = getaddrinfo(config->host, servname, &hints, &res);
	}
	if (error) {
		lprintf(0, "Name lookup failure for \"%s\": %s\n", config->host,
		    gai_strerror(error));
		return (-1);
	}
	ok = 0;
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s != -1) {
			error = connect(s, ai->ai_addr, ai->ai_addrlen);
			if (error && errno == EINTR) {
				FD_ZERO(&connfd);
				FD_SET(s, &connfd);
				do {
					rv = select(s + 1, &connfd,
					    NULL, NULL, NULL);
				} while (rv == -1 && errno == EINTR);
				if (rv == 1)
					error = 0;
			}
			if (error)
				close(s);
		}
		if (s == -1 || error) {
			addr = inet_ntop(ai->ai_family, ai->ai_addr, addrbuf,
			    sizeof(addrbuf));
			if (addr == NULL)
				err(1, "inet_ntop");
			lprintf(0, "Cannot connect to %s: %s\n", addrbuf,
			    strerror(errno));
			continue;
		}
		ok = 1;
		break;
	}
	freeaddrinfo(res);
	if (!ok)
		return (-1);
	config->socket = s;
	return (0);
}

/* Greet the server. */
static int
proto_greet(struct config *config)
{
	char *line, *cmd, *msg, *swver;
	struct stream *s;

	s = config->server;
	line = stream_getln(s, NULL);
	cmd = proto_get_ascii(&line);
	if (cmd == NULL)
		goto bad;
	if (strcmp(cmd, "OK") == 0) {
		(void)proto_get_ascii(&line);	/* major number */
		(void)proto_get_ascii(&line);	/* minor number */
		swver = proto_get_ascii(&line);
	} else if (strcmp(cmd, "!") == 0) {
		msg = proto_get_rest(&line);
		if (msg == NULL)
			goto bad;
		lprintf(-1, "Rejected by server: %s\n", msg);
		return (-1);
	} else
		goto bad;
	lprintf(2, "Server software version: %s\n",
	    swver != NULL ? swver : ".");
	return (0);
bad:
	lprintf(-1, "Invalid greeting from server\n");
	return (-1);
}

/* Negotiate protocol version with the server. */
static int
proto_negproto(struct config *config)
{
	struct stream *s;
	char *cmd, *line, *msg;
	int error, maj, min;

	s = config->server;
	proto_printf(s, "PROTO %d %d %s\n", PROTO_MAJ, PROTO_MIN, PROTO_SWVER);
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = proto_get_ascii(&line);
	if (line == NULL)
		goto bad;
	if (strcmp(cmd, "!") == 0) {
		msg = proto_get_rest(&line);
		lprintf(-1, "Protocol negotiation failed: %s\n", msg);
		return (1);
	} else if (strcmp(cmd, "PROTO") != 0)
		goto bad;
	error = proto_get_int(&line, &maj);
	if (!error)
		error = proto_get_int(&line, &min);
	if (error)
		goto bad;
	if (maj != PROTO_MAJ || min != PROTO_MIN) {
		lprintf(-1, "Server protocol version %d.%d not supported "
		    "by client\n", maj, min);
		return (1);
	}
	return (0);
bad:
	lprintf(-1, "Invalid PROTO command from server\n");
	return (1);
}

static int
proto_login(struct config *config)
{
	struct stream *s;
	char host[MAXHOSTNAMELEN];
	char *line, *cmd, *realm, *challenge, *msg;

	s = config->server;
	gethostname(host, sizeof(host));
	host[sizeof(host) - 1] = '\0';
	proto_printf(s, "USER %s %s\n", getlogin(), host);
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = proto_get_ascii(&line);
	realm = proto_get_ascii(&line);
	challenge = proto_get_ascii(&line);
	if (challenge == NULL || line != NULL)
		goto bad;
	if (strcmp(realm, ".") != 0 || strcmp(challenge, ".") != 0) {
		lprintf(-1, "Authentication required by the server and not "
		    "supported by client\n");
		return (1);
	}
	proto_printf(s, "AUTHMD5 . . .\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = proto_get_ascii(&line);
	if (strcmp(cmd, "OK") == 0)
		return (0);
	if (strcmp(cmd, "!") == 0) {
		msg = proto_get_rest(&line);
		if (msg == NULL)
			goto bad;
		lprintf(-1, "Server error: %s\n", msg);
		return (1);
	}
bad:
	lprintf(-1, "Invalid server reply to AUTHMD5\n");
	return (1);
}

/*
 * File attribute support negotiation.
 */
static int
proto_fileattr(struct config *config)
{
	fattr_support_t support;
	struct stream *s;
	char *line, *cmd;
	int i, n, attr;

	s = config->server;
	lprintf(2, "Negotiating file attribute support\n");
	proto_printf(s, "ATTR %d\n", FT_NUMBER);
	for (i = 0; i < FT_NUMBER; i++)
		proto_printf(s, "%x\n", fattr_supported(i));
	proto_printf(s, ".\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = proto_get_ascii(&line);
	if (line == NULL || strcmp(cmd, "ATTR") != 0)
		goto bad;
	errno = 0;
	n = strtol(line, NULL, 10);
	if (errno || n > FT_NUMBER)
		goto bad;
	for (i = 0; i < n; i++) {
		line = stream_getln(s, NULL);
		if (line == NULL)
			goto bad;
		errno = 0;
		attr = strtol(line, NULL, 16);
		if (errno)
			goto bad;
		support[i] = fattr_supported(i) & attr;
	}
	for (i = n; i < FT_NUMBER; i++)
		support[i] = 0;
	line = stream_getln(s, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	memcpy(config->fasupport, support, sizeof(config->fasupport));
	return (0);
bad:
	lprintf(-1, "Protocol error negotiating attribute support\n");
	return (1);
}

/*
 * Exchange collection information.
 */
static int
proto_xchgcoll(struct config *config)
{
	struct coll *cur;
	struct stream *s;
	char *line, *cmd, *coll, *options;
	char *msg, *release, *ident, *rcskey, *prefix;
	int error, opts;

	s = config->server;
	lprintf(2, "Exchanging collection information\n");
	STAILQ_FOREACH(cur, &config->colls, co_next)
		proto_printf(s, "COLL %s %s %o %d\n.\n", cur->co_name,
		    cur->co_release, cur->co_umask, cur->co_options);
	proto_printf(s, ".\n");
	stream_flush(s);
	STAILQ_FOREACH(cur, &config->colls, co_next) {
		if (cur->co_options & CO_SKIP)
			continue;
		line = stream_getln(s, NULL);
		if (line == NULL)
			goto bad;
		cmd = proto_get_ascii(&line);
		coll = proto_get_ascii(&line);
		release = proto_get_ascii(&line);
		options = proto_get_ascii(&line);
		if (options == NULL || line != NULL)
			goto bad;
		if (strcmp(cmd, "COLL") != 0)
			goto bad;
		if (strcmp(coll, cur->co_name) != 0)
			goto bad;
		if (strcmp(release, cur->co_release) != 0)
			goto bad;
		errno = 0;
		opts = strtol(options, NULL, 10);
		if (errno)
			goto bad;
		cur->co_options = (cur->co_options | (opts & CO_SERVMAYSET)) &
		    ~(~opts & CO_SERVMAYCLEAR);
		cur->co_keyword = keyword_new();
		while ((line = stream_getln(s, NULL)) != NULL) {
		 	if (strcmp(line, ".") == 0)
				break;
			cmd = proto_get_ascii(&line);
			if (cmd == NULL)
				goto bad;
			if (strcmp(cmd, "!") == 0) {
				msg = proto_get_rest(&line);
				if (msg == NULL)
					goto bad;
				lprintf(-1, "Server message: %s\n", msg);
			} else if (strcmp(cmd, "PRFX") == 0) {
				prefix = proto_get_ascii(&line);
				if (prefix == NULL || line != NULL)
					goto bad;
				cur->co_cvsroot = xstrdup(prefix);
			} else if (strcmp(cmd, "KEYALIAS") == 0) {
				ident = proto_get_ascii(&line);
				rcskey = proto_get_ascii(&line);
				if (rcskey == NULL || line != NULL)
					goto bad;
				error = keyword_alias(cur->co_keyword, ident,
				    rcskey);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYON") == 0) {
				ident = proto_get_ascii(&line);
				if (ident == NULL || line != NULL)
					goto bad;
				error = keyword_enable(cur->co_keyword, ident);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYOFF") == 0) {
				ident = proto_get_ascii(&line);
				if (ident == NULL || line != NULL)
					goto bad;
				error = keyword_disable(cur->co_keyword, ident);
				if (error)
					goto bad;
			}
		}
		if (line == NULL)
			goto bad;
	}
	return (0);
bad:
	lprintf(-1, "Protocol error during collection exchange\n");
	return (1);
}

static struct mux *
proto_mux(struct config *config)
{
	struct mux *m;
	struct stream *s, *wr;
	struct chan *chan0, *chan1;
	int id;

	s = config->server;
	lprintf(2, "Establishing multiplexed-mode data connection\n");
	proto_printf(s, "MUX\n");
	stream_flush(s);
	m = mux_open(config->socket, &chan0);
	if (m == NULL) {
		lprintf(-1, "mux_open() failed\n");
		return (NULL);
	}
	id = chan_listen(m);
	if (id == -1) {
		lprintf(-1, "chan_listen() failed\n");
		mux_close(m);
		return (NULL);
	}
	wr = stream_open(chan0, NULL, (stream_writefn_t *)chan_write, NULL);
	proto_printf(wr, "CHAN %d\n", id);
	stream_close(wr);
	chan1 = chan_accept(m, id);
	if (chan1 == NULL) {
		/* XXX - Sync error message with CVSup. */
		lprintf(-1, "Accept failed for channel %d\n", id);
		mux_close(m);
		return (NULL);
	}
	stream_close(config->server);
	config->chan0 = chan0;
	config->chan1 = chan1;
	return (m);
}

/*
 * Initializes the connection to the CVSup server.  This includes
 * the protocol negotiation, logging in, exchanging file attributes
 * support and collections information.
 */
int
proto_init(struct config *config)
{
	struct threads *workers;
	struct mux *m;
	int error, i;

	/*
	 * We pass NULL for the close() function because we'll reuse
	 * the socket after the stream is closed.
	 */
	config->server = stream_open_fd(config->socket, stream_read_fd,
	    stream_write_fd, NULL);
	error = proto_greet(config);
	if (!error)
		error = proto_negproto(config);
	if (!error)
		error = proto_login(config);
	if (!error)
		error = proto_fileattr(config);
	if (!error)
		error = proto_xchgcoll(config);
	if (error)
		return (error);

	m = proto_mux(config);
	if (m == NULL)
		return (-1);

	/* Initialize the fattr API.  Hopefully this is not needed
	   earlier, since it's only for fattr_mergedefault(). */
	fattr_init();

	workers = threads_new();
	threads_create(workers, lister, config);
	threads_create(workers, detailer, config);
	threads_create(workers, updater, config);
	lprintf(2, "Running\n");
	/* Wait for all the worker threads to finish. */
	for (i = 0; i < 3; i++)
		threads_wait(workers);
	threads_free(workers);
	lprintf(2, "Shutting down connection to server\n");
	chan_close(config->chan0);
	chan_close(config->chan1);
	chan_wait(config->chan0);
	chan_wait(config->chan1);
	mux_close(m);
	lprintf(2, "Finished successfully\n");
	fattr_fini();
	return (error);
}

/*
 * Write a string into the stream, escaping characters as needed.
 * Characters escaped:
 *
 * SPACE	-> "\_"
 * TAB		->  "\t"
 * NEWLINE	-> "\n"
 * CR		-> "\r"
 * \		-> "\\"
 */
static int
proto_escape(struct stream *wr, const char *s)
{
	size_t len;
	ssize_t n;
	char c;

	/* Handle characters that need escaping. */
	do {
		len = strcspn(s, " \t\r\n\\");
		n = stream_write(wr, s, len);
		if (n == -1)
			return (-1);
		c = s[len];
		switch (c) {
		case ' ':
			n = stream_write(wr, "\\_", 2);
			break;
		case '\t':
			n = stream_write(wr, "\\t", 2);
			break;
		case '\r':
			n = stream_write(wr, "\\r", 2);
			break;
		case '\n':
			n = stream_write(wr, "\\n", 2);
			break;
		case '\\':
			n = stream_write(wr, "\\\\", 2);
			break;
		}
		if (n == -1)
			return (-1);
		s += len + 1;
	} while (c != '\0');
	return (0);
}

/*
 * A simple printf() implementation specifically tailored for csup.
 * List of the supported formats:
 *
 * %c		Print a char.
 * %d or %i	Print an int as decimal.
 * %x		Print an int as hexadecimal.
 * %o		Print an int as octal.
 * %t		Print a time_t as decimal.
 * %s		Print a char * escaping some characters as needed.
 * %S		Print a char * without escaping.
 * %f		Print an encoded struct fattr *.
 * %F		Print an encoded struct fattr *, specifying the supported
 * 		attributes.
 */
int
proto_printf(struct stream *wr, const char *format, ...)
{
	fattr_support_t *support;
	long long longval;
	struct fattr *fa;
	const char *fmt;
	va_list ap;
	char *cp, *s, *attr;
	ssize_t n;
	int rv, val;
	char c;

	n = 0;
	rv = 0;
	fmt = format;
	va_start(ap, format);
	while ((cp = strchr(fmt, '%')) != NULL) {
		if (cp > fmt) {
			n = stream_write(wr, fmt, cp - fmt);
			if (n == -1)
				return (-1);
		}
		if (*++cp == '\0')
			goto done;
		switch (*cp) {
		case 'c':
			c = va_arg(ap, int);
			rv = stream_printf(wr, "%c", c);
			break;
		case 'd':
		case 'i':
			val = va_arg(ap, int);
			rv = stream_printf(wr, "%d", val);
			break;
		case 'x':
			val = va_arg(ap, int);
			rv = stream_printf(wr, "%x", val);
			break;
		case 'o':
			val = va_arg(ap, int);
			rv = stream_printf(wr, "%o", val);
			break;
		case 'S':
			s = va_arg(ap, char *);
			rv = stream_printf(wr, "%s", s);
			break;
		case 's':
			s = va_arg(ap, char *);
			if (s == NULL)
				break;
			rv = proto_escape(wr, s);
			break;
		case 't':
			longval = (long long)va_arg(ap, time_t);
			rv = stream_printf(wr, "%lld", longval);
			break;
		case 'f':
			fa = va_arg(ap, struct fattr *);
			attr = fattr_encode(fa, NULL);
			rv = proto_escape(wr, attr);
			free(attr);
			break;
		case 'F':
			fa = va_arg(ap, struct fattr *);
			support = va_arg(ap, fattr_support_t *);
			attr = fattr_encode(fa, *support);
			rv = proto_escape(wr, attr);
			free(attr);
			break;
		case '%':
			n = stream_write(wr, "%", 1);
			if (n == -1)
				return (-1);
			break;
		}
		if (rv == -1)
			return (-1);
		fmt = cp + 1;
	}
	if (*fmt != '\0') {
		rv = stream_printf(wr, "%s", fmt);
		if (rv == -1)
			return (-1);
	}
done:
	va_end(ap);
	return (0);
}

/*
 * Unescape the string, see proto_escape().
 */
static void
proto_unescape(char *s)
{
	char *cp, *cp2;

	cp = s;
	while ((cp = strchr(cp, '\\')) != NULL) {
		switch (cp[1]) {
		case '_':
			*cp = ' ';
			break;
		case 't':
			*cp = '\t';
			break;
		case 'r':
			*cp = '\r';
			break;
		case 'n':
			*cp = '\n';
			break;
		case '\\':
			*cp = '\\';
			break;
		default:
			*cp = *(cp + 1);
		}
		cp2 = ++cp;
		while (*cp2 != '\0') {
			*cp2 = *(cp2 + 1);
			cp2++;
		}
	}
}

/*
 * Get an ascii token in the string.
 */
char *
proto_get_ascii(char **s)
{
	char *ret;

	if (s == NULL)
		return (NULL);
	ret = strsep(s, " ");
	if (ret == NULL)
		return (NULL);
	proto_unescape(ret);
	return (ret);
}

/*
 * Get the rest of the string.
 */
char *
proto_get_rest(char **s)
{
	char *ret;

	if (s == NULL)
		return (NULL);
	ret = *s;
	proto_unescape(ret);
	*s = NULL;
	return (ret);
}

/*
 * Get an int token.
 */
int
proto_get_int(char **s, int *val)
{
	char *cp, *end;

	cp = proto_get_ascii(s);
	if (cp == NULL)
		return (-1);
	errno = 0;
	*val = strtol(cp, &end, 10);
	if (errno || *end != '\0')
		return (-1);
	return (0);
}

/*
 * Get a time_t token.
 *
 * Ideally, we would use an intmax_t and strtoimax() here, but strtoll()
 * is more portable and 64bits should be enough for a timestamp.
 */
int
proto_get_time(char **s, time_t *val)
{
	long long tmp;
	char *cp, *end;

	cp = proto_get_ascii(s);
	if (cp == NULL)
		return (-1);
	errno = 0;
	tmp = strtoll(cp, &end, 10);
	if (errno || *end != '\0')
		return (-1);
	*val = (time_t)tmp;
	return (0);
}
