/*-
 * Copyright (c) 2003-2006, Maxime Henrion <mux@FreeBSD.org>
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
 * $FreeBSD: projects/csup/updater.c,v 1.65 2006/01/27 17:13:50 mux Exp $
 */

#include <sys/types.h>
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
#include "proto.h"
#include "status.h"
#include "stream.h"

struct context {
	/* These fields are constant during the updater run. */
	struct coll *coll;
	struct stream *rd;
	struct status *st;

	/* These fields are freed if necessary by context_fini(). */
	struct statusrec sr;
	char *author;
	char *state;
	char *tag;
	char *cksum;
	struct stream *from;
	struct stream *to;
	int expand;
};

static void	 context_fini(struct context *);

static void	 updater_prunedirs(char *, char *);
static char	*updater_maketmp(const char *, const char *);
static int	 updater_docoll(struct coll *, struct stream *,
		     struct status *);
static void	 updater_delete(struct coll *, char *);
static int	 updater_checkout(struct context *, char *);
static int	 updater_setattrs(struct context *, char *);
static int	 updater_diff(struct context *, char *);
static int	 updater_diff_parseln(struct context *, char *);
static int	 updater_diff_batch(struct context *);
static int	 updater_diff_apply(struct context *);
static int	 updater_install(struct coll *, struct fattr *, const char *,
		     const char *);

void *
updater(void *arg)
{
	struct config *config;
	struct coll *coll;
	struct status *st;
	struct stream *rd;
	char *line, *cmd, *errmsg, *collname, *release;
	int error;

	config = arg;
	rd = stream_fdopen(config->id1, chan_read, NULL, NULL);
	error = 0;
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		umask(coll->co_umask);
		line = stream_getln(rd, NULL);
		cmd = proto_get_ascii(&line);
		collname = proto_get_ascii(&line);
		release = proto_get_ascii(&line);
		if (release == NULL || line != NULL)
			goto bad;
		if (strcmp(cmd, "COLL") != 0 ||
		    strcmp(collname, coll->co_name) != 0 ||
		    strcmp(release, coll->co_release) != 0)
			goto bad;

		st = status_open(coll, coll->co_scantime, &errmsg);
		if (st == NULL) {
			lprintf(-1, "Updater: %s\n", errmsg);
			stream_close(rd);
			return (NULL);
		}

		lprintf(1, "Updating collection %s/%s\n", coll->co_name,
		    coll->co_release);

		if (coll->co_options & CO_COMPRESS)
			stream_filter_start(rd, STREAM_FILTER_ZLIB, NULL);

		error = updater_docoll(coll, rd, st);
		if (error)
			goto bad;

		status_close(st, &errmsg);
		if (errmsg != NULL) {
			lprintf(-1, "Updater: %s\n", errmsg);
			stream_close(rd);
			return (NULL);
		}

		if (coll->co_options & CO_COMPRESS)
			stream_filter_stop(rd);
	}
	line = stream_getln(rd, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	stream_close(rd);
	return (NULL);
bad:
	lprintf(-1, "Updater: error (%s)\n", line);
	stream_close(rd);
	return (NULL);
}

