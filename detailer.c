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

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <md5.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "detailer.h"
#include "mux.h"

#define	MD5_DIGEST_LEN	32

#define	LINE_MAX	4096

void *
detailer(void *arg)
{
	char buf[LINE_MAX];
	struct stat sb;
	struct config *config;
	struct collection *cur;
	char *tok, *line, *md5;
	size_t in, off;
	int rdchan, wrchan, error;

	config = arg;
	rdchan = config->chan0;
	wrchan = config->chan1;
	in = off = 0;
	chdir("/usr");
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		line = chan_getln(rdchan, buf, sizeof(buf), &in, &off);
		tok = strsep(&line, " ");
		assert(strcmp(tok, "COLL") == 0);
		tok = strsep(&line, " ");
		assert(strcmp(tok, cur->name) == 0);
		assert(line != NULL);
		chan_printf(wrchan, "COLL %s %s\n", cur->name, cur->release);
		for (;;) {
			line = chan_getln(rdchan, buf, sizeof(buf), &in, &off);
			if (strcmp(line, ".") == 0)
				break;
			assert(line != NULL);
			tok = strsep(&line, " ");
			assert(strcmp(tok, "U") == 0);
			tok = strsep(&line, " ");
			tok[strlen(tok) - 2] = '\0';
			error = stat(tok, &sb);
			if (!error) {
				md5 = MD5File(tok, NULL);
				chan_printf(wrchan, "S %s,v %s %s %s\n", tok,
				    cur->tag, cur->date, md5);
				free(md5);
			} else
				chan_printf(wrchan, "C %s,v %s %s\n", tok,
				    cur->tag, cur->date);
		}
		chan_printf(wrchan, ".\n");
	}
	line = chan_getln(rdchan, buf, sizeof(buf), &in, &off);
	assert(strcmp(line, ".") == 0);
	chan_printf(wrchan, ".\n");
	return (NULL);
}
