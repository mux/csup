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
 * $FreeBSD: projects/csup/lister.c,v 1.20 2006/02/03 15:47:13 mux Exp $
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attrstack.h"
#include "config.h"
#include "fattr.h"
#include "lister.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "status.h"
#include "stream.h"

static int	lister_coll(struct config *, struct stream *, struct coll *,
		    struct status *);
static void	lister_sendbogus(struct config *, struct stream *,
		    struct statusrec *);
static int	lister_dodirdown(struct config *, struct stream *,
		    struct coll *, struct statusrec *, struct attrstack *as);
static int	lister_dodirup(struct config *, struct stream *, struct coll *,
    		    struct statusrec *, struct attrstack *as);
static int	lister_dofile(struct config *, struct stream *, struct coll *,
		    struct statusrec *);
static int	lister_dodead(struct config *, struct stream *, struct coll *,
		    struct statusrec *);

void *
lister(void *arg)
{
	struct config *config;
	struct coll *coll;
	struct stream *wr;
	struct status *st;
	char *errmsg;
	int error;

	config = arg;
	wr = stream_fdopen(config->id0, NULL, chan_write, NULL);
	STAILQ_FOREACH(coll, &config->colls, co_next) {
		if (coll->co_options & CO_SKIP)
			continue;
		st = status_open(coll, -1, &errmsg);
		if (st == NULL) {
			lprintf(-1, "Lister: %s\n", errmsg);
			stream_close(wr);
			return (NULL);
		}
		proto_printf(wr, "COLL %s %s\n", coll->co_name,
		    coll->co_release);
		stream_flush(wr);
		if (coll->co_options & CO_COMPRESS)
			stream_filter_start(wr, STREAM_FILTER_ZLIB, NULL);
		error = lister_coll(config, wr, coll, st);
		status_close(st, NULL);
		if (error) {
			stream_close(wr);
			return (NULL);
		}
		if (coll->co_options & CO_COMPRESS)
			stream_filter_stop(wr);
		stream_flush(wr);
	}
	proto_printf(wr, ".\n");
	stream_close(wr);
	return (NULL);
}

/* List a single collection based on the status file. */
static int
lister_coll(struct config *config, struct stream *wr, struct coll *coll,
    struct status *st)
{
	struct attrstack *as;
	struct statusrec *sr;
	struct fattr *fa;
	int depth, error, prunedepth;

	depth = 0;
	prunedepth = INT_MAX;
	as = attrstack_new();
	while ((sr = status_get(st, NULL, 0, 0)) != NULL) {
		switch (sr->sr_type) {
		case SR_DIRDOWN:
			depth++;
			if (depth < prunedepth) {
				error = lister_dodirdown(config, wr, coll, sr,
				    as);
				if (error) {
					prunedepth = depth;
					break;
				}
			}
			break;
		case SR_DIRUP:
			if (depth < prunedepth) {
				error = lister_dodirup(config, wr, coll, sr,
				    as);
			} else if (depth == prunedepth) {
				/* Finished pruning. */
				prunedepth = INT_MAX;
			}
			depth--;
			continue;
		case SR_CHECKOUTLIVE:
			if (depth < prunedepth)
				error = lister_dofile(config, wr, coll, sr);
			break;
		case SR_CHECKOUTDEAD:
			if (depth < prunedepth)
				error = lister_dodead(config, wr, coll, sr);
			break;
		default:
			goto bad;
		}
	}
	if (!status_eof(st)) {
		lprintf(-1, "Lister: %s.  Delete it and try again.\n",
		    status_errmsg(st));
		goto bad;
	}
	assert(depth == 0);
	proto_printf(wr, ".\n");
	attrstack_free(as);
	return (0);
bad:
	while (depth-- > 0) {
		fa = attrstack_pop(as);
		fattr_free(fa);
	}
	attrstack_free(as);
	return (-1);
}

/* Handle a directory up entry found in the status file. */
static int
lister_dodirdown(struct config *config, struct stream *wr, struct coll *coll,
    struct statusrec *sr, struct attrstack *as)
{
	struct fattr *fa, *fa2;
	char *path;

	if (coll->co_options & CO_TRUSTSTATUSFILE) {
		fa = fattr_new(FT_DIRECTORY, -1);
	} else {
		xasprintf(&path, "%s/%s", coll->co_prefix, sr->sr_file);
		fa = fattr_frompath(path, FATTR_NOFOLLOW);
		if (fa == NULL) {
			/* The directory doesn't exist, prune
			 * everything below it. */
			free(path);
			return (-1);
		}
		if (fattr_type(fa) == FT_SYMLINK) {
			fa2 = fattr_frompath(path, FATTR_FOLLOW);
			if (fa2 != NULL && fattr_type(fa2) == FT_DIRECTORY) {
				/* XXX - When not in checkout mode, CVSup warns
				 * here about the file being a symlink to a
				 * directory instead of a directory. */
				fattr_free(fa);
				fa = fa2;
			} else {
				fattr_free(fa2);
			}
		}
		free(path);
	}