static int
updater_docoll(struct coll *coll, struct stream *rd, struct status *st)
{
	struct context ctx;
	struct statusrec sr;
	char *cmd, *line, *path, *msg;
	char *name, *tag, *date, *attr;
	int error;

	error = 0;
	memset(&ctx, 0, sizeof(ctx));
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		cmd = proto_get_ascii(&line);
		if (cmd == NULL)
			return (-1);
		ctx.coll = coll;
		ctx.rd = rd;
		ctx.st = st;
		if (strcmp(cmd, "T") == 0) {
			/* Update recorded information for checked-out file. */
			error = updater_setattrs(&ctx, line);
		} else if (strcmp(cmd, "c") == 0) {
			/* Checkout dead file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			if (attr == NULL || line != NULL)
				return (-1);

			/* Theoritically, the file does not exist on the client.
			   Just to make sure, we'll delete it here, if it
			   exists. */
			path = checkoutpath(coll->co_prefix, name);
			if (access(path, F_OK) == 0)
				updater_delete(coll, path);
			free(path);

			sr.sr_type = SR_CHECKOUTDEAD;
			sr.sr_file = name;
			sr.sr_tag = tag;
			sr.sr_date = date;
			sr.sr_serverattr = fattr_decode(attr);
			if (sr.sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}

			error = status_put(st, &sr);
			fattr_free(sr.sr_serverattr);
			if (error) {
				/* XXX write error? */
				return (-1);
			}
		} else if (strcmp(cmd, "U") == 0)
			/* Update live checked-out file. */
			error = updater_diff(&ctx, line);
		else if (strcmp(cmd, "u") == 0) {
			/* Update dead checked-out file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			if (attr == NULL || line != NULL)
				return (-1);

			path = checkoutpath(coll->co_prefix, name);
			updater_delete(coll, path);
			free(path);
			sr.sr_type = SR_CHECKOUTDEAD;
			sr.sr_file = name;
			sr.sr_tag = tag;
			sr.sr_date = date;
			sr.sr_serverattr = fattr_decode(attr);
			if (sr.sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}
			error = status_put(st, &sr);
			fattr_free(sr.sr_serverattr);
			if (error) {
				/* XXX write error? */
				return (-1);
			}
		} else if (strcmp(cmd, "C") == 0) {
			/* Checkout file. */
			error = updater_checkout(&ctx, line);
		} else if (strcmp(cmd, "D") == 0) {
			/* Delete file. */
			name = proto_get_ascii(&line);
			if (name == NULL || line != NULL)
				return (-1);
			path = checkoutpath(coll->co_prefix, name);
			updater_delete(coll, path);
			free(path);
			error = status_delete(st, name, 0);
			if (error)
				return (-1);
		} else if (strcmp(cmd, "!") == 0) {
			/* Warning from server. */
			msg = proto_get_rest(&line);
			if (msg == NULL)
				return (-1);
			lprintf(-1, "Server warning: %s\n", msg);
		} else {
			lprintf(-1, "Updater: Unknown command: "
			    "\"%s\"\n", cmd);
			return (-1);
		}
		context_fini(&ctx);
		if (error)
			return (-1);
	}
	if (line == NULL)
		return (-1);
	return (0);
}

/* Delete file. */
static void
updater_delete(struct coll *coll, char *name)
{
	int error;

	/* XXX - destDir handling */
	/* XXX - delete limit handling */
	if (coll->co_options & CO_DELETE) {
		lprintf(1, " Delete %s\n", name + coll->co_prefixlen + 1);
		error = unlink(name);
		if (error)
			lprintf(-1, "Updater: Cannot delete \"%s\": %s\n",
			    name, strerror(errno));
		if (coll->co_options & CO_CHECKOUTMODE)
			updater_prunedirs(coll->co_prefix, name);
	} else {
		lprintf(1," NoDelete %s\n", name + coll->co_prefixlen + 1);
	}
}

