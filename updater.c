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
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "diff.h"
#include "fileattr.h"
#include "keyword.h"
#include "updater.h"
#include "misc.h"
#include "mux.h"
#include "stream.h"

static char	*updater_getpath(struct coll *, char *);
static int	 updater_makedirs(char *);
static void	 updater_prunedirs(char *, char *);
static int	 updater_checkout(struct coll *, struct stream *, char *);
static int	 updater_delete(struct coll *, char *);
static int	 updater_diff(struct coll *, struct stream *, char *);
static int	 updater_diff_apply(struct coll *, char *, struct stream *,
		     struct diff *);
static int	 updater_dodiff(struct coll *, char *, struct stream *,
		     struct diff *);

void *
updater(void *arg)
{
	struct config *config;
	struct coll *cur;
	struct stream *rd;
	char *line, *cmd, *coll, *release;
	int error;

	config = arg;
	rd = config->chan1;
	error = 0;
	STAILQ_FOREACH(cur, &config->colls, next) {
		if (cur->options & CO_SKIP)
			continue;
		umask(cur->umask);
		line = stream_getln(rd, NULL);
		cmd = strsep(&line, " ");
		coll = strsep(&line, " ");
		release = strsep(&line, " ");
		if (release == NULL || line != NULL)
			goto bad;
		if (strcmp(cmd, "COLL") != 0 || strcmp(coll, cur->name) != 0 ||
		    strcmp(release, cur->release) != 0)
			goto bad;
		lprintf(1, "Updating collection %s/%s\n", cur->name,
		    cur->release);
			
		while ((line = stream_getln(rd, NULL)) != NULL) {
			if (strcmp(line, ".") == 0)
				break;
			cmd = strsep(&line, " ");
			if (cmd == NULL)
				goto bad;
			if (strcmp(cmd, "T") == 0)
				/* XXX */;
			else if (strcmp(cmd, "c") == 0)
				/* XXX */;
			else if (strcmp(cmd, "U") == 0)
				error = updater_diff(cur, rd, line);
			else if (strcmp(cmd, "u") == 0)
				error = updater_delete(cur, line);
			else if (strcmp(cmd, "C") == 0)
				error = updater_checkout(cur, rd, line);
			else
				goto bad;
			if (error)
				goto bad;
		}
		if (line == NULL)
			goto bad;
	}
	return (NULL);
bad:
	fprintf(stderr, "Updater: error\n");
	return (NULL);
}

static int
updater_delete(struct coll *coll, char *line)
{
	char *file, *rcsfile;
	int error;

	rcsfile = strsep(&line, " ");
	file = updater_getpath(coll, rcsfile);
	if (file == NULL) {
		printf("Updater: bad filename %s\n", rcsfile);
		return (-1);
	}
	lprintf(1, " Delete %s\n", file + strlen(coll->base) + 1);
	error = unlink(file);
	if (error) {
		free(file);
		return (error);
	}
	updater_prunedirs(coll->base, file);
	free(file);
	return (0);
}