	if (fattr_type(fa) != FT_DIRECTORY) {
		fattr_free(fa);
		/* Report it as something bogus so
		 * that it will be replaced. */
		lister_sendbogus(config, wr, sr);
		return (-1);
	}

	/* It really is a directory. */
	attrstack_push(as, fa);
	proto_printf(wr, "D %s\n", pathlast(sr->sr_file));
	return (0);
}

/* Handle a directory up entry found in the status file. */
static int
lister_dodirup(struct config *config, struct stream *wr, struct coll *coll,
    struct statusrec *sr, struct attrstack *as)
{
	const struct fattr *sendattr;
	struct fattr *fa, *fa2;

	fa = attrstack_pop(as);
	if (coll->co_options & CO_TRUSTSTATUSFILE) {
		fattr_free(fa);
		fa = sr->sr_clientattr;
	}

	fa2 = sr->sr_clientattr;
	if (fattr_equal(fa, fa2))
		sendattr = fa;
	else
		sendattr = fattr_bogus;
	proto_printf(wr, "U %F\n", sendattr, config->fasupport);
	if (!(coll->co_options & CO_TRUSTSTATUSFILE))
		fattr_free(fa);
	/* XXX CVSup flushes here for some reason with a comment saying
	   "Be smarter".  We don't flush when listing other file types. */
	stream_flush(wr);
	return (0);
}

/* Handle a checkout live entry found in the status file. */
static int
lister_dofile(struct config *config, struct stream *wr, struct coll *coll,
    struct statusrec *sr)
{
	struct fattr *fa, *fa2, *cfa, *sfa, *rfa;
	char *path;
	int error;

	fa = NULL;
	rfa = NULL;
	error = 0;
	if (!(coll->co_options & CO_TRUSTSTATUSFILE)) {
		path = checkoutpath(coll->co_prefix, sr->sr_file);
		if (path == NULL)
			goto bad;
		rfa = fattr_frompath(path, FATTR_NOFOLLOW);
		free(path);
		if (rfa == NULL)
			goto bad;
		fa = rfa;
	}
	cfa = sr->sr_clientattr;
	if (fa == NULL)
		fa = cfa;
	sfa = sr->sr_serverattr;
	fa2 = fattr_forcheckout(sfa, coll->co_umask);
	if (!fattr_equal(fa, cfa) || !fattr_equal(fa, fa2) ||
	    strcmp(coll->co_tag, sr->sr_tag) != 0 ||
	    strcmp(coll->co_date, sr->sr_date) != 0) {
		fattr_free(fa2);
		fattr_free(rfa);
		goto bad;
	}
	proto_printf(wr, "F %s %F\n", pathlast(sr->sr_file), sfa,
	    config->fasupport);
	fattr_free(fa2);
	fattr_free(rfa);
	return (0);
bad:
	lister_sendbogus(config, wr, sr);
	return (-1);
}

/* Handle a checkout dead entry found in the status file. */
static int
lister_dodead(struct config *config, struct stream *wr, struct coll *coll,
    struct statusrec *sr)
{
	const struct fattr *sendattr;
	struct fattr *fa;
	char *path;

	if (!(coll->co_options & CO_TRUSTSTATUSFILE)) {
		path = checkoutpath(coll->co_prefix, sr->sr_file);
		if (path == NULL) {
			/* XXX I think checkoutpath() errors are fatal. */
			return (-1);
		}
		fa = fattr_frompath(path, FATTR_NOFOLLOW);
		free(path);
		if (fa != NULL && fattr_type(fa) != FT_DIRECTORY) {
			/*
			 * We shouldn't have this file but we do.  Report
			 * it to the server, which will either send a
			 * deletion request, of (if the file has come alive)
			 * sent the correct version.
			 */
			fattr_free(fa);
			lister_sendbogus(config, wr, sr);
			return (-1);
		}
		fattr_free(fa);
	}
	if (strcmp(coll->co_tag, sr->sr_tag) != 0 ||
	    strcmp(coll->co_date, sr->sr_date) != 0)
		sendattr = fattr_bogus;
	else
		sendattr = sr->sr_serverattr;
	proto_printf(wr, "f %s %F\n", pathlast(sr->sr_file), sendattr,
	    config->fasupport);
	return (0);
}

static void
lister_sendbogus(struct config *config, struct stream *wr, struct statusrec *sr)
{

	proto_printf(wr, "F %s %F\n", pathlast(sr->sr_file), fattr_bogus,
	    config->fasupport);
}
