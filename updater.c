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
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "diff.h"
#include "keyword.h"
#include "updater.h"
#include "misc.h"
#include "mux.h"
#include "stream.h"

static char	*updater_getpath(struct collection *, char *);
static int	 updater_makedirs(char *);
static void	 updater_prunedirs(char *, char *);
static int	 updater_checkout(struct collection *, struct stream *, char *);
static int	 updater_delete(struct collection *, char *);
static int	 updater_diff(struct collection *, struct stream *, char *);
static int	 updater_diff_apply(struct collection *, char *,
		     struct stream *, struct diff *);
static int	 updater_dodiff(struct collection *, char *, struct stream *,
		     struct diff *);
static void	 diff_free(struct diff *diff);

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
		umask(cur->umask);
		line = stream_getln(rd, NULL);
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
			line = stream_getln(rd, NULL);
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
	}
	return (NULL);
bad:
	fprintf(stderr, "Updater: Protocol error\n");
	return (NULL);
}

static int
updater_delete(struct collection *coll, char *line)
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
updater_diff(struct collection *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	char cksum[MD5_DIGEST_SIZE];
	struct diff diff;
	char *author, *tag, *tok, *path, *rcsfile, *revnum, *revdate;
	int error;

	memset(&diff, 0, sizeof(struct diff));

	rcsfile = strsep(&line, " ");
	tag = strsep(&line, " ");
	strsep(&line, " "); /* XXX - date */
	strsep(&line, " "); /* XXX - orig revnum */
	strsep(&line, " "); /* XXX - from attic */
	strsep(&line, " "); /* XXX - loglines */
	strsep(&line, " "); /* XXX - expand */
	strsep(&line, " "); /* XXX - attr */
	tok = strsep(&line, " ");
	if (tok == NULL || line != NULL)
		return (-1);
	diff.d_rcsfile = strdup(rcsfile);
	if (diff.d_rcsfile == NULL)
		err(1, "strdup");
	if (strcmp(tag, ".") != 0) {
		diff.d_tag = strdup(tag);
		if (diff.d_tag == NULL)
			err(1, "strdup");
	}
	strlcpy(cksum, tok, sizeof(cksum));

	path = updater_getpath(coll, rcsfile);
	if (path == NULL) {
		printf("%s: bad filename %s\n", __func__, rcsfile);
		return (-1);
	}

	lprintf(1, " Edit %s\n", path + strlen(coll->base) + 1);
	for (;;) {
		line = stream_getln(rd, NULL);
		if (strcmp(line, ".") == 0)
			break;
		tok = strsep(&line, " ");
		if (strcmp(tok, "D") != 0)
			goto bad;
		revnum = strsep(&line, " ");
		strsep(&line, " "); /* XXX - diffbase */
		revdate = strsep(&line, " ");
		author = strsep(&line, " ");
		if (revnum == NULL || revdate == NULL || author == NULL ||
		    line != NULL)
			goto bad;
		diff.d_cvsroot = strdup(coll->cvsroot);
		diff.d_revnum = strdup(revnum);
		diff.d_revdate = strdup(revdate);
		diff.d_author = strdup(author);
		if (diff.d_cvsroot == NULL || diff.d_revnum == NULL ||
		    diff.d_revdate == NULL || diff.d_author == NULL)
			err(1, "strdup");
		lprintf(2, "  Add diff %s %s %s\n", revnum, revdate, author);
		error = updater_diff_apply(coll, path, rd, &diff);
		if (error) {
			printf("%s: updater_diff_apply failed\n", __func__);
			goto bad;
		}
	}
	diff_free(&diff);
	if (MD5file(path, md5) == -1 || strcmp(cksum, md5) != 0) {
		free(path);
		printf("%s: bad md5 checksum\n", __func__);
		return (-1);
	}
	free(path);
	return (0);
bad:
	diff_free(&diff);
	free(path);
	return (-1);
}

static void
diff_free(struct diff *diff)
{

	free(diff->d_rcsfile);
	free(diff->d_cvsroot);
	free(diff->d_revnum);
	free(diff->d_revdate);
	free(diff->d_author);
	free(diff->d_state);
}

static int
updater_diff_apply(struct collection *coll, char *path, struct stream *rd,
    struct diff *diff)
{
	char *tok, *line;
	int error;

	for (;;) {
		line = stream_getln(rd, NULL);
		if (line == NULL)
			return (-1);
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
	return (0);
}

/*
 * XXX - This is a mess, I should finish the stream API and make it work
 * sufficiently fine so that we can use it here instead of stdio.
 */
int
updater_dodiff(struct collection *coll, char *path, struct stream *rd,
    struct diff *diff)
{
	FILE *to;
	struct stream *orig;
	char *tmp;
	int fd, error;

	asprintf(&tmp, "%s/%s", dirname(path), "#cvs.csup-XXXXX");
	if (tmp == NULL) {
		warn("asprintf");
		return (-1);
	}
	fd = mkstemp(tmp);
	if (fd == -1) {
		warn("mkstemp");
		free(tmp);
		return (-1);
	}
	to = fdopen(fd, "w");
	if (to == NULL) {
		warn("fdopen");
		close(fd);
		free(tmp);
		return (-1);
	}

	orig = stream_open_file(path, O_RDONLY);
	if (orig == NULL) {
		warn("stream_open_file");
		fclose(to);
		free(tmp);
		return (-1);
	}
	diff->d_orig = orig;
	diff->d_diff = rd;
	error = diff_apply(diff, coll->keyword, to);
	fclose(to);
	stream_close(orig);
	if (error) {
		printf("%s: diff_apply failed\n", __func__);
		free(tmp);
		return (-1);
	}
	error = rename(tmp, path);
	free(tmp);
	if (error) {
		warn("rename");
		return (-1);
	}
	return (0);
}

static int
updater_checkout(struct collection *coll, struct stream *rd, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	char *cksum, *cmd, *file, *rcsfile;
	FILE *to;
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
	to = fopen(file, "w");
	if (to == NULL) {
		warn("fopen");
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
			fprintf(to, "\n");
		fwrite(line, size, 1, to);
		line = stream_getln(rd, &size);
		first = 0;
	}
	if (line == NULL) {
		fclose(to);
		free(file);
		return (-1);
	}
	if (memcmp(line, ".", size) == 0)
		fprintf(to, "\n");
	fsync(fileno(to));
	fclose(to);
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
updater_getpath(struct collection *coll, char *rcsfile)
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
	if (cp == NULL || cp[2] != '\0')
		return (NULL);
	*cp = '\0';
	asprintf(&path, "%s/%s", coll->base, rcsfile);
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
