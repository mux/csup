/*-
 * Copyright (c) 2003-2004, Maxime Henrion <mux@FreeBSD.org>
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
 * $Id$
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
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
#include "stream.h"
#include "threads.h"
#include "updater.h"

#define	PROTO_MAJ	17
#define	PROTO_MIN	0
#define	PROTO_SWVER	"CSUP_0_1"

static int	cvsup_greet(struct config *);
static int	cvsup_negproto(struct config *);
static int	cvsup_login(struct config *);
static int	cvsup_fileattr(struct config *);
static int	cvsup_xchgcoll(struct config *);
static int	cvsup_mux(struct config *);

/* Connect to the CVSup server. */
int
cvsup_connect(struct config *config)
{
	/* This is large enough to hold sizeof("cvsup") or any port number. */
	char servname[8];
	struct addrinfo *res, *ai, hints;
	fd_set connfd;
	int error, s;

	s = -1;
	if (config->port != 0)
		snprintf(servname, sizeof(servname), "%d", config->port);
	else {
		strncpy(servname, "cvsup", sizeof(servname) - 1);
		servname[sizeof(servname) - 1] = '\0';
	}
	memset(&hints, 0, sizeof(hints));
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
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1) {
			perror("Cannot create socket");
			continue;
		}
		error = connect(s, ai->ai_addr, ai->ai_addrlen);
		if (error && errno == EINTR) {
			FD_ZERO(&connfd);
			FD_SET(s, &connfd);
			do {
				error = select(s + 1, &connfd, NULL, NULL,
				    NULL);
			} while (error == -1 && errno == EINTR);
			if (error == 1)
				error = 0;
		}
		if (error) {
			close(s);
			lprintf(0, "Cannot connect to %s: %s\n", config->host,
			    strerror(errno));
		}
	}
	freeaddrinfo(res);
	config->socket = s;
	return (error);
}

/* Greet the server. */
static int
cvsup_greet(struct config *config)
{
	char *line, *tok;
	struct stream *s;

	s = config->server;
	line = stream_getln(s, NULL);
	tok = strsep(&line, " "); 
	if (tok == NULL)
		goto bad;
	if (strcmp(tok, "OK") == 0) {
		tok = strsep(&line, " "); 
		tok = strsep(&line, " "); 
		tok = strsep(&line, " "); 
	} else if (strcmp(tok, "!") == 0) {
		lprintf(-1, "Rejected by server: %s\n", line);
		return (-1);
	} else
		goto bad;
	lprintf(2, "Server software version: %s\n", tok != NULL ? tok : ".");
	return (0);
bad:
	lprintf(-1, "Invalid greeting from server\n");
	return (-1);
}

/* Negotiate protocol version with the server. */
static int
cvsup_negproto(struct config *config)
{
	struct stream *s;
	char *cmd, *line, *maj, *min;
	int pmaj, pmin;

	s = config->server;
	stream_printf(s, "PROTO %d %d %s\n", PROTO_MAJ, PROTO_MIN, PROTO_SWVER);
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " "); 
	if (strcmp(cmd, "!") == 0) {
		lprintf(-1, "Protocol negotiation failed: %s\n", line);
		return (1);
	} else if (strcmp(cmd, "PROTO") != 0)
		goto bad;
	maj = strsep(&line, " "); 
	min = strsep(&line, " "); 
	if (min == NULL || line != NULL)
		goto bad;
	errno = 0;
	pmaj = strtol(maj, NULL, 10);
	if (errno == EINVAL)
		goto bad;
	errno = 0;
	pmin = strtol(min, NULL, 10);
	if (errno == EINVAL)
		goto bad;
	if (pmaj != PROTO_MAJ || pmin != PROTO_MIN) {
		lprintf(-1, "Server protocol version %s.%s not supported "
		    "by client\n", maj, min);
		return (1);
	}
	return (0);
bad:
	lprintf(-1, "Invalid PROTO command from server\n");
	return (1);
}