static int
updater_diff(struct coll *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	struct diff diff;
	struct fattr *fa;
	char *author, *tag, *rcsfile, *revnum, *revdate;
	char *attr, *cp, *cksum, *line2, *line3, *tok, *path;
	int error;

	path = NULL;
	line3 = NULL;
	memset(&diff, 0, sizeof(struct diff));

	line2 = strdup(line);
	if (line2 == NULL)
		err(1, "strdup");
	cp = line2;
	rcsfile = strsep(&cp, " ");
	tag = strsep(&cp, " ");
	strsep(&cp, " "); /* XXX - date */
	strsep(&cp, " "); /* XXX - orig revnum */
	strsep(&cp, " "); /* XXX - from attic */
	strsep(&cp, " "); /* XXX - loglines */
	strsep(&cp, " "); /* XXX - expand */
	attr = strsep(&cp, " ");
	fa = fattr_parse(attr);
	if (fa == NULL)
		errx(1, "fattr_parse failed");
#if 0
	fattr_print(fa);
#endif
	fattr_free(fa);
	cksum = strsep(&cp, " ");
	if (cksum == NULL || cp != NULL)
		goto bad;
	diff.d_rcsfile = rcsfile;
	if (strcmp(tag, ".") != 0)
		diff.d_tag = tag;

	path = updater_getpath(coll, rcsfile);
	if (path == NULL) {
		printf("%s: bad filename %s\n", __func__, rcsfile);
		goto bad;
	}

	lprintf(1, " Edit %s\n", path + strlen(coll->base) + 1);
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (strcmp(tok, "D") != 0)
			goto bad;
		line3 = strdup(line);
		if (line3 == NULL)
			err(1, "strdup");
		cp = line3;
		revnum = strsep(&cp, " ");
		strsep(&cp, " "); /* XXX - diffbase */
		revdate = strsep(&cp, " ");
		author = strsep(&cp, " ");
		if (author == NULL || cp != NULL)
			goto bad;
		diff.d_cvsroot = coll->cvsroot;
		diff.d_revnum = revnum;
		diff.d_revdate = revdate;
		diff.d_author = author;
		lprintf(2, "  Add diff %s %s %s\n", revnum, revdate, author);
		error = updater_diff_apply(coll, path, rd, &diff);
		if (error) {
			printf("%s: updater_diff_apply failed\n", __func__);
			goto bad;
		}
	}
	if (line == NULL)
		goto bad;
	if (MD5file(path, md5) == -1) {
		printf("%s: MD5file() failed\n", __func__);
		goto bad;
	}
	if (strcmp(cksum, md5) != 0) {
		printf("%s: bad md5 checksum\n", __func__);
		goto bad;
	}
	free(path);
	free(line3);
	free(line2);
	return (0);
bad:
	free(path);
	free(line3);
	free(line2);
	return (-1);
}

static int
updater_diff_apply(struct coll *coll, char *path, struct stream *rd,
    struct diff *diff)
{
	char *tok, *line;
	int error;

	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (tok == NULL)
			return (-1);
		if (strcmp(tok, "L") == 0) {
			line = stream_getln(rd, NULL);
			/* We're just eating the log for now. */
			while (line != NULL && strcmp(line, ".") != 0 &&
			    strcmp(line, ".+") != 0)
				line = stream_getln(rd, NULL);
			if (line == NULL)
				return (-1);
		} else if (strcmp(tok, "S") == 0) {
			tok = strsep(&line, " ");
			if (tok == NULL || line != NULL)
				return (-1);
			diff->d_state = strdup(tok);
			if (diff->d_state == NULL)
				err(1, "strdup");
		} else if (strcmp(tok, "T") == 0) {
			error = updater_dodiff(coll, path, rd, diff);
			if (error) {
				printf("%s: updater_dodiff failed\n", __func__);
				return (-1);
			}
		}
	}
	if (line == NULL)
		return (-1);
	return (0);
}

int
updater_dodiff(struct coll *coll, char *path, struct stream *rd,
    struct diff *diff)
{
	struct stream *orig, *to;
	char *cp, *tmp;
	int error;

	cp = strrchr(path, '/');
	assert(cp != NULL);
	asprintf(&tmp, "%.*s/#cvs.csup-%ld", (int)(cp - path), path,
	    (long)getpid());
	if (tmp == NULL)
		err(1, "asprintf");
	/*
	 * XXX - the mode parameter should be what's the server sends us
	 * merged with the default mode for files (0666).
	 */
	to = stream_open_file(tmp, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (to == NULL) {
		warn("stream_open_file(\"%s\")", tmp);
		free(tmp);
		return (-1);
	}

	orig = stream_open_file(path, O_RDONLY);
	if (orig == NULL) {
		warn("stream_open_file(\"%s\")", path);
		stream_close(to);
		free(tmp);
		return (-1);
	}
	diff->d_orig = orig;
	diff->d_to = to;
	diff->d_diff = rd;
	error = diff_apply(diff, coll->keyword);
	stream_close(orig);
	stream_close(to);
	if (error) {
		printf("%s: diff_apply failed\n", __func__);
		free(tmp);
		return (-1);
	}
	error = rename(tmp, path);
	if (error) {
		warn("rename(\"%s\", \"%s\")", tmp, path);
		free(tmp);
		return (-1);
	}
	free(tmp);
	return (0);
}

static int
updater_checkout(struct coll *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	char *cksum, *cmd, *file, *rcsfile;
	struct stream *to;
	size_t size;
	int error, first;

	rcsfile = strsep(&line, " ");
	file = updater_getpath(coll, rcsfile);
	if (file == NULL) {
		printf("Updater: bad filename %s\n", rcsfile);
		return (-1);
	}

	lprintf(1, " Checkout %s\n", file + strlen(coll->base) + 1);
	error = updater_makedirs(file);
	if (error) {
		free(file);
		printf("Updater: updater_makedirs() failed\n");
		return (-1);
	}
	/* XXX Use correct permissions. */
	to = stream_open_file(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (to == NULL) {
		warn("stream_open_file(\"%s\")", file);
		free(file);
		return (-1);
	}
	line = stream_getln(rd, &size);
	first = 1;
	while (line != NULL) {
	       	if (size > 0 && (memcmp(line, ".", size) == 0 ||
		    memcmp(line, ".+", size) == 0))
			break;
		if (size >= 2 && memcmp(line, "..", 2) == 0) {
			size--;
			line++;
		}
		if (!first)
			stream_printf(to, "\n");
		stream_write(to, line, size);
		line = stream_getln(rd, &size);
		first = 0;
	}
	if (line == NULL) {
		stream_close(to);
		free(file);
		return (-1);
	}
	if (memcmp(line, ".", size) == 0)
		stream_printf(to, "\n");
	stream_close(to);
	/* Get the checksum line. */
	line = stream_getln(rd, NULL);
	cmd = strsep(&line, " ");
	cksum = strsep(&line, " ");
	if (cmd == NULL || cksum == NULL || line != NULL ||
	    strcmp(cmd, "5") != 0) {
		free(file);
		return (-1);
	}
	if (MD5file(file, md5) == -1 || strcmp(cksum, md5) != 0) {
		printf("%s: bad md5 checksum\n", __func__);
		free(file);
		return (-1);
	}
	free(file);
	return (0);
}

/*
 * Extract an absolute pathname from the RCS filename and
 * make sure the server isn't trying to make us change
 * unrelated files for security reasons.
 */
static char *
updater_getpath(struct coll *coll, char *rcsfile)
{
	char *cp, *path;

	if (*rcsfile == '/')
		return (NULL);
	cp = rcsfile;
	while ((cp = strstr(cp, "..")) != NULL) {
		if (cp == rcsfile || cp[2] == '\0' ||
		    (cp[-1] == '/' && cp[2] == '/'))
			return (NULL);
		cp += 2;
	}
	cp = strstr(rcsfile, ",v");
	if (cp == NULL || *(cp + 2) != '\0')
		return (NULL);
	asprintf(&path, "%s/%.*s", coll->base, (int)(cp - rcsfile), rcsfile);
	if (path == NULL)
		err(1, "asprintf");
	return (path);
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
 * Remove all empty directories below file.
 * This function will trash the path passed to it.
 */
static void
updater_prunedirs(char *base, char *file)
{
	char *cp;
	int error;

	while ((cp = strrchr(file, '/')) != NULL) {
		if (strcmp(base, file) == 0)
			return;
		*cp = '\0';
		error = rmdir(file);
		if (error) {
			if (errno != ENOTEMPTY)
				err(1, "rmdir");
			return;
		}
	}
}
