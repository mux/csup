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
#include <sys/socket.h>
#include <sys/stat.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "detailer.h"
#include "diff.h"
#include "fileattr.h"
#include "keyword.h"
#include "lister.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "stream.h"
#include "updater.h"

#define	PROTO_MAJ	17
#define	PROTO_MIN	0
#define	PROTO_SWVER	"Csup_0.1"
#define	PROTO_MAXLINE	2048

/*
 * XXX - we only print the error for the last connection attempt
 */
int
cvsup_connect(struct config *config)
{
	/* This is large enough to hold sizeof("cvsup") or any port number. */
	char servname[8];
	struct addrinfo *res, *ai, hints;
	int error, s;

	s = -1;
	if (config->port != 0)
		snprintf(servname, sizeof(servname), "%d", config->port);
	else
		strlcpy(servname, "cvsup", sizeof(servname));
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(config->host, servname, &hints, &res);
	/*
	 * Try with the hardcoded port number for OSes that don't
	 * have cvsup defined in the /etc/services file.
	 */
	if (error == EAI_SERVICE) {
		strlcpy(servname, "5999", sizeof(servname));
		error = getaddrinfo(config->host, servname, &hints, &res);
	}
	if (error) {
		lprintf(0, "Name lookup failure for \"%s\": %s\n", config->host,
		    gai_strerror(error));
		return (-1);
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1) 
			continue;
		error = connect(s, ai->ai_addr, ai->ai_addrlen);
		if (error) {
			close(s);
			s = -1;
			continue;
		}
	}
	freeaddrinfo(res);
	if (s == -1) {
		lprintf(0, "Cannot connect to %s: %s\n", config->host,
		    strerror(errno));
		return (-1);
	}
	config->socket = s;
	return (0);
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
		printf("Protocol negotiation failed: %s\n", line);
		return (1);
	} else if (strcmp(cmd, "PROTO") != 0)
		goto bad;
	maj = strsep(&line, " "); 
	min = strsep(&line, " "); 
	if (maj == NULL || min == NULL || line != NULL)
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
		printf("Server protocol version %s.%s not supported by client",
		    maj, min);
		return (1);
	}
	return (0);
bad:
	printf("Invalid PROTO command from server\n");
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
	if (realm == NULL || challenge == NULL || line != NULL)
		goto bad;
	if (strcmp(realm, ".") != 0 || strcmp(challenge, ".") != 0) {
		printf("Authentication required by the server and not supported"
		    "by client\n");
		return (1);
	}
	stream_printf(s, "AUTHMD5 . . .\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " "); 
	if (strcmp(cmd, "OK") == 0)
		return (0);
	if (strcmp(cmd, "!") == 0 && line != NULL) {
		printf("Server error: %s\n", line);
		return (1);
	}
bad:
	printf("Invalid server reply to AUTHMD5\n");
	return (1);
}

/*
 * File attribute support negotiation.
 * XXX - We don't really negotiate yet but expect the server
 * to have the same attributes support as us.
 */
