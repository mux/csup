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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/queue.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "keyword.h"
#include "stream.h"

static void	diff_writeln(struct keyword *, struct diff *, FILE *, char *);

int
diff_apply(struct diff *diff, struct keyword *keyword, FILE *to)
{
	intmax_t at, count, i, n;
	char *end, *line;
	char cmd, last;


	last = 'a';
	n = 0;
	line = stream_getln(diff->d_diff, NULL);
	/* XXX - Handle .+ termination properly. */
	while (line != NULL && strcmp(line, ".") != 0 &&
	    strcmp(line, ".+") != 0) {
		/*
		 * The server sends an empty line and then terminates
		 * with .+ for forced (and thus empty) commits.  We
		 * just ignore empty lines for now.
		 */
		if (*line == '\0') {
			line = stream_getln(diff->d_diff, NULL);
			continue;
		}
		cmd = line[0];
		if (cmd != 'a' && cmd != 'd') {
			printf("bad command (%s)\n", line);
			return (-1);
		}
		errno = 0;
		at = strtoimax(line + 1, &end, 10);
		if (errno || at < 0 || *end != ' ') {
			printf("kaboom\n");
			return (-1);
		}
		line = end + 1;
		errno = 0;
		count = strtoimax(line, &end, 10);
		if (errno || count <= 0 || *end != '\0') {
			printf("kaboom (2)\n");
			return (-1);
		}
		if (cmd == 'a' && last == 'a')
			at += 1;
		while (n < at - 1) {
			line = stream_getln(diff->d_orig, NULL);
			if (line == NULL) {
				printf("boom");
				return (-1);
			}
			n++;
			diff_writeln(keyword, diff, to, line);
		}
		if (cmd == 'a') {
			for (i = 0; i < count; i++) {
				line = stream_getln(diff->d_diff, NULL);
				if (line == NULL) {
					printf("boom\n");
					return (-1);
				}
				if (line[0] == '.')
					line++;
				diff_writeln(keyword, diff, to, line);
			}
		} else if (cmd == 'd') {
			while (count-- > 0) {
				line = stream_getln(diff->d_orig, NULL);
				if (line == NULL)
					printf("aie\n");
				n++;
			}
		}
		line = stream_getln(diff->d_diff, NULL);
		last = cmd;
	}
	if (line == NULL) {
		printf("line = NULL\n");
		return (-1);
	}
	/* XXX - Should probably be done in a more efficient manner. */
	while ((line = stream_getln(diff->d_orig, NULL)) != NULL)
		diff_writeln(keyword, diff, to, line);
	return (0);
}

static void
diff_writeln(struct keyword *keyword, struct diff *diff, FILE *to, char *line)
{
	char *newline;

	newline = keyword_expand(keyword, diff, line);
	fprintf(to, "%s\n", newline);
	if (newline != line)
		free(newline);
}
