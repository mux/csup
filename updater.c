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

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "updater.h"
#include "misc.h"
#include "mux.h"
#include "stream.h"

static int	updater_checkfile(char *);
static int	updater_makedirs(char *);
static void	updater_prunedirs(char *, char *);
static int	updater_checkout(struct stream *, char *);
static int	updater_delete(struct collection *, char *);
static int	updater_diff(struct collection *, struct stream *, char *);
static int	updater_diff_apply(struct collection *, char *,
    		    struct stream *);

void *
updater(void *arg)
{
	struct config *config;
	struct collection *cur;
	struct stream *rd;
	char *line, *cmd, *coll, *release;
	int error;

	config = arg;
	rd = config->chan1;
	error = 0;
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (cur->options & CO_SKIP)
			continue;
		chdir(cur->base);
		umask(cur->umask);
		line = stream_getln(rd);
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
			line = stream_getln(rd);
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
				error = updater_diff(cur, rd, line);
			else if (strcmp(cmd, "u") == 0)
				error = updater_delete(cur, line);
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
updater_delete(struct collection *collec, char *line)
{
	char *cp, *file;
	int error;

	file = strsep(&line, " ");
	if (file == NULL || updater_checkfile(file) != 0)
		return (-1);
	cp = strstr(file, ",v");
	if (cp == NULL || cp[2] != '\0')
		return (-1);
	*cp = '\0';
	lprintf(1, " Delete %s\n", file);
	error = unlink(file);
	if (error)
		return (error);
	updater_prunedirs(collec->base, file);
	return (0);
}

static int
updater_diff(struct collection *cur, struct stream *rd, char *line)
{
	char pathbuf[PATH_MAX];
	char *cp, *tok, *file, *revnum, *revdate, *author;
	int error;

	file = strsep(&line, " ");
	if (file == NULL || updater_checkfile(file) != 0)
		return (-1);
	cp = strstr(file, ",v");
	if (cp == NULL || cp[2] != '\0')
		return (-1);
	/*
	 * We need to copy the filename because we'll need it later and
	 * the pointer is only valid until the next stream_getln() call.
	 */
	strlcpy(pathbuf, file, min(sizeof(pathbuf), (unsigned)(cp - file + 1)));
	file = pathbuf;
	lprintf(1, " Edit %s\n", file);
	for (;;) {
		line = stream_getln(rd);
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
				line = stream_getln(rd);
				if (line == NULL)
					return (-1);
				if (strcmp(line, ".") == 0)
					break;
				tok = strsep(&line, " ");
				if (strcmp(tok, "T") == 0) {
					error = updater_diff_apply(cur, file,
					    rd);
					if (error)
						return (-1);
				} else if (strcmp(tok, "L") == 0) {
					line = stream_getln(rd);
					while (line != NULL &&
					    strcmp(line, ".") != 0 &&
					    strcmp(line, ".+") != 0)
						line = stream_getln(rd);
					if (line == NULL)
						return (-1);
				}
			}
		}
	}
	return (0);
}

