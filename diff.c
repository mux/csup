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
#include <sys/queue.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "keyword.h"
#include "stream.h"

/* Only use intmax_t and strtoimax() if C99 is supported. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901
#define	strtoimax	strtol

typedef long lineno_t;
#else
#include <inttypes.h>
#include <stdint.h>

typedef intmax_t lineno_t;
#endif

#define	EC_ADD	0
#define	EC_DEL	1

/* Editing command and state. */
struct editcmd {
	int cmd;
	lineno_t where;
	lineno_t count;
	lineno_t lasta;
	lineno_t lastd;
	lineno_t editline;
	/* Those are here for convenience. */
	struct diff *diff;
	struct keyword *keyword;
	FILE *to;
};

static int	diff_geteditcmd(struct editcmd *, char *);
static int	diff_copyln(struct editcmd *, lineno_t);
static void	diff_writeln(struct editcmd *, char *);
/* XXX Should be elsewhere. */
static int	diff_shrink(FILE *);

int
diff_apply(struct diff *diff, struct keyword *keyword, FILE *to)
{
	struct editcmd ec;
	lineno_t i;
	char *line;
	int empty, error, noeol;

	memset(&ec, 0, sizeof(ec));
	empty = 0;
	noeol = 0;
	ec.diff = diff;
	ec.keyword = keyword;
	ec.to = to;
	line = stream_getln(diff->d_diff, NULL);
	while (line != NULL && strcmp(line, ".") != 0 &&
	    strcmp(line, ".+") != 0) {
		/*
		 * The server sends an empty line and then terminates
		 * with .+ for forced (and thus empty) commits.
		 */
		if (*line == '\0') {
			if (empty)
				return (-1);
			empty = 1;
			line = stream_getln(diff->d_diff, NULL);
			continue;
		}
		error = diff_geteditcmd(&ec, line);
		if (error) {
			printf("%s: diff_geteditcmd() failed\n", __func__);
			return (-1);
		}

		if (ec.cmd == EC_ADD) {
			error = diff_copyln(&ec, ec.where);
			if (error) {
				printf("%s: diff_copyln() failed\n", __func__);
				return (-1);
			}
			for (i = 0; i < ec.count; i++) {
				line = stream_getln(diff->d_diff, NULL);
				if (line == NULL) {
					printf("%s: boom\n", __func__);
					return (-1);
				}
				if (line[0] == '.')
					line++;
				diff_writeln(&ec, line);
			}
		} else {
			assert(ec.cmd == EC_DEL);
			error = diff_copyln(&ec, ec.where - 1);
			if (error) {
				printf("%s: diff_copyln() failed\n", __func__);
				return (-1);
			}
			for (i = 0; i < ec.count; i++) {
				line = stream_getln(diff->d_orig, NULL);
				if (line == NULL) {
					printf("%s: aie\n", __func__);
					return (-1);
				}
				ec.editline++;
			}
		}
		line = stream_getln(diff->d_diff, NULL);
	}
	if (line == NULL) {
		printf("%s: line == NULL\n", __func__);
		return (-1);
	}
	/* If we got ".+", there's no ending newline. */
	if (strcmp(line, ".+") == 0 && !empty)
		noeol = 1;
	ec.where = 0;
	while ((line = stream_getln(diff->d_orig, NULL)) != NULL)
		diff_writeln(&ec, line);
	fflush(to);
	if (noeol) {
		error = diff_shrink(to);
		if (error) {
			printf("%s: diff_shrink() failed\n", __func__);
			return (-1);
		}
	}
	return (0);
}

/* Get an editing command from the diff. */
static int
diff_geteditcmd(struct editcmd *ec, char *line)
{
	char *end;

	if (line[0] == 'a')
		ec->cmd = EC_ADD;
	else if (line[0] == 'd')
		ec->cmd = EC_DEL;
	else {
		printf("%s: Bad editing command from server (%s)\n",
		    __func__, line);
		return (-1);
	}
	errno = 0;
	ec->where = strtoimax(line + 1, &end, 10);
	if (errno || ec->where < 0 || *end != ' ') {
		printf("kaboom\n");
		return (-1);
	}
	line = end + 1;
	errno = 0;
	ec->count = strtoimax(line, &end, 10);
	if (errno || ec->count <= 0 || *end != '\0') {
		printf("kaboom (2)\n");
		return (-1);
	}
	if (ec->cmd == EC_ADD) {
		if (ec->where < ec->lasta)
			return (-1);
		ec->lasta = ec->where + 1;
	} else {
		if (ec->where < ec->lasta || ec->where < ec->lastd)
			return (-1);
		ec->lasta = ec->where;
		ec->lastd = ec->where + ec->count;
	}
	return (0);
}

/* Copy lines from the original version of the file up to line "to". */
static int
diff_copyln(struct editcmd *ec, lineno_t to)
{
	char *line;

	while (ec->editline < to) {
		line = stream_getln(ec->diff->d_orig, NULL);
		if (line == NULL)
			return (-1);
		ec->editline++;
		diff_writeln(ec, line);
	}
	return (0);
}

/* Write a new line to the file, expanding RCS keywords. */
static void
diff_writeln(struct editcmd *ec, char *line)
{
	char *newline;

	newline = keyword_expand(ec->keyword, ec->diff, line);
	fprintf(ec->to, "%s\n", newline);
	if (newline != line)
		free(newline);
}

/*
 * Shrink the file by one byte to get rid of the ending newline.
 * This will only work if the buffer is properly flushed before.
 */
static int
diff_shrink(FILE *f)
{
	struct stat sb;
	off_t newsize;
	int error, fd;

	fd = fileno(f);
	error = fstat(fd, &sb);
	if (error)
		return (-1);
	newsize = sb.st_size - 1;
	error = ftruncate(fd, newsize);
	return (error);
}
