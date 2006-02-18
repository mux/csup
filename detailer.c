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
 * $FreeBSD: projects/csup/detailer.c,v 1.38 2006/02/10 18:18:47 mux Exp $
 */

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "detailer.h"
#include "fixups.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "status.h"
#include "stream.h"

static int	detailer_coll(struct coll *, struct status *);
static int	detailer_dofile(struct coll *, struct status *, char *);

static struct stream *rd, *wr;

void *
detailer(void *arg)
{
	struct config *config;
	struct coll *coll;
	struct status *st;
	struct fixup *fixup;
	char *errmsg;
	char *cmd, *collname, *line, *release;
	int error, fixupseof;

	config = arg;
	rd = stream_open(config->chan0,
	    (stream_readfn_t *)chan_read, NULL, NULL);
	wr = stream_open(config->chan1,
	    NULL, (stream_writefn_t *)chan_write, NULL);
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		line = stream_getln(rd, NULL);
		cmd = proto_get_ascii(&line);
		collname = proto_get_ascii(&line);
		release = proto_get_ascii(&line);
		error = proto_get_time(&line, &coll->co_scantime);
		if (error || line != NULL || strcmp(cmd, "COLL") != 0 ||
		    strcmp(collname, coll->co_name) != 0 ||
		    strcmp(release, coll->co_release) != 0)
			goto bad;
		proto_printf(wr, "COLL %s %s\n", coll->co_name,
		    coll->co_release);
		stream_flush(wr);
		if (coll->co_options & CO_COMPRESS) {
			stream_filter_start(rd, STREAM_FILTER_ZLIB, NULL);
			stream_filter_start(wr, STREAM_FILTER_ZLIB, NULL);
		}
		st = status_open(coll, -1, &errmsg);
		if (st == NULL) {
			lprintf(-1, "Detailer: %s\n", errmsg);
			stream_close(rd);
			stream_close(wr);
			return (NULL);
		}
		error = detailer_coll(coll, st);
		status_close(st, NULL);
		if (error)
			return (NULL);
		if (coll->co_options & CO_COMPRESS) {
			stream_filter_stop(rd);
			stream_filter_stop(wr);
		}
		stream_flush(wr);
	}
	line = stream_getln(rd, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	proto_printf(wr, ".\n");
	stream_flush(wr);

	/* Now send fixups if needed. */
	fixup = NULL;
	fixupseof = 0;
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		proto_printf(wr, "COLL %s %s\n", coll->co_name,
		    coll->co_release);
		if (coll->co_options & CO_COMPRESS)
			stream_filter_start(wr, STREAM_FILTER_ZLIB, NULL);
		while (!fixupseof) {
			if (fixup == NULL)
				fixup = fixups_get(config->fixups);
			if (fixup == NULL) {
				fixupseof = 1;
				break;
			}
			if (fixup->f_coll != coll)
				break;
			proto_printf(wr, "Y %s %s %s\n", fixup->f_name,
			    coll->co_tag, coll->co_date);
			fixup = NULL;
		}
		proto_printf(wr, ".\n");
		if (coll->co_options & CO_COMPRESS)
			stream_filter_stop(wr);
		stream_flush(wr);
	}
	proto_printf(wr, ".\n");
	stream_close(wr);
	stream_close(rd);
	return (NULL);
bad:
	lprintf(-1, "Detailer: Protocol error\n");
	stream_close(wr);
	stream_close(rd);
	return (NULL);
}

static int
detailer_coll(struct coll *coll, struct status *st)
{
	char *cmd, *file, *line, *msg;
	int error;

	line = stream_getln(rd, NULL);
	if (line == NULL)
		return (-1);
	while (strcmp(line, ".") != 0) {
		cmd = proto_get_ascii(&line);
		if (cmd == NULL || strlen(cmd) != 1)
			return (-1);
		switch (cmd[0]) {
		case 'D':
			/* Delete file. */
			file = proto_get_ascii(&line);
			if (file == NULL || line != NULL)
				return (-1);
			proto_printf(wr, "D %s\n", file);
			break;
		case 'U':
			/* Add or update file. */
			file = proto_get_ascii(&line);
			if (file == NULL || line != NULL)
				return (-1);
			error = detailer_dofile(coll, st, file);
			if (error)
				return (-1);
			break;
		case '!':
			/* Warning from server. */
			msg = proto_get_rest(&line);
			if (msg == NULL)
				return (-1);
			lprintf(-1, "Server warning: %s\n", msg);
			break;
		default:
			if (line == NULL)
				lprintf(-1, "Bad command from server: %s\n",
				    cmd);
			else
				lprintf(-1, "Bad command from server: %s %s\n",
				    cmd, line);
			return (-1);
		}
		stream_flush(wr);
		line = stream_getln(rd, NULL);
		if (line == NULL)
			return (-1);
	}
	proto_printf(wr, ".\n");
	return (0);
}

static int
detailer_dofile(struct coll *coll, struct status *st, char *file)
{
	char md5[MD5_DIGEST_SIZE];
	struct fattr *fa;
	struct statusrec *sr;
	char *path;
	int error, ret;

	path = checkoutpath(coll->co_prefix, file);
	if (path == NULL)
		return (-1);
	fa = fattr_frompath(path, FATTR_NOFOLLOW);
	if (fa == NULL) {
		/* We don't have the file, so the only option at this
		   point is to tell the server to send it.  The server
		   may figure out that the file is dead, in which case
		   it will tell us. */
		proto_printf(wr, "C %s %s %s\n",
		    file, coll->co_tag, coll->co_date);
		free(path);
		return (0);
	}
	ret = status_get(st, file, 0, 0, &sr);
	if (ret == -1) {
		lprintf(-1, "Detailer: %s\n", status_errmsg(st));
		free(path);
		return (-1);
	}
	if (ret == 0)
		sr = NULL;

	/* If our recorded information doesn't match the file that the
	   client has, then ignore the recorded information. */
	if (sr != NULL && (sr->sr_type != SR_CHECKOUTLIVE ||
	    !fattr_equal(sr->sr_clientattr, fa)))
		sr = NULL;
	fattr_free(fa);
	if (sr != NULL && strcmp(sr->sr_revdate, ".") != 0) {
		proto_printf(wr, "U %s %s %s %s %s\n", file, coll->co_tag,
		    coll->co_date, sr->sr_revnum, sr->sr_revdate);
		free(path);
		return (0);
	}

	error = MD5_File(path, md5);
	if (error)
		return (-1);
	free(path);
	if (sr == NULL) {
		proto_printf(wr, "S %s %s %s %s\n", file, coll->co_tag,
		    coll->co_date, md5);
	} else {
		proto_printf(wr, "s %s %s %s %s %s\n", file, coll->co_tag,
		    coll->co_date, sr->sr_revnum, md5);
	}
	return (0);
}