static int
updater_diff_apply(struct collection *cur, char *file, struct stream *rd)
{
	char tmp[PATH_MAX];
	intmax_t at, count, i, n;
	FILE *to;
	struct stream *orig;
	char *end, *line;
	int fd, error;
	char cmd, last;

	/* XXX */
	snprintf(tmp, sizeof(tmp), "%s/%s/%s", cur->base, dirname(file),
	    "#cvs.csup-XXXXX");
	/*
	 * XXX - CVSup creates the temporary file in the same directory
	 * as the updated file and it's of the form "#cvs.cvsup-XXXXX.X".
	 * We should change this to behave the same.
	 */
	fd = mkstemp(tmp);
	if (fd == -1)
		return (-1);
	to = fdopen(fd, "w");
	if (to == NULL) {
		close(fd);
		return (-1);
	}

	fd = open(file, O_RDONLY);
	if (fd == -1) {
		fclose(to);
		return (-1);
	}
	orig = stream_open(fd, read, write, close);
	if (orig == NULL) {
		close(fd);
		fclose(to);
		return (-1);
	}

	last = 'a';
	n = 0;
	line = stream_getln(rd);
	/* XXX - Handle .+ termination properly. */
	while (line != NULL && strcmp(line, ".") != 0 &&
	    strcmp(line, ".+") != 0) {
		printf("%s\n", line);
		cmd = line[0];
		if (cmd != 'a' && cmd != 'd')
			goto bad;
		errno = 0;
		at = strtoimax(line + 1, &end, 10);
		if (errno || at < 0 || *end != ' ') {
			printf("kaboom (3)\n");
			goto bad;
		}
		line = end + 1;
		errno = 0;
		count = strtoimax(line, &end, 10);
		if (errno || count <= 0 || *end != '\0') {
			printf("kaboom (3)\n");
			goto bad;
		}
		if (cmd == 'a' && last == 'a')
			at += 1;
		while (n < at - 1) {
			line = stream_getln(orig);
			n++;
			fprintf(to, "%s\n", line);
		}
		if (cmd == 'a') {
			for (i = 0; i < count; i++) {
				line = stream_getln(rd);
				if (line == NULL) {
					printf("kaboom\n");
					goto bad;
				}
				if (line[0] == '.')
					line++;
				fprintf(to, "%s\n", line);
			}
		} else if (cmd == 'd') {
			while (count-- > 0) {
				line = stream_getln(orig);
				n++;
			}
		}
		line = stream_getln(rd);
		last = cmd;
	}
	if (line == NULL)
		goto bad;
	while ((line = stream_getln(orig)) != NULL)
		fprintf(to, "%s\n", line);
	fclose(to);
	stream_close(orig);
	error = rename(tmp, file);
	if (error) {
		warn("rename");
		return (-1);
	}
	return (0);
bad:
	fclose(to);
	stream_close(orig);
	return (-1);
}

static int
updater_checkout(struct stream *rd, char *line)
{
	char pathbuf[PATH_MAX];
	char md5[MD5_DIGEST_LEN + 1];
	char *cp, *cmd, *file, *cksum;
	FILE *to;
	int error, first;

	file = strsep(&line, " ");
	if (file == NULL || updater_checkfile(file) != 0)
		return (-1);
	cp = strstr(file, ",v");
	if (cp == NULL || cp[2] != '\0')
		return (-1);
	/*
	 * We need to copy the filename because we'll need it later and
	 * the pointer is only valid until the next stream_getln() call.
	 */
	strlcpy(pathbuf, file, min(sizeof(pathbuf), (unsigned)(cp - file + 1)));
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
	line = stream_getln(rd);
	first = 1;
	while (line != NULL && strcmp(line, ".") != 0 &&
	    strcmp(line, ".+") != 0) {
		if (strncmp(line, "..", 2) == 0)
			line++;
		if (!first)
			fprintf(to, "\n");
		fprintf(to, "%s", line);
		line = stream_getln(rd);
		first = 0;
	}
	if (line == NULL) {
		fclose(to);
		return (-1);
	}
	if (strcmp(line, ".+") != 0)
		fprintf(to, "\n");
	fsync(fileno(to));
	fclose(to);
	/* Get the checksum line. */
	line = stream_getln(rd);
	cmd = strsep(&line, " ");
	cksum = strsep(&line, " ");
	if (cmd == NULL || cksum == NULL || line != NULL ||
	    strcmp(cmd, "5") != 0)
		return (-1);
	if (MD5file(file, md5) == -1 || strcmp(cksum, md5) != 0)
		return (-1);
	return (0);
}

static int
updater_checkfile(char *path)
{
	char *cp;

	if (*path == '/')
		return (-1);
	while ((cp = strstr(path, "..")) != NULL) {
		if (cp == path || cp[2] == '\0' ||
		    (cp[-1] == '/' && cp[2] == '/'))
			return (-1);
		path = cp + 2;
	}
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

/*
 * Remove all empty directories until base.
 * This function will trash the path passed to it.
 */
static void
updater_prunedirs(char *base, char *path)
{
	char *cp;
	int error;

	while ((cp = strrchr(path, '/')) != NULL) {
		*cp = '\0';
		if (strcmp(path, base) == 0)
			return;
		error = rmdir(path);
		/*
		 * XXX - we should probably check that errno is ENOTEMPTY
		 * here but it seems CVSup just stops at the first error.
		 * This needs to be double checked.
		 */
		if (error)
			return;
	}
}