static int
cvsup_fileattr(struct config *config)
{
	struct stream *s;
	char *line, *cmd;
	int i, n, attr;

	s = config->server;
	lprintf(2, "Negotiating file attribute support\n");
	stream_printf(s, "ATTR %d\n", config->supported->number);
	for (i = 0; i < config->supported->number; i++)
		stream_printf(s, "%x\n", config->supported->attrs[i]);
	stream_printf(s, ".\n");
	stream_flush(s);
	line = stream_getln(s, NULL);
	cmd = strsep(&line, " ");
	if (line == NULL || strcmp(cmd, "ATTR") != 0)
		goto bad;
	errno = 0;
	n = strtol(line, NULL, 10);
	if (errno || n != config->supported->number)
		goto bad;
	for (i = 0; i < n; i++) {
		line = stream_getln(s, NULL);
		errno = 0;
		attr = strtol(line, NULL, 16);
		if (errno || attr != config->supported->attrs[i])
			goto bad;
	}
	line = stream_getln(s, NULL);
	if (strcmp(line, ".") != 0)
		goto bad;
	return (0);
bad:
	printf("Protocol error negotiating attribute support\n");
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
	struct collection *cur;
	struct stream *s;
	char *line, *cmd, *coll, *options, *release, *ident, *rcskey;
	int error, opts;

	s = config->server;
	lprintf(2, "Exchanging collection information\n");
	STAILQ_FOREACH(cur, &config->collections, next)
		stream_printf(s, "COLL %s %s %o %d\n.\n", cur->name,
		    cur->release, cur->umask, cur->options);
	stream_printf(s, ".\n");
	stream_flush(s);
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		line = stream_getln(s, NULL);
		cmd = strsep(&line, " ");
		if (strcmp(cmd, "COLL") != 0)
			goto bad;
		coll = strsep(&line, " ");
		release = strsep(&line, " ");
		options = strsep(&line, " ");
		if (coll == NULL || release == NULL || options == NULL ||
		    line != NULL)
			goto bad;
		if (strcmp(coll, cur->name) != 0)
			goto bad;
		if (strcmp(release, cur->release) != 0)
			goto bad;
		errno = 0;
		opts = strtol(options, NULL, 10);
		if (errno)
			goto bad;
		cur->options = (cur->options | (opts & CO_SERVMAYSET)) &
		    ~(~opts & CO_SERVMAYCLEAR);
		cur->keyword = keyword_new();
		for (;;) {
			line = stream_getln(s, NULL);
		 	if (strcmp(line, ".") == 0)
				break;
			cmd = strsep(&line, " ");
			if (cmd != NULL && strcmp(cmd, "!") == 0) {
				printf("Server message: %s\n",
				    cvsup_unescape(line));
			} else if (strcmp(cmd, "PRFX") == 0) {
				cur->cvsroot = strdup(line);
				if (cur->cvsroot == NULL)
					goto bad;
			} else if (strcmp(cmd, "KEYALIAS") == 0) {
				ident = strsep(&line, " ");
				rcskey = strsep(&line, " ");
				if (ident == NULL || rcskey == NULL ||
				    line != NULL)
					goto bad;
				error = keyword_alias(cur->keyword, ident,
				    rcskey);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYON") == 0) {
				ident = strsep(&line, " ");
				if (ident == NULL || line != NULL)
					goto bad;
				error = keyword_enable(cur->keyword, ident);
				if (error)
					goto bad;
			} else if (strcmp(cmd, "KEYOFF") == 0) {
				ident = strsep(&line, " ");
				if (ident == NULL || line != NULL)
					goto bad;
				error = keyword_disable(cur->keyword, ident);
				if (error)
					goto bad;
			} 
		}
	}
	return (0);
bad:
	printf("Protocol error during collection exchange\n");
	return (1);
}

static int
cvsup_mux(struct config *config)
{
	struct stream *s;
	int id0, id1;
	int error;

	s = config->server;
	lprintf(2, "Establishing multiplexed-mode data connection\n");
	stream_printf(s, "MUX\n");
	stream_flush(s);
	id0 = chan_open(config->socket);
	if (id0 == -1) {
		fprintf(stderr, "chan_open() failed\n");
		return (-1);
	}
	id1 = chan_listen();
	if (id1 == -1) {
		fprintf(stderr, "chan_listen() failed\n");
		return (-1);
	}
	config->chan0 = stream_fdopen(id0, chan_read, chan_write, NULL);
	if (config->chan0 == NULL)
		return (-1);
	stream_printf(config->chan0, "CHAN %d\n", id1);
	stream_flush(config->chan0);
	error = chan_accept(id1);
	if (error) {
		/* XXX - Sync error message with CVSup. */
		fprintf(stderr, "Accept failed for chan %d\n", id1);
		return (-1);
	}
	config->chan1 = stream_fdopen(id1, chan_read, chan_write, NULL);
	if (config->chan1 == NULL) {
		stream_close(config->chan0);
		return (-1);
	}
	stream_close(config->server);
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
	struct stream *s;
	char *cur, *line;
	pthread_t lister_thread;
	pthread_t detailer_thread;
	pthread_t updater_thread;
	int error;

	/*
	 * We pass NULL for the close() function because we'll reuse
	 * the socket after the stream is closed.
	 */
	config->server = stream_fdopen(config->socket, read, write, NULL);
	if (config->server == NULL)
		return (-1);
	s = config->server;
	line = stream_getln(s, NULL);
	cur = strsep(&line, " "); 
	if (strcmp(cur, "OK") == 0) {
		cur = strsep(&line, " "); 
		cur = strsep(&line, " "); 
		cur = strsep(&line, " "); 
	} else if (strcmp(cur, "!") == 0) {
		printf("Rejected by server: %s\n", line);
		return (1);
	} else {
		printf("Invalid greeting from server\n");
		return (1);
	}
	lprintf(2, "Server software version: %s\n", cur != NULL ? cur : ".");
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
	pthread_create(&lister_thread, NULL, lister, config);
	pthread_create(&detailer_thread, NULL, detailer, config);
	pthread_create(&updater_thread, NULL, updater, config);
	lprintf(2, "Running\n");
	error = pthread_join(lister_thread, NULL);
	if (error)
		err(1, "pthread_join");
	error = pthread_join(detailer_thread, NULL);
	if (error)
		err(1, "pthread_join");
	error = pthread_join(updater_thread, NULL);
	if (error)
		err(1, "pthread_join");
	lprintf(2, "Finished successfully\n");
	return (error);
}
