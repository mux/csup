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

#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <md5.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "detailer.h"
#include "misc.h"
#include "mux.h"
#include "stream.h"

#define	LINE_MAX	4096

void *
detailer(void *arg)
{
	char md5[MD5_DIGEST_SIZE];
	struct stat sb;
	struct config *config;
	struct collection *cur;
	struct stream *rd, *wr;
	char *cmd, *coll, *file, *line, *release;
	int error;

	config = arg;
	rd = config->chan0;
	wr = config->chan1;
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		chdir(cur->base);
		line = stream_getln(rd, NULL);
		cmd = strsep(&line, " ");
		coll = strsep(&line, " ");
		release = strsep(&line, " ");
		if (cmd == NULL || coll == NULL || release == NULL ||
		    strcmp(cmd, "COLL") != 0 || strcmp(coll, cur->name) != 0 ||
		    strcmp(release, cur->release) != 0)
			goto bad;
		stream_printf(wr, "COLL %s %s\n", cur->name, cur->release);
		line = stream_getln(rd, NULL);
		if (line == NULL)
			goto bad;
		while (strcmp(line, ".") != 0) {
			cmd = strsep(&line, " ");
			file = strsep(&line, " ");
			if (cmd == NULL || file == NULL ||
			    strcmp(cmd, "U") != 0)
				goto bad;
			/* XXX */
			file[strlen(file) - 2] = '\0';
			error = stat(file, &sb);
			if (!error && MD5file(file, md5) == 0)
				stream_printf(wr, "S %s,v %s %s %s\n", file,
				    cur->tag, cur->date, md5);
			else
				stream_printf(wr, "C %s,v %s %s\n", file,
				    cur->tag, cur->date);
			stream_flush(wr);
			line = stream_getln(rd, NULL);
			if (line == NULL)
				goto bad;
		}
		stream_printf(wr, ".\n");
		stream_flush(wr);
	}
	line = stream_getln(rd, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	stream_printf(wr, ".\n");
	stream_flush(wr);
	return (NULL);
bad:
	fprintf(stderr, "Detailer: Protocol error\n");
	return (NULL);
}
