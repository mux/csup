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
#include <sys/queue.h>
#include <sys/stat.h>

#include <err.h>
#include <limits.h>
#include <md5.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "updater.h"
#include "main.h"
#include "mux.h"

static char buf[4096];
static size_t in, off;

static int	updater_checkfile(char *);
static int	updater_makedirs(char *);
static int	updater_checkout(int, char *);
static int	updater_diff(int, char *);

void *
updater(void *arg)
{
	struct config *config;
	struct collection *cur;
	char *line, *cmd, *coll, *release;
	int rd, error;

	config = arg;
	rd = config->chan1;
	in = off = 0;
	error = 0;
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		chdir(cur->base);
		umask(cur->umask);
		line = chan_getln(rd, buf, sizeof(buf), &in, &off);
		cmd = strsep(&line, " ");
		coll = strsep(&line, " ");
		release = strsep(&line, " ");
		if (cmd == NULL || coll == NULL || release == NULL ||
		    line != NULL)
			goto bad;
		if (strcmp(cmd, "COLL") != 0 || strcmp(coll, cur->name) != 0 ||
		    strcmp(release, cur->release) != 0)
			goto bad;
		lprintf(1, "Updating collection %s/%s\n", cur->name,
		    cur->release);
		for (;;) {
			line = chan_getln(rd, buf, sizeof(buf), &in, &off);
			if (strcmp(line, ".") == 0)
				break;
			cmd = strsep(&line, " ");
			if (cmd == NULL)
				goto bad;
			if (strcmp(cmd, "T") == 0)
				;
			else if (strcmp(cmd, "c") == 0)
				;
			else if (strcmp(cmd, "U") == 0)
				error = updater_diff(rd, line);
			else if (strcmp(cmd, "C") == 0)
				error = updater_checkout(rd, line);
			else
				goto bad;
			if (error)
				goto bad;
		}
	}
	return (NULL);
bad:
	fprintf(stderr, "Updater: Protocol error\n");
	return (NULL);
}

static int
updater_diff(int rd, char *line)
{
	char *cp, *tok, *file, *revnum, *revdate, *author;

	file = strsep(&line, " ");
	if (file == NULL)
		return (-1);
	cp = strstr(file, ",v");
	if (cp == NULL || cp[2] != '\0')
		return (-1);
	*cp = '\0';
	lprintf(1, " Edit %s\n", file);
	for (;;) {
		line = chan_getln(rd, buf, sizeof(buf), &in, &off);
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (strcmp(tok, "D") == 0) {
			revnum = strsep(&line, " ");
			strsep(&line, " ");
			revdate = strsep(&line, " ");
			author = strsep(&line, " ");
			if (revnum == NULL || revdate == NULL ||
			    author == NULL || line != NULL)
				return (-1);
			lprintf(2, "  Add delta %s %s %s\n", revnum, revdate,
			    author);
			for (;;) {
				line = chan_getln(rd, buf, sizeof(buf),
				    &in, &off);
				if (strcmp(line, ".") == 0)
					break;
				tok = strsep(&line, " ");
				if (strcmp(tok, "T") == 0 ||
				    strcmp(tok, "L") == 0) {
					for (;;) {
						line = chan_getln(rd, buf,
						    sizeof(buf), &in, &off);
						if (strcmp(line, ".") == 0)
							break;
					}
				}
			}
		}
	}
	return (0);
}

static int
updater_checkout(int rd, char *line)
{
	char pathbuf[PATH_MAX];
	char md5[MD5_DIGEST_LEN + 1];
	char *cp, *cmd, *file, *cksum;
	int error;
	FILE *to;

	file = strsep(&line, " ");
	if (file == NULL || updater_checkfile(file) != 0)
		return (-1);
	cp = strstr(file, ",v");
	if (cp == NULL || cp[2] != '\0')
		return (-1);
	/*
	 * We need to copy the filename because we'll need it later and
	 * the pointer is only valid until the next chan_getln() call.
	 */
	strlcpy(pathbuf, file, min(sizeof(buf), (unsigned)(cp - file + 1)));
	file = pathbuf;
	lprintf(1, " Checkout %s\n", file);
	error = updater_makedirs(file);
	if (error)
		return (-1);
	to = fopen(file, "w");
	if (to == NULL) {
		warn("fopen");
		return (-1);
	}
	while (strcmp(line = chan_getln(rd, buf, sizeof(buf), &in, &off),
	    ".") != 0) {
		if (line == NULL) {
			fclose(to);
			return (-1);
		}
		if (strncmp(line, "..", 2) == 0)
			line++;
		fprintf(to, "%s\n", line);
	}
	fsync(fileno(to));
	fclose(to);
	/* Get the checksum line. */
	line = chan_getln(rd, buf, sizeof(buf), &in, &off);
	cmd = strsep(&line, " ");
	cksum = strsep(&line, " ");
	if (cmd == NULL || cksum == NULL || line != NULL ||
	    strcmp(cmd, "5") != 0) {
		printf("foo\n");
		return (-1);
	}
	if (MD5File(file, md5) == NULL || strcmp(cksum, md5) != 0) {
		printf("bar");
		return (-1);
	}
	return (0);
}

static int
updater_checkfile(char *path)
{

	if (*path == '/' || strstr(path, "..") != NULL)
		return (-1);
	return (0);
}

static int
updater_makedirs(char *path)
{
	char *cp, *comp;
	int error;

	comp = path;
	while ((cp = strchr(comp, '/')) != NULL) {
		if (cp != comp) {
			*cp = '\0';
			if (access(path, F_OK) != 0) {
				error = mkdir(path, S_IRWXU | S_IRWXG |
				    S_IRWXO);
				if (error) {
					*cp = '/';
					return (-1);
				}
			}
			*cp = '/';
		}
		comp = cp + 1;
	}
	return (0);
}