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
#include "fattr.h"
#include "keyword.h"
#include "updater.h"
#include "misc.h"
#include "mux.h"
#include "stream.h"

/* Everything we need to update a file. */
struct update {
	struct coll *coll;
	char *author;
	char *rcsfile;
	char *revnum;
	char *revdate;
	char *state;
	char *tag;
	struct stream *from;
	struct stream *to;
	struct stream *stream;
	struct fattr *rcsattr;
	int expand;
};

static char	*updater_getpath(struct coll *, char *);
static int	 updater_makedirs(char *);
static void	 updater_prunedirs(char *, char *);
static char	*updater_maketmp(char *);
static int	 updater_checkout(struct coll *, struct stream *, char *);
static int	 updater_delete(struct coll *, char *);
static int	 updater_diff(struct coll *, struct stream *, char *);
static int	 updater_diff_batch(struct update *);
static int	 updater_diff_apply(struct update *);
static int	 updater_install(struct coll *, struct fattr *, const char *,
		     const char *);

void *
updater(void *arg)
{
	struct config *config;
	struct coll *coll;
	struct stream *rd;
	char *line, *cmd, *collname, *release;
	int error;

	config = arg;
	rd = stream_fdopen(config->id1, chan_read, NULL, NULL);
	error = 0;
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		umask(coll->co_umask);
		line = stream_getln(rd, NULL);
		cmd = strsep(&line, " ");
		collname = strsep(&line, " ");
		release = strsep(&line, " ");
		if (release == NULL || line != NULL)
			goto bad;
		if (strcmp(cmd, "COLL") != 0 ||
		    strcmp(collname, coll->co_name) != 0 ||
		    strcmp(release, coll->co_release) != 0)
			goto bad;
		lprintf(1, "Updating collection %s/%s\n", coll->co_name,
		    coll->co_release);
			
		if (coll->co_options & CO_COMPRESS)
			stream_filter(rd, SF_ZLIB);
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
				error = updater_diff(coll, rd, line);
			else if (strcmp(cmd, "u") == 0)
				error = updater_delete(coll, line);
			else if (strcmp(cmd, "C") == 0)
				error = updater_checkout(coll, rd, line);
			else
				goto bad;
			if (error)
				goto bad;
		}
		if (line == NULL)
			goto bad;
		if (coll->co_options & CO_COMPRESS)
			stream_filter(rd, SF_NONE);
	}
	line = stream_getln(rd, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	stream_close(rd);
	return (NULL);
bad:
	fprintf(stderr, "Updater: error (%s)\n", line);
	stream_close(rd);
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
		printf("Updater: bad filename \"%s\"\n", rcsfile);
		return (-1);
	}
	lprintf(1, " Delete %s\n", file + strlen(coll->co_base) + 1);
	error = unlink(file);
	if (error) {
		free(file);
		return (error);
	}
	updater_prunedirs(coll->co_base, file);
	free(file);
	return (0);
}