static int
updater_setattrs(struct context *ctx, char *line)
{
	struct statusrec sr;
	struct fattr *fileattr, *rcsattr, *fa;
	struct coll *coll;
	char *attr, *name, *tag, *date, *revnum, *revdate;
	char *path;
	int error, rv;

	coll = ctx->coll;

	name = proto_get_ascii(&line);
	tag = proto_get_ascii(&line);
	date = proto_get_ascii(&line);
	revnum = proto_get_ascii(&line);
	revdate = proto_get_ascii(&line);
	attr = proto_get_ascii(&line);
	if (attr == NULL || line != NULL) {
		lprintf(-1, "Updater: Parse error\n");
		return (-1);
	}

	rcsattr = fattr_decode(attr);
	if (rcsattr == NULL) {
		lprintf(-1, "Updater: Bad attributes \"%s\"\n", attr);
		return (-1);
	}

	path = checkoutpath(coll->co_prefix, name);
	if (path == NULL) {
		lprintf(-1, "Updater: Bad filename \"%s\"\n", name);
		return (-1);
	}
	fileattr = fattr_frompath(path, FATTR_NOFOLLOW);
	if (fileattr == NULL) {
		/* The file has vanished. */
		error = status_delete(ctx->st, name, 0);
		if (error) {
			/* XXX */
		}
		free(path);
		fattr_free(rcsattr);
		return (0);
	}
	fa = fattr_forcheckout(rcsattr, coll->co_umask);
	fattr_override(fileattr, fa, FA_MASK);
	fattr_free(fa);

	rv = fattr_install(fileattr, path, NULL);
	if (rv == -1) {
		/* XXX ignore if errno == ENOENT and if a different detDir
		   was specified. */
		/*
		if (errno == ENOENT && XXX) {
			lprintf(1, " SetAttrs %s\n",
			    path + coll->co_prefixlen + 1);
			    XXX
		}
		 */
		free(path);
		fattr_free(rcsattr);
		fattr_free(fileattr);
		return (-1);
	}
	if (rv == 1) {
		lprintf(1, " SetAttrs %s\n",
		    path + coll->co_prefixlen + 1);
		fattr_free(fileattr);
		fileattr = fattr_frompath(path, FATTR_NOFOLLOW);
	}

	fattr_maskout(fileattr, FA_COIGNORE);

	sr.sr_type = SR_CHECKOUTLIVE;
	sr.sr_file = name;
	sr.sr_tag = tag;
	sr.sr_date = date;
	sr.sr_revnum = revnum;
	sr.sr_revdate = revdate;
	sr.sr_clientattr = fileattr;
	sr.sr_serverattr = rcsattr;

	error = status_put(ctx->st, &sr);
	fattr_free(fileattr);
	fattr_free(rcsattr);
	if (error) {
		/* XXX */
		return (-1);
	}
	free(path);
	return (0);
}

static int
updater_diff_parseln(struct context *ctx, char *line)
{
	struct statusrec *sr;
	char *cp, *name, *tag, *date;
	char *expand, *attr, *cksum;

	cp = line;
	name = proto_get_ascii(&cp);
	tag = proto_get_ascii(&cp);
	date = proto_get_ascii(&cp);
	proto_get_ascii(&cp);	/* XXX - oldRevNum */
	proto_get_ascii(&cp);	/* XXX - fromAttic */
	proto_get_ascii(&cp);	/* XXX - logLines */
	expand = proto_get_ascii(&cp);
	attr = proto_get_ascii(&cp);
	cksum = proto_get_ascii(&cp);
	if (cksum == NULL || cp != NULL)
		return (-1);

	sr = &ctx->sr;
	sr->sr_type = SR_CHECKOUTLIVE;
	sr->sr_file = xstrdup(name);
	sr->sr_date = xstrdup(date);
	sr->sr_tag = xstrdup(tag);
	sr->sr_serverattr = fattr_decode(attr);
	if (sr->sr_serverattr == NULL) {
		lprintf(-1, "Updater: Bad attributes \"%s\"\n", attr);
		return (-1);
	}

	if (strcmp(expand, ".") == 0)
		ctx->expand = EXPAND_DEFAULT;
	else if (strcmp(expand, "kv") == 0)
		ctx->expand = EXPAND_KEYVALUE;
	else if (strcmp(expand, "kvl") == 0)
		ctx->expand = EXPAND_KEYVALUELOCKER;
	else if (strcmp(expand, "k") == 0)
		ctx->expand = EXPAND_KEY;
	else if (strcmp(expand, "o") == 0)
		ctx->expand = EXPAND_OLD;
	else if (strcmp(expand, "b") == 0)
		ctx->expand = EXPAND_BINARY;
	else if (strcmp(expand, "v") == 0)
		ctx->expand = EXPAND_VALUE;
	else
		return (-1);

	ctx->tag = xstrdup(tag);
	ctx->cksum = xstrdup(cksum);
	return (0);
}

