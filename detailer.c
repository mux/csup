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

#include <sys/types.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "detailer.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "stream.h"

void *
detailer(void *arg)
{
	char md5[MD5_DIGEST_SIZE];
	struct stat sb;
	struct config *config;
	struct coll *coll;
	struct stream *rd, *wr;
	char *cmd, *collname, *file, *line, *path, *release;
	int error;

	config = arg;
	rd = stream_fdopen(config->id0, chan_read, NULL, NULL);
	wr = stream_fdopen(config->id1, NULL, chan_write, NULL);
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		line = stream_getln(rd, NULL);
		cmd = proto_getstr(&line);
		collname = proto_getstr(&line);
		release = proto_getstr(&line);
		if (release == NULL || strcmp(cmd, "COLL") != 0 ||
		    strcmp(collname, coll->co_name) != 0 ||
		    strcmp(release, coll->co_release) != 0)
			goto bad;
		proto_printf(wr, "COLL %s %s\n", coll->co_name,
		    coll->co_release);
		if (coll->co_options & CO_COMPRESS) {
			stream_filter_start(rd, STREAM_FILTER_ZLIB, NULL);
			stream_filter_start(wr, STREAM_FILTER_ZLIB, NULL);
		}
		line = stream_getln(rd, NULL);
		if (line == NULL)
			goto bad;
		while (strcmp(line, ".") != 0) {
			cmd = proto_getstr(&line);
			file = proto_getstr(&line);
			if (file == NULL || strcmp(cmd, "U") != 0)
				goto bad;
			path = checkoutpath(coll->co_prefix, file);
			if (path == NULL)
				goto bad;
			error = stat(path, &sb);
			if (!error && MD5file(path, md5) == 0)
				proto_printf(wr, "S %s %s %s %s\n", file,
				    coll->co_tag, coll->co_date, md5);
			else
				proto_printf(wr, "C %s %s %s\n", file,
				    coll->co_tag, coll->co_date);
			free(path);
			stream_flush(wr);
			line = stream_getln(rd, NULL);
			if (line == NULL)
				goto bad;
		}
		proto_printf(wr, ".\n");
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
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		proto_printf(wr, "COLL %s %s\n", coll->co_name,
		    coll->co_release);
		if (coll->co_options & CO_COMPRESS)
			stream_filter_start(wr, STREAM_FILTER_ZLIB, NULL);
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