static int
updater_diff(struct coll *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	struct update up;
	struct fattr *rcsattr, *fa;
	char *author, *expand, *tag, *rcsfile, *revnum, *revdate, *path;
	char *attr, *cp, *cksum, *line2, *line3, *tok, *topath;
	int error;

	line3 = NULL;
	rcsattr = NULL;
	path = NULL;
	topath = NULL;
	memset(&up, 0, sizeof(struct update));
	up.coll = coll;

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
	expand = strsep(&cp, " ");
	attr = strsep(&cp, " ");
	cksum = strsep(&cp, " ");
	if (cksum == NULL || cp != NULL)
		goto bad;
	path = updater_getpath(coll, rcsfile);
	if (path == NULL) {
		fprintf(stderr, "Updater: bad filename %s\n", rcsfile);
		goto bad;
	}
	if (strcmp(expand, ".") == 0)
		up.expand = EXPAND_DEFAULT;
	else if (strcmp(expand, "kv") == 0)
		up.expand = EXPAND_KEYVALUE;
	else if (strcmp(expand, "kvl") == 0)
		up.expand = EXPAND_KEYVALUELOCKER;
	else if (strcmp(expand, "k") == 0)
		up.expand = EXPAND_KEY;
	else if (strcmp(expand, "o") == 0)
		up.expand = EXPAND_OLD;
	else if (strcmp(expand, "b") == 0)
		up.expand = EXPAND_BINARY;
	else if (strcmp(expand, "v") == 0)
		up.expand = EXPAND_VALUE;
	else {
		fprintf(stderr, "Updater: invalid expansion mode \"%s\"\n",
		    expand);
		goto bad;
	}
	rcsattr = fattr_decode(attr);
	if (rcsattr == NULL) {
		fprintf(stderr, "Updater: invalid file attributes \"%s\"\n",
		    attr);
		goto bad;
	}
	up.rcsattr = rcsattr;
	up.rcsfile = rcsfile;
	if (strcmp(tag, ".") != 0)
		up.tag = tag;

	lprintf(1, " Edit %s\n", path + strlen(coll->co_base) + 1);
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (strcmp(tok, "D") != 0)
			goto bad;
		free(line3);
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
		up.revnum = revnum;
		up.revdate = revdate;
		up.author = author;
		up.stream = rd;
		if (up.from == NULL) {
			/* First patch, the "origin" file is the one we have. */
			up.from = stream_open_file(path, O_RDONLY);
			if (up.from == NULL)
				goto bad;
		} else {
			/* Subsequent patches. */
			stream_close(up.from);
			up.from = up.to;
			stream_rewind(up.from);
			/* XXX */
			if (unlink(topath) == -1)
				warn("unlink");
			free(topath);
		}
		topath = updater_maketmp(up.rcsfile);
		if (topath == NULL)
			goto bad;
		up.to = stream_open_file(topath, O_RDWR | O_CREAT | O_EXCL,
		    0600);
		if (up.to == NULL) {
			perror("Cannot create temporary file");
			goto bad;
		}
		lprintf(2, "  Add delta %s %s %s\n", revnum, revdate, author);
		error = updater_diff_batch(&up);
		if (error) {
			printf("%s: updater_diff_batch failed\n", __func__);
			goto bad;
		}
	}
	if (line == NULL)
		goto bad;
	fa = fattr_dup(rcsattr);
	fattr_maskout(fa, FA_MODTIME);
	updater_install(coll, fa, topath, path);
	fattr_free(fa);
	/* XXX - Compute MD5 while writing the file. */
	if (MD5file(path, md5) == -1) {
		printf("%s: MD5file() failed\n", __func__);
		goto bad;
	}
	if (strcmp(cksum, md5) != 0) {
		printf("%s: bad md5 checksum\n", __func__);
		goto bad;
	}
	stream_close(up.from);
	stream_close(up.to);
	free(topath);
	fattr_free(rcsattr);
	free(path);
	free(line3);
	free(line2);
	return (0);
bad:
	stream_close(up.from);
	stream_close(up.to);
	free(topath);
	fattr_free(rcsattr);
	free(path);
	free(line3);
	free(line2);
	return (-1);
}

static int
updater_diff_batch(struct update *up)
{
	struct stream *rd;
	char *tok, *line;
	int error;

	rd = up->stream;
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (tok == NULL)
			goto bad;
		if (strcmp(tok, "L") == 0) {
			line = stream_getln(rd, NULL);
			/* XXX - We're just eating the log for now. */
			while (line != NULL && strcmp(line, ".") != 0 &&
			    strcmp(line, ".+") != 0)
				line = stream_getln(rd, NULL);
			if (line == NULL)
				goto bad;
		} else if (strcmp(tok, "S") == 0) {
			tok = strsep(&line, " ");
			if (tok == NULL || line != NULL)
				goto bad;
			free(up->state);
			up->state = strdup(tok);
			if (up->state == NULL)
				err(1, "strdup");
		} else if (strcmp(tok, "T") == 0) {
			error = updater_diff_apply(up);
			if (error)
				goto bad;
		}
	}
	if (line == NULL)
		goto bad;
	free(up->state);
	up->state = NULL;
	return (0);
