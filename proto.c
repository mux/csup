/*-
 * Copyright (c) 2003, Maxime Henrion <mux@FreeBSD.org>
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/socket.h>

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
#include "main.h"
#include "mux.h"
#include "proto.h"

#define	PROTO_MAJ	17
#define	PROTO_MIN	0
#define	PROTO_SWVER	"Csup_0.1"
#define	PROTO_MAXLINE	2048

/*
 * XXX - we only print the error for the last connection attempt
 */
FILE *
cvsup_connect(struct config *config)
{
	FILE *f;
	char servname[6];
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
	if (error) {
		warnx("connect: %s", gai_strerror(error));
		return (NULL);
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
	f = fdopen(s, "r+");
	if (f == NULL)
		err(1, "fdopen");
	return (f);
}

/*
 * Get next line sent by server.
 */
static char *
cvsup_getline(FILE *f)
{
	size_t len;
	char *line;

	line = fgetln(f, &len);
	/* XXX - handle EOF from server. */
	line[len - 1] = '\0';
	return (line);
}

/* Negotiate protocol version with the server. */
static int
cvsup_negproto(FILE *f)
{
	char *cmd, *line, *maj, *min;
	int pmaj, pmin;

	fprintf(f, "PROTO %d %d %s\n", PROTO_MAJ, PROTO_MIN, PROTO_SWVER);
	line = cvsup_getline(f);
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
cvsup_login(FILE *f)
{
	char host[MAXHOSTNAMELEN];
	char *line, *cmd, *realm, *challenge;

	gethostname(host, sizeof(host));
	host[sizeof(host) - 1] = '\0';
	fprintf(f, "USER %s %s\n", getlogin(), host);
	line = cvsup_getline(f);
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
	fprintf(f, "AUTHMD5 . . .\n");
	line = cvsup_getline(f);
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
cvsup_fileattr(FILE *f, struct config *config)
{
	char *line, *cmd;
	int i, n, attr;

	lprintf(2, "Negotiating file attribute support\n");
	fprintf(f, "ATTR %d\n", FT_NUMBER);
	for (i = 0; i < FT_NUMBER; i++)
		fprintf(f, "%x\n", config->ftypes[i]);
	fprintf(f, ".\n");
	line = cvsup_getline(f);
	cmd = strsep(&line, " ");
	if (line == NULL || strcmp(cmd, "ATTR") != 0)
		goto bad;
	errno = 0;
	n = strtol(line, NULL, 10);
	if (errno || n != FT_NUMBER)
		goto bad;
	for (i = 0; i < n; i++) {
		line = cvsup_getline(f);
		errno = 0;
		attr = strtol(line, NULL, 16);
		if (errno || attr != config->ftypes[i])
			goto bad;
	}
	line = cvsup_getline(f);
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
cvsup_xchgcoll(FILE *f, struct config *config)
{
	struct collection *cur;
	char *line, *cmd, *coll, *release, *options, *tok;
	int opts;

	lprintf(2, "Exchanging collection information\n");
	STAILQ_FOREACH(cur, &config->collections, next)
		fprintf(f, "COLL %s %s %o %d\n.\n", cur->name,
		    cur->release, cur->umask, cur->options);
	fprintf(f, ".\n");
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		line = cvsup_getline(f);
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
		/* Eat the rest for now. */
		do {
			line = cvsup_getline(f);
			tok = line;
			cmd = strsep(&tok, " ");
			if (tok != NULL && strcmp(cmd, "!") == 0)
				printf("Server message: %s\n",
				    cvsup_unescape(tok));
		} while (strcmp(line, ".") != 0);
	}
	return (0);
bad:
	printf("Protocol error during collection exchange\n");
	return (1);
}

static int
cvsup_mux(FILE *f)
{
	int chan0, chan1;
	int error;

	lprintf(2, "Establishing multiplexed-mode data connection\n");
	fprintf(f, "MUX\n");
	fflush(f);
	chan0 = chan_open(fileno(f));
	chan1 = chan_listen();
	chan_printf(chan0, "CHAN %d\n", chan1);
	error = chan_accept(chan1);
	if (error) {
		/* XXX - Sync error message with CVSup. */
		fprintf(stderr, "Accept failed for chan %d\n", chan1);
		return (-1);
	}
	sleep(60 * 60);
	return (0);
}

/*
 * Initializes the connection to the CVSup server.  This includes
 * the protocol negotiation, logging in, exchanging file attributes
 * support and collections information.
 */
int
cvsup_init(FILE *f, struct config *config)
{
	char *cur, *line;
	int error;

	line = cvsup_getline(f);
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
	error = cvsup_negproto(f);
	if (error)
		return (error);
	error = cvsup_login(f);
	if (error)
		return (error);
	error = cvsup_fileattr(f, config);
	if (error)
		return (error);
	error = cvsup_xchgcoll(f, config);
	if (error)
		return (error);
	error = cvsup_mux(f);
	return (error);
}