static int
cvsup_login(struct config *config)
{
	struct stream *s;
	char host[MAXHOSTNAMELEN];
	char *line, *cmd, *realm, *challenge;

	s = config->server;
	gethostname(host, sizeof(host));
	host[sizeof(host) - 1] = '\0';
	stream_printf(s, "USER %s %s\n", getlogin(), host);
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " ");
	realm = strsep(&line, " ");
	challenge = strsep(&line, " ");
	if (challenge == NULL || line != NULL)
		goto bad;
	if (strcmp(realm, ".") != 0 || strcmp(challenge, ".") != 0) {
		lprintf(-1, "Authentication required by the server and not "
		    "supported by client\n");
		return (1);
	}
	stream_printf(s, "AUTHMD5 . . .\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " "); 
	if (strcmp(cmd, "OK") == 0)
		return (0);
	if (strcmp(cmd, "!") == 0 && line != NULL) {
		lprintf(-1, "Server error: %s\n", line);
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
cvsup_fileattr(struct config *config)
{
	struct stream *s;
	char *line, *cmd;
	int i, n, attr;

	s = config->server;
	lprintf(2, "Negotiating file attribute support\n");
	stream_printf(s, "ATTR %d\n", FT_NUMBER);
	for (i = 0; i < FT_NUMBER; i++)
		stream_printf(s, "%x\n", fattr_supported(i));
	stream_printf(s, ".\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " ");
	if (line == NULL || strcmp(cmd, "ATTR") != 0)
		goto bad;
	errno = 0;
	n = strtol(line, NULL, 10);
	if (errno || n != FT_NUMBER)
		/* XXX - We should merge here. */
		goto bad;
	for (i = 0; i < n; i++) {
		line = stream_getln(s, NULL);
		errno = 0;
		attr = strtol(line, NULL, 16);
		if (errno || attr != fattr_supported(i))
			/* XXX - Ditto. */
			goto bad;
	}
	line = stream_getln(s, NULL);
	if (strcmp(line, ".") != 0)
		goto bad;
	return (0);
bad:
	lprintf(-1, "Protocol error negotiating attribute support\n");
	return (1);
}

/*
 * Change "\_" with " " in the string.
 */
static char *
cvsup_unescape(char *str)
{
	char *c;
	int len;

	len = strlen(str);
	while ((c = strstr(str, "\\_")) != NULL) {
		*c = ' ';
		while (*++c != '\0')
			*c = *(c + 1);
	}
	return (str);
}

/*
 * Exchange collection information.
 */
static int
cvsup_xchgcoll(struct config *config)
{
	struct coll *cur;
	struct stream *s;
	char *line, *cmd, *coll, *options, *release, *ident, *rcskey;
	int error, opts;

	s = config->server;
	lprintf(2, "Exchanging collection information\n");
	STAILQ_FOREACH(cur, &config->colls, co_next)
		stream_printf(s, "COLL %s %s %o %d\n.\n", cur->co_name,
		    cur->co_release, cur->co_umask, cur->co_options);
	stream_printf(s, ".\n");
	stream_flush(s);
	STAILQ_FOREACH(cur, &config->colls, co_next) {
		if (cur->co_options & CO_SKIP)
			continue;
		line = stream_getln(s, NULL);
		if (line == NULL)
			goto bad;
		cmd = strsep(&line, " ");
		coll = strsep(&line, " ");
		release = strsep(&line, " ");
		options = strsep(&line, " ");
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
			cmd = strsep(&line, " ");
			if (cmd == NULL)
				goto bad;
			if (strcmp(cmd, "!") == 0) {
				lprintf(-1, "Server message: %s\n",
				    cvsup_unescape(line));
			} else if (strcmp(cmd, "PRFX") == 0) {
				cur->co_cvsroot = strdup(line);
				if (cur->co_cvsroot == NULL)
					err(1, "strdup");
			} else if (strcmp(cmd, "KEYALIAS") == 0) {
				ident = strsep(&line, " ");
				rcskey = strsep(&line, " ");
				if (rcskey == NULL || line != NULL)
					goto bad;
				error = keyword_alias(cur->co_keyword, ident,
				    rcskey);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYON") == 0) {
				ident = strsep(&line, " ");
				if (ident == NULL || line != NULL)
					goto bad;
				error = keyword_enable(cur->co_keyword, ident);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYOFF") == 0) {
				ident = strsep(&line, " ");
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

static int
cvsup_mux(struct config *config)
{
	struct stream *s, *chan0;
	int id0, id1;
	int error;

	s = config->server;
	lprintf(2, "Establishing multiplexed-mode data connection\n");
	stream_printf(s, "MUX\n");
	stream_flush(s);
	error = mux_init(config->socket);
	if (error) {
		lprintf(-1, "mux_init() failed\n");
		return (error);
	}
	id0 = chan_open();
	if (id0 == -1) {
		lprintf(-1, "chan_open() failed\n");
		return (-1);
	}
	id1 = chan_listen();
	if (id1 == -1) {
		lprintf(-1, "chan_listen() failed\n");
		return (-1);
	}
	chan0 = stream_fdopen(id0, chan_read, chan_write, NULL);
	stream_printf(chan0, "CHAN %d\n", id1);
	stream_close(chan0);
	error = chan_accept(id1);
	if (error) {
		/* XXX - Sync error message with CVSup. */
		lprintf(-1, "Accept failed for channel %d\n", id1);
		return (-1);
	}
	stream_close(config->server);
	config->id0 = id0;
	config->id1 = id1;
	return (0);
}

/*
 * Initializes the connection to the CVSup server.  This includes
 * the protocol negotiation, logging in, exchanging file attributes
 * support and collections information.
 */
int
cvsup_init(struct config *config)
{
	struct threads *workers;
	int error, i;

	/*
	 * We pass NULL for the close() function because we'll reuse
	 * the socket after the stream is closed.
	 */
	config->server = stream_fdopen(config->socket, read, write, NULL);
	error = cvsup_greet(config);
	if (error)
		return (error);
	error = cvsup_negproto(config);
	if (error)
		return (error);
	error = cvsup_login(config);
	if (error)
		return (error);
	error = cvsup_fileattr(config);
	if (error)
		return (error);
	error = cvsup_xchgcoll(config);
	if (error)
		return (error);
	error = cvsup_mux(config);
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
	chan_close(config->id0);
	chan_close(config->id1);
	chan_wait(config->id0);
	chan_wait(config->id1);
	mux_fini();
	lprintf(2, "Finished successfully\n");
	return (error);
}