static void
context_fini(struct context *ctx)
{
	struct statusrec *sr;

	sr = &ctx->sr;
	if (ctx->author != NULL) {
		free(ctx->author);
		ctx->author = NULL;
	}
	if (ctx->cksum != NULL) {
		free(ctx->cksum);
		ctx->cksum = NULL;
	}
	if (ctx->tag != NULL) {
		free(ctx->tag);
		ctx->tag = NULL;
	}
	if (ctx->from != NULL) {
		stream_close(ctx->from);
		ctx->from = NULL;
	}
	if (ctx->to != NULL) {
		stream_close(ctx->to);
		ctx->to = NULL;
	}
	if (sr->sr_file != NULL)
		free(sr->sr_file);
	if (sr->sr_tag != NULL)
		free(sr->sr_tag);
	if (sr->sr_date != NULL)
		free(sr->sr_date);
	if (sr->sr_revnum != NULL)
		free(sr->sr_revnum);
	if (sr->sr_revdate != NULL)
		free(sr->sr_revdate);
	fattr_free(sr->sr_clientattr);
	fattr_free(sr->sr_serverattr);
	memset(sr, 0, sizeof(*sr));
}

static int
updater_diff(struct context *ctx, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	struct coll *coll;
	struct statusrec *sr;
	struct fattr *fa;
	char *author, *path, *revnum, *revdate;
	char *tok, *topath;
	int error;

	path = NULL;
	topath = NULL;
	sr = &ctx->sr;
	coll = ctx->coll;

	error = updater_diff_parseln(ctx, line);
	if (error) {
		lprintf(-1, "Updater: Parse error\n");
		goto bad;
	}

	path = checkoutpath(coll->co_prefix, sr->sr_file);
	if (path == NULL) {
		lprintf(-1, "Updater: Bad filename \"%s\"\n", sr->sr_file);
		goto bad;
	}

	lprintf(1, " Edit %s\n", path + coll->co_prefixlen + 1);
	while ((line = stream_getln(ctx->rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = proto_get_ascii(&line);
		if (strcmp(tok, "D") != 0)
			goto bad;
		revnum = proto_get_ascii(&line);
		proto_get_ascii(&line); /* XXX - diffbase */
		revdate = proto_get_ascii(&line);
		author = proto_get_ascii(&line);
		if (author == NULL || line != NULL)
			goto bad;
		if (sr->sr_revnum != NULL)
			free(sr->sr_revnum);
		if (sr->sr_revdate != NULL)
			free(sr->sr_revdate);
		if (ctx->author != NULL)
			free(ctx->author);
		ctx->author = xstrdup(author);
		sr->sr_revnum = xstrdup(revnum);
		sr->sr_revdate = xstrdup(revdate);
		if (ctx->from == NULL) {
			/* First patch, the "origin" file is the one we have. */
			ctx->from = stream_open_file(path, O_RDONLY);
			if (ctx->from == NULL)
				goto bad;
		} else {
			/* Subsequent patches. */
			stream_close(ctx->from);
			ctx->from = ctx->to;
			stream_rewind(ctx->from);
			unlink(topath);
			free(topath);
		}
		topath = updater_maketmp(coll->co_prefix, sr->sr_file);
		if (topath == NULL) {
			perror("Cannot create temporary file");
			goto bad;
		}
		ctx->to = stream_open_file(topath,O_RDWR | O_CREAT | O_EXCL,
		    0600);
		if (ctx->to == NULL)
			goto bad;
		lprintf(2, "  Add delta %s %s %s\n", revnum, revdate, author);
		error = updater_diff_batch(ctx);
		if (error)
			goto bad;
	}
	if (line == NULL)
		goto bad;
	fa = fattr_dup(sr->sr_serverattr);
	fattr_maskout(fa, FA_MODTIME);
	updater_install(coll, fa, topath, path);
	sr->sr_clientattr = fa;
	error = status_put(ctx->st, sr);
	if (error) {
		lprintf(-1, "Updater: status_put() failed!\n");
		goto bad;
	}

	if (MD5_File(path, md5) == -1) {
		lprintf(-1, "%s: MD5_File() failed\n", __func__);
		goto bad;
	}
	if (strcmp(ctx->cksum, md5) != 0) {
		lprintf(-1, "Updater: Bad MD5 checksum for \"%s\"\n", path);
		goto bad;
	}
	free(topath);
	free(path);
	return (0);
bad:
	free(topath);
	free(path);
	return (-1);
}

static int
updater_diff_batch(struct context *ctx)
{
	struct stream *rd;
	char *tok, *line;
	int error;

	rd = ctx->rd;
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		tok = proto_get_ascii(&line);
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
			tok = proto_get_ascii(&line);
			if (tok == NULL || line != NULL)
				goto bad;
			free(ctx->state);
			ctx->state = xstrdup(tok);
		} else if (strcmp(tok, "T") == 0) {
			error = updater_diff_apply(ctx);
			if (error) {
				free(ctx->state);
				ctx->state = NULL;
				return (error);
			}
		}
	}
	if (line == NULL)
		goto bad;
	free(ctx->state);
	ctx->state = NULL;
	return (0);
bad:
	lprintf(-1, "Updater: Protocol error\n");
	free(ctx->state);
	ctx->state = NULL;
	return (-1);
}