bad:
	free(up->state);
	up->state = NULL;
	return (-1);
}

int
updater_diff_apply(struct update *up)
{
	struct diff diff;
	int error;

	/* XXX - This is stupid, both structs should be merged. */
	diff.d_orig = up->from;
	diff.d_to = up->to;
	diff.d_diff = up->stream;
	diff.d_author = up->author;
	diff.d_cvsroot = up->coll->co_cvsroot;
	diff.d_rcsfile = up->rcsfile;
	diff.d_revnum = up->revnum;
	diff.d_revdate = up->revdate;
	diff.d_state = up->state;
	diff.d_tag = up->tag;
	diff.d_expand = up->expand;
	error = diff_apply(&diff, up->coll->co_keyword);
	if (error) {
		fprintf(stderr, "Updater: bad diff from server\n");
		return (-1);
	}
	return (0);
}

static int
updater_checkout(struct coll *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	struct fattr *fa;
	struct stream *to;
	char *attr, *cksum, *cmd, *file, *rcsfile;
	size_t size;
	int error, first;

	/*
	 * It seems I don't need all these meta-data here since I get
	 * sent a file with its RCS keywords correctly expanded already.
	 * However, I need to double check that.
	 */
	rcsfile = strsep(&line, " ");
	strsep(&line, " ");	/* XXX - tag */
	strsep(&line, " ");	/* XXX - date */
	strsep(&line, " ");	/* XXX - revnum */
	strsep(&line, " ");	/* XXX - revdate */
	attr = strsep(&line, " ");
	if (attr == NULL || line != NULL)
		return (-1);

	fa = fattr_decode(attr);
	if (fa == NULL) {
		printf("Updater: bad attribute %s\n", attr);
		return (-1);
	}

	file = updater_getpath(coll, rcsfile);
	if (file == NULL) {
		printf("Updater: bad filename \"%s\"\n", rcsfile);
		goto bad;
	}

	lprintf(1, " Checkout %s\n", file + strlen(coll->co_base) + 1);
	error = updater_makedirs(file);
	if (error) {
		printf("Updater: updater_makedirs() failed\n");
		goto bad;
	}

	to = stream_open_file(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (to == NULL) {
		warn("stream_open_file(\"%s\")", file);
		goto bad;
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
		goto bad;
	}
	if (memcmp(line, ".", size) == 0)
		stream_printf(to, "\n");
	stream_close(to);
	/* Get the checksum line. */
	line = stream_getln(rd, NULL);
	cmd = strsep(&line, " ");
	cksum = strsep(&line, " ");
	if (cksum == NULL || line != NULL ||
	    strcmp(cmd, "5") != 0) {
		goto bad;
	}
	if (MD5file(file, md5) == -1 || strcmp(cksum, md5) != 0) {
		printf("%s: bad md5 checksum\n", __func__);
		goto bad;
	}
	updater_install(coll, fa, NULL, file);
	fattr_free(fa);
	free(file);
	return (0);
bad:
	fattr_free(fa);
	free(file);
	return (-1);
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
	asprintf(&path, "%s/%.*s", coll->co_base, (int)(cp - rcsfile), rcsfile);
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

static char *
updater_maketmp(char *file)
{
	char *cp, *tmp;

	cp = strrchr(file, '/');
	if (cp == NULL)
		return (NULL);
	asprintf(&tmp, "%.*s/#cvs.csup-%ld", (int)(cp - file), file,
	    (long)getpid());
	if (tmp == NULL)
		err(1, "asprintf");
	return (tmp);
}

static int
updater_install(struct coll *coll, struct fattr *fa, const char *from,
    const char *to)
{
	struct fattr *tmp;

	tmp = fattr_forcheckout(fa, coll->co_umask);
	fattr_override(fa, tmp, FA_MASK);
	fattr_free(tmp);
	fattr_install(fa, from, to);
	return (0);
}