int
updater_diff_apply(struct context *ctx)
{
	struct diff diff;
	struct coll *coll;
	struct statusrec *sr;
	int error;

	sr = &ctx->sr;
	coll = ctx->coll;

	diff.d_orig = ctx->from;
	diff.d_to = ctx->to;
	diff.d_diff = ctx->rd;
	diff.d_author = ctx->author;
	diff.d_cvsroot = coll->co_cvsroot;
	diff.d_rcsfile = sr->sr_file;
	diff.d_revnum = sr->sr_revnum;
	diff.d_revdate = sr->sr_revdate;
	diff.d_state = ctx->state;
	diff.d_tag = ctx->tag;
	diff.d_expand = ctx->expand;
	error = diff_apply(&diff, coll->co_keyword);
	if (error) {
		lprintf(-1, "Updater: Bad diff from server\n");
		return (-1);
	}
	return (0);
}

/* XXX check write errors */
static int
updater_checkout(struct context *ctx, char *line)
{
	char md5[MD5_DIGEST_SIZE];
	struct statusrec *sr;
	struct coll *coll;
	struct fattr *rcsattr, *fileattr, *tmp;
	struct stream *to;
	char *attr, *cksum, *cmd, *file, *name;
	char *tag, *date, *revnum, *revdate;
	time_t t;
	size_t size;
	int error, first;

	coll = ctx->coll;
	sr = &ctx->sr;
	name = proto_get_ascii(&line);
	tag = proto_get_ascii(&line);
	date = proto_get_ascii(&line);
	revnum = proto_get_ascii(&line);
	revdate = proto_get_ascii(&line);
	attr = proto_get_ascii(&line);
	if (attr == NULL || line != NULL)
		return (-1);

	rcsattr = fattr_decode(attr);
	if (rcsattr == NULL) {
		lprintf(-1, "Updater: Bad attributes %s\n", attr);
		return (-1);
	}
	t = rcsdatetotime(revdate);
	if (t == -1) {
		lprintf(-1, "Updater: Invalid RCS date: %s\n", revdate);
		return (-1);
	}
	fileattr = fattr_new(FT_FILE, t);

	tmp = fattr_forcheckout(rcsattr, coll->co_umask);
	fattr_override(fileattr, tmp, FA_MASK);
	fattr_free(tmp);

	fattr_mergedefault(fileattr);
	fattr_umask(fileattr, coll->co_umask);

	sr->sr_type = SR_CHECKOUTLIVE;
	sr->sr_file = xstrdup(name);
	sr->sr_tag = xstrdup(tag);
	sr->sr_date = xstrdup(date);
	sr->sr_revnum = xstrdup(revnum);
	sr->sr_revdate = xstrdup(revdate);
	sr->sr_serverattr = rcsattr;
	sr->sr_clientattr = fileattr;

	file = checkoutpath(coll->co_prefix, name);
	if (file == NULL) {
		lprintf(-1, "Updater: Bad filename \"%s\"\n", name);
		goto bad;
	}

	lprintf(1, " Checkout %s\n", file + coll->co_prefixlen + 1);
	error = mkdirhier(file);
	if (error) {
		lprintf(-1, "Updater: Cannot create directory hierarchy: %s\n",
		    strerror(errno));
		goto bad;
	}

	to = stream_open_file(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (to == NULL) {
		warn("stream_open_file(\"%s\")", file);
		goto bad;
	}
	stream_filter_start(to, STREAM_FILTER_MD5, md5);
	line = stream_getln(ctx->rd, &size);
	first = 1;
	while (line != NULL) {
		if (line[size - 1] == '\n')
			size--;
	       	if ((size == 1 && *line == '.') ||
		    (size == 2 && memcmp(line, ".+", 2) == 0))
			break;
		if (size >= 2 && memcmp(line, "..", 2) == 0) {
			size--;
			line++;
		}
		if (!first)
			stream_write(to, "\n", 1);
		stream_write(to, line, size);
		line = stream_getln(ctx->rd, &size);
		first = 0;
	}
	if (line == NULL) {
		stream_close(to);
		goto bad;
	}
	if (size == 1 && *line == '.')
		stream_write(to, "\n", 1);
	stream_close(to);
	/* Get the checksum line. */
	line = stream_getln(ctx->rd, NULL);
	cmd = proto_get_ascii(&line);
	cksum = proto_get_ascii(&line);
	if (cksum == NULL || line != NULL || strcmp(cmd, "5") != 0)
		goto bad;
	if (strcmp(cksum, md5) != 0) {
		lprintf(-1, "Updater: Bad MD5 checksum for \"%s\"\n", file);
		goto bad;
	}

	fattr_install(fileattr, file, NULL);

	/* XXX Executes */
	/*
	 * We weren't necessarily able to set all the file attributes to the
	 * desired values, and any executes may have altered the attributes.
	 * To make sure we record the actual attribute values, we fetch
	 * them from the file.
	 *
	 * However, we preserve the link count as received from the
	 * server.  This is important for preserving hard links in mirror
	 * mode.
	 */
	fileattr = fattr_frompath(file, FATTR_NOFOLLOW);
	if (fileattr == NULL) {
		lprintf(-1, "Updater: Cannot stat \"%s\": %s\n", file,
		    strerror(errno));
		return (-1);
	}
	fattr_override(fileattr, sr->sr_clientattr, FA_LINKCOUNT);
	fattr_free(sr->sr_clientattr);
	sr->sr_clientattr = fileattr;

	/*
	 * To save space, don't write out the device and inode unless
	 * the link count is greater than 1.  These attributes are used
	 * only for detecting hard links.  If the link count is 1 then we
	 * know there aren't any hard links.
	 */
	if (!(fattr_getmask(sr->sr_clientattr) & FA_LINKCOUNT) ||
	    fattr_getlinkcount(sr->sr_clientattr) <= 1)
		fattr_maskout(sr->sr_clientattr, FA_DEV | FA_INODE);

	if (coll->co_options & CO_CHECKOUTMODE)
		fattr_maskout(sr->sr_clientattr, FA_COIGNORE);

	error = status_put(ctx->st, sr);
	if (error) {
		/* XXX */
		return (-1);
	}

	free(file);
	return (0);
bad:
	free(file);
	return (-1);
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
				/* XXX */
				err(1, "rmdir");
			return;
		}
	}
}

static char *
updater_maketmp(const char *prefix, const char *file)
{
	char *path, *tmp;

	xasprintf(&path, "%s/%s", prefix, file);
	tmp = tempname(path);
	free(path);
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
	fattr_install(fa, to, from);
	return (0);
}
