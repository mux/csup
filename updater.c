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
 * $FreeBSD: projects/csup/updater.c,v 1.74 2006/02/12 04:10:28 mux Exp $
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
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
#include "fixups.h"
#include "keyword.h"
#include "updater.h"
#include "misc.h"
#include "mux.h"
#include "proto.h"
#include "status.h"
#include "stream.h"

/* Everything needed to update a file. */
struct file_update {
	struct statusrec srbuf;
	char *destpath;
	char *coname;		/* Points somewhere in destpath. */
	char *wantmd5;
	struct coll *coll;
	struct status *st;
	/* Those are only used for diff updating. */
	char *author;
	struct stream *orig;
	struct stream *to;
	int expand;
};

struct updater {
	struct config *config;
	struct stream *rd;
};

static struct file_update	*fup_new(struct coll *, struct status *);
static int	 fup_prepare(struct file_update *, char *);
static void	 fup_cleanup(struct file_update *);
static void	 fup_free(struct file_update *);

static void	 updater_prunedirs(char *, char *);
static int	 updater_dobatch(struct updater *, int);
static int	 updater_docoll(struct updater *, struct file_update *, int);
static void	 updater_delete(struct file_update *);
static int	 updater_checkout(struct updater *, struct file_update *, int);
static int	 updater_setattrs(struct file_update *, char *, char *, char *,
		     char *, char *, struct fattr *);
static void	 updater_checkmd5(struct updater *, struct file_update *,
		     const char *, int);
static int	 updater_updatefile(struct file_update *fup, const char *,
		     const char *);
static int	 updater_diff(struct updater *, struct file_update *);
static int	 updater_diff_batch(struct updater *, struct file_update *);
static int	 updater_diff_apply(struct updater *, struct file_update *,
		     char *);

static struct file_update *
fup_new(struct coll *coll, struct status *st)
{
	struct file_update *fup;

	fup = xmalloc(sizeof(struct file_update));
	memset(fup, 0, sizeof(*fup));
	fup->coll = coll;
	fup->st = st;
	return (fup);
}

static int
fup_prepare(struct file_update *fup, char *name)
{
	struct coll *coll;

	coll = fup->coll;
	fup->destpath = checkoutpath(coll->co_prefix, name);
	if (fup->destpath == NULL)
		return (-1);
	fup->coname = fup->destpath + coll->co_prefixlen + 1;
	return (0);
}

/* Called after each file update to reinit the structure. */
static void
fup_cleanup(struct file_update *fup)
{
	struct statusrec *sr;

	sr = &fup->srbuf;

	if (fup->destpath != NULL) {
		free(fup->destpath);
		fup->destpath = NULL;
	}
	fup->coname = NULL;
	if (fup->author != NULL) {
		free(fup->author);
		fup->author = NULL;
	}
	fup->expand = 0;
	if (fup->wantmd5 != NULL) {
		free(fup->wantmd5);
		fup->wantmd5 = NULL;
	}
	if (fup->orig != NULL) {
		stream_close(fup->orig);
		fup->orig = NULL;
	}
	if (fup->to != NULL) {
		stream_close(fup->to);
		fup->to = NULL;
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

static void
fup_free(struct file_update *fup)
{

	fup_cleanup(fup);
	free(fup);
}

void *
updater(void *arg)
{
	struct updater up;
	struct config *config;
	struct stream *rd;
	int error;

	config = arg;
	rd = stream_open(config->chan1,
	    (stream_readfn_t *)chan_read, NULL, NULL);

	up.config = config;
	up.rd = rd;
	error = updater_dobatch(&up, 0);

	/*
	 * Make sure to close the fixups even in case of an error,
	 * so that the lister thread doesn't block indefinitely.
	 */
	fixups_close(config->fixups);
	if (!error)
		error = updater_dobatch(&up, 1);
	stream_close(rd);
	return (NULL);
}

static int
updater_dobatch(struct updater *up, int isfixups)
{
	struct stream *rd;
	struct coll *coll;
	struct status *st;
	struct file_update *fup;
	char *line, *cmd, *errmsg, *collname, *release;
	int error;

	rd = up->rd;
	STAILQ_FOREACH(coll, &up->config->colls, co_next) {
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
			return (-1);
		}

		if (!isfixups)
			lprintf(1, "Updating collection %s/%s\n", coll->co_name,
			    coll->co_release);

		if (coll->co_options & CO_COMPRESS)
			stream_filter_start(rd, STREAM_FILTER_ZLIB, NULL);

		fup = fup_new(coll, st);
		error = updater_docoll(up, fup, isfixups);
		fup_free(fup);
		if (error)
			return (-1);

		status_close(st, &errmsg);
		if (errmsg != NULL) {
			lprintf(-1, "Updater: %s\n", errmsg);
			return (-1);
		}

		if (coll->co_options & CO_COMPRESS)
			stream_filter_stop(rd);
	}
	line = stream_getln(rd, NULL);
	if (line == NULL || strcmp(line, ".") != 0)
		goto bad;
	return (0);
bad:
	lprintf(-1, "Updater: Protocol error\n");
	return (-1);
}

static int
updater_docoll(struct updater *up, struct file_update *fup, int isfixups)
{
	struct stream *rd;
	struct coll *coll;
	struct statusrec srbuf, *sr;
	struct fattr *rcsattr, *tmp;
	char *cmd, *line, *msg, *attr;
	char *name, *tag, *date, *revdate;
	char *expand, *wantmd5, *revnum;
	time_t t;
	int error, needfixupmsg;

	error = 0;
	rd = up->rd;
	coll = fup->coll;
	needfixupmsg = isfixups;
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		memset(&srbuf, 0, sizeof(srbuf));
		if (needfixupmsg) {
			lprintf(1, "Applying fixups for collection %s/%s\n",
			    coll->co_name, coll->co_release);
			needfixupmsg = 0;
		}
		cmd = proto_get_ascii(&line);
		if (cmd == NULL || strlen(cmd) != 1)
			return (-1);
		switch (cmd[0]) {
		case 'T':
			/* Update recorded information for checked-out file. */
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
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}

			error = fup_prepare(fup, name);
			if (error)
				return (-1);
			error = updater_setattrs(fup, name, tag, date, revnum,
			    revdate, rcsattr);
			fattr_free(rcsattr);
			break;
		case 'c':
			/* Checkout dead file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			if (attr == NULL || line != NULL)
				return (-1);

			error = fup_prepare(fup, name);
			if (error)
				return (-1);
			/* Theoritically, the file does not exist on the client.
			   Just to make sure, we'll delete it here, if it
			   exists. */
			if (access(fup->destpath, F_OK) == 0)
				updater_delete(fup);

			sr = &srbuf;
			sr->sr_type = SR_CHECKOUTDEAD;
			sr->sr_file = name;
			sr->sr_tag = tag;
			sr->sr_date = date;
			sr->sr_serverattr = fattr_decode(attr);
			if (sr->sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}

			error = status_put(fup->st, sr);
			fattr_free(sr->sr_serverattr);
			if (error) {
				lprintf(-1, "Updater: %s\n",
				    status_errmsg(fup->st));
				return (-1);
			}
			break;
		case 'U':
			/* Update live checked-out file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			proto_get_ascii(&line);	/* XXX - oldRevNum */
			proto_get_ascii(&line);	/* XXX - fromAttic */
			proto_get_ascii(&line);	/* XXX - logLines */
			expand = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			wantmd5 = proto_get_ascii(&line);
			if (wantmd5 == NULL || line != NULL)
				return (-1);

			sr = &fup->srbuf;
			sr->sr_type = SR_CHECKOUTLIVE;
			sr->sr_file = xstrdup(name);
			sr->sr_date = xstrdup(date);
			sr->sr_tag = xstrdup(tag);
			sr->sr_serverattr = fattr_decode(attr);
			if (sr->sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}

			fup->expand = keyword_decode_expand(expand);
			if (fup->expand == -1)
				return (-1);
			error = fup_prepare(fup, name);
			if (error)
				return (-1);

			fup->wantmd5 = xstrdup(wantmd5);
			error = updater_diff(up, fup);
			break;
		case 'u':
			/* Update dead checked-out file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			if (attr == NULL || line != NULL)
				return (-1);

			error = fup_prepare(fup, name);
			if (error)
				return (-1);
			updater_delete(fup);
			sr = &srbuf;
			sr->sr_type = SR_CHECKOUTDEAD;
			sr->sr_file = name;
			sr->sr_tag = tag;
			sr->sr_date = date;
			sr->sr_serverattr = fattr_decode(attr);
			if (sr->sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes \"%s\"\n",
				    attr);
				return (-1);
			}
			error = status_put(fup->st, sr);
			fattr_free(sr->sr_serverattr);
			if (error) {
				lprintf(-1, "Updater: %s\n",
				    status_errmsg(fup->st));
				return (-1);
			}
			break;
		case 'C':
		case 'Y':
			/* Checkout file. */
			name = proto_get_ascii(&line);
			tag = proto_get_ascii(&line);
			date = proto_get_ascii(&line);
			revnum = proto_get_ascii(&line);
			revdate = proto_get_ascii(&line);
			attr = proto_get_ascii(&line);
			if (attr == NULL || line != NULL)
				return (-1);

			sr = &fup->srbuf;
			sr->sr_type = SR_CHECKOUTLIVE;
			sr->sr_file = xstrdup(name);
			sr->sr_tag = xstrdup(tag);
			sr->sr_date = xstrdup(date);
			sr->sr_revnum = xstrdup(revnum);
			sr->sr_revdate = xstrdup(revdate);
			sr->sr_serverattr = fattr_decode(attr);
			if (sr->sr_serverattr == NULL) {
				lprintf(-1, "Updater: Bad attributes %s\n",
				    attr);
				return (-1);
			}
			t = rcsdatetotime(revdate);
			if (t == -1) {
				lprintf(-1, "Updater: Invalid RCS date: %s\n",
				    revdate);
				return (-1);
			}

			sr->sr_clientattr = fattr_new(FT_FILE, t);
			tmp = fattr_forcheckout(sr->sr_serverattr,
			    coll->co_umask);
			fattr_override(sr->sr_clientattr, tmp, FA_MASK);
			fattr_free(tmp);
			fattr_mergedefault(sr->sr_clientattr);
			error = fup_prepare(fup, name);
			if (error)
				return (-1);
			if (*cmd == 'Y')
				error = updater_checkout(up, fup, 1);
			else
				error = updater_checkout(up, fup, 0);
			break;
		case 'D':
			/* Delete file. */
			name = proto_get_ascii(&line);
			if (name == NULL || line != NULL)
				return (-1);
			error = fup_prepare(fup, name);
			if (error)
				return (-1);
			updater_delete(fup);
			error = status_delete(fup->st, fup->destpath, 0);
			if (error) {
				lprintf(-1, "Updater: %s\n",
				    status_errmsg(fup->st));
				return (-1);
			}
			break;
		case '!':
			/* Warning from server. */
			msg = proto_get_rest(&line);
			if (msg == NULL)
				return (-1);
			lprintf(-1, "Server warning: %s\n", msg);
			break;
		default:
			lprintf(-1, "Updater: Unknown command: "
			    "\"%s\"\n", cmd);
			return (-1);
		}
		if (error)
			return (-1);
		fup_cleanup(fup);
	}
	if (line == NULL)
		return (-1);
	return (0);
}

/* Delete file. */
static void
updater_delete(struct file_update *fup)
{
	struct coll *coll;
	int error;

	/* XXX - delete limit handling */
	coll = fup->coll;
	if (coll->co_options & CO_DELETE) {
		lprintf(1, " Delete %s\n", fup->coname);
		error = fattr_delete(fup->destpath);
		if (error) {
			lprintf(-1, "Cannot delete \"%s\": %s\n",
			    fup->destpath, strerror(errno));
			return;
		}
		if (coll->co_options & CO_CHECKOUTMODE)
			updater_prunedirs(coll->co_prefix, fup->destpath);
	} else {
		lprintf(1," NoDelete %s\n", fup->coname);
	}
}

static int
updater_setattrs(struct file_update *fup, char *name, char *tag, char *date,
    char *revnum, char *revdate, struct fattr *rcsattr)
{
	struct statusrec sr;
	struct status *st;
	struct coll *coll;
	struct fattr *fileattr, *fa;
	char *path;
	int error, rv;

	coll = fup->coll;
	st = fup->st;
	path = fup->destpath;

	fileattr = fattr_frompath(path, FATTR_NOFOLLOW);
	if (fileattr == NULL) {
		/* The file has vanished. */
		error = status_delete(st, name, 0);
		if (error) {
			lprintf(-1, "Updater: %s\n", status_errmsg(st));
			return (-1);
		}
		return (0);
	}
	fa = fattr_forcheckout(rcsattr, coll->co_umask);
	fattr_override(fileattr, fa, FA_MASK);
	fattr_free(fa);

	rv = fattr_install(fileattr, path, NULL);
	if (rv == -1) {
		lprintf(-1, "Cannot set attributes for \"%s\": %s\n", path,
		    strerror(errno));
		fattr_free(fileattr);
		return (-1);
	}
	if (rv == 1) {
		lprintf(1, " SetAttrs %s\n", fup->coname);
		fattr_free(fileattr);
		fileattr = fattr_frompath(path, FATTR_NOFOLLOW);
		if (fileattr == NULL) {
			/* We're being very unlucky. */
			error = status_delete(st, name, 0);
			if (error) {
				lprintf(-1, "Updater: %s\n", status_errmsg(st));
				return (-1);
			}
			return (-1);
		}
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

	error = status_put(st, &sr);
	fattr_free(fileattr);
	if (error) {
		lprintf(-1, "Updater: %s\n", status_errmsg(st));
		return (-1);
	}
	return (0);
}

/*
 * Check that the file we created/updated has a correct MD5 checksum.
 * If it doesn't and that this is not a fixup update, add a fixup
 * request to checkout the whole file.  If it's already a fixup update,
 * we just fail.
 */
static void
updater_checkmd5(struct updater *up, struct file_update *fup, const char *md5,
    int isfixup)
{
	struct statusrec *sr;

	sr = &fup->srbuf;
	if (strcmp(fup->wantmd5, md5) == 0)
		return;
	if (isfixup) {
		lprintf(-1, "%s: Checksum mismatch -- file not updated\n",
		    fup->destpath);
		return;
	}
	lprintf(-1, "%s: Checksum mismatch -- will transfer entire file\n",
	    fup->destpath);
	fixups_put(up->config->fixups, fup->coll, sr->sr_file);
}

static int
updater_updatefile(struct file_update *fup, const char *to, const char *from)
{
	struct coll *coll;
	struct status *st;
	struct statusrec *sr;
	struct fattr *fileattr;
	int error, rv;

	coll = fup->coll;
	sr = &fup->srbuf;
	st = fup->st;

	fattr_umask(sr->sr_clientattr, coll->co_umask);
	rv = fattr_install(sr->sr_clientattr, to, from);
	if (rv == -1)
		return (-1);

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
	fileattr = fattr_frompath(to, FATTR_NOFOLLOW);
	if (fileattr == NULL) {
		lprintf(-1, "Updater: Cannot stat \"%s\": %s\n", to,
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

	error = status_put(st, sr);
	if (error) {
		lprintf(-1, "Updater: %s\n", status_errmsg(st));
		return (-1);
	}
	return (0);
}

static int
updater_diff(struct updater *up, struct file_update *fup)
{
	char md5[MD5_DIGEST_SIZE];
	struct coll *coll;
	struct statusrec *sr;
	struct fattr *fa, *tmp;
	char *author, *path, *revnum, *revdate;
	char *line, *cmd, *temppath;
	int error;

	temppath = NULL;
	coll = fup->coll;
	sr = &fup->srbuf;
	path = fup->destpath;

	lprintf(1, " Edit %s\n", fup->coname);
	while ((line = stream_getln(up->rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		cmd = proto_get_ascii(&line);
		if (cmd == NULL || strcmp(cmd, "D") != 0)
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
		if (fup->author != NULL)
			free(fup->author);
		sr->sr_revnum = xstrdup(revnum);
		sr->sr_revdate = xstrdup(revdate);
		fup->author = xstrdup(author);
		if (fup->orig == NULL) {
			/* First patch, the "origin" file is the one we have. */
			fup->orig = stream_open_file(path, O_RDONLY);
			if (fup->orig == NULL)
				goto bad;
		} else {
			/* Subsequent patches. */
			stream_close(fup->orig);
			fup->orig = fup->to;
			stream_rewind(fup->orig);
			unlink(temppath);
			free(temppath);
		}
		temppath = tempname(path);
		fup->to = stream_open_file(temppath,
		    O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fup->to == NULL)
			goto bad;
		lprintf(2, "  Add delta %s %s %s\n", sr->sr_revnum,
		    sr->sr_revdate, fup->author);
		error = updater_diff_batch(up, fup);
		if (error)
			goto bad;
	}
	if (line == NULL)
		goto bad;

	fa = fattr_frompath(path, FATTR_FOLLOW);
	tmp = fattr_forcheckout(sr->sr_serverattr, coll->co_umask);
	fattr_override(fa, tmp, FA_MASK);
	fattr_free(tmp);
	fattr_maskout(fa, FA_MODTIME);
	sr->sr_clientattr = fa;

	error = updater_updatefile(fup, path, temppath);
	if (error)
		goto bad;

	if (MD5_File(path, md5) == -1)
		goto bad;
	updater_checkmd5(up, fup, md5, 0);
	free(temppath);
	return (0);
bad:
	if (temppath != NULL)
		free(temppath);
	return (-1);
}

static int
updater_diff_batch(struct updater *up, struct file_update *fup)
{
	struct stream *rd;
	char *cmd, *line, *state, *tok;
	int error;

	state = NULL;
	rd = up->rd;
	while ((line = stream_getln(rd, NULL)) != NULL) {
		if (strcmp(line, ".") == 0)
			break;
		cmd = proto_get_ascii(&line);
		if (cmd == NULL || strlen(cmd) != 1)
			goto bad;
		switch (cmd[0]) {
		case 'L':
			line = stream_getln(rd, NULL);
			/* XXX - We're just eating the log for now. */
			while (line != NULL && strcmp(line, ".") != 0 &&
			    strcmp(line, ".+") != 0)
				line = stream_getln(rd, NULL);
			if (line == NULL)
				goto bad;
			break;
		case 'S':
			tok = proto_get_ascii(&line);
			if (tok == NULL || line != NULL)
				goto bad;
			if (state != NULL)
				free(state);
			state = xstrdup(tok);
			break;
		case 'T':
			error = updater_diff_apply(up, fup, state);
			if (error)
				goto bad;
			break;
		default:
			goto bad;
		}
	}
	if (line == NULL)
		goto bad;
	if (state != NULL)
		free(state);
	return (0);
bad:
	if (state != NULL)
		free(state);
	lprintf(-1, "Updater: Protocol error\n");
	return (-1);
}

int
updater_diff_apply(struct updater *up, struct file_update *fup, char *state)
{
	struct diffinfo dibuf, *di;
	struct coll *coll;
	struct statusrec *sr;
	int error;

	coll = fup->coll;
	sr = &fup->srbuf;
	di = &dibuf;

	di->di_rcsfile = sr->sr_file;
	di->di_cvsroot = coll->co_cvsroot;
	di->di_revnum = sr->sr_revnum;
	di->di_revdate = sr->sr_revdate;
	di->di_author = fup->author;
	di->di_tag = sr->sr_tag;
	di->di_state = state;
	di->di_expand = fup->expand;

	error = diff_apply(up->rd, fup->orig, fup->to, coll->co_keyword, di);
	if (error) {
		lprintf(-1, "Updater: Bad diff from server\n");
		return (-1);
	}
	return (0);
}

/* XXX check write errors */
static int
updater_checkout(struct updater *up, struct file_update *fup, int isfixup)
{
	char md5[MD5_DIGEST_SIZE];
	struct statusrec *sr;
	struct coll *coll;
	struct stream *to;
	char *cmd, *path, *line;
	size_t size;
	int error, first;

	coll = fup->coll;
	sr = &fup->srbuf;
	path = fup->destpath;

	if (isfixup)
		lprintf(1, " Fixup %s\n", fup->coname);
	else
		lprintf(1, " Checkout %s\n", fup->coname);
	error = mkdirhier(path, coll->co_umask);
	if (error) {
		lprintf(-1, "Cannot create directories leading to \"%s\": %s\n",
		    path, strerror(errno));
		return (-1);
	}

	to = stream_open_file(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (to == NULL) {
		lprintf(-1, "%s: Cannot create: %s\n", path, strerror(errno));
		return (-1);
	}
	stream_filter_start(to, STREAM_FILTER_MD5, md5);
	line = stream_getln(up->rd, &size);
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
		line = stream_getln(up->rd, &size);
		first = 0;
	}
	if (line == NULL) {
		stream_close(to);
		return (-1);
	}
	if (size == 1 && *line == '.')
		stream_write(to, "\n", 1);
	stream_close(to);
	/* Get the checksum line. */
	line = stream_getln(up->rd, NULL);
	cmd = proto_get_ascii(&line);
	fup->wantmd5 = proto_get_ascii(&line);
	if (fup->wantmd5 == NULL || line != NULL || strcmp(cmd, "5") != 0)
		return (-1);
	updater_checkmd5(up, fup, md5, isfixup);
	fup->wantmd5 = NULL;	/* So that it doesn't get freed. */
	error = updater_updatefile(fup, path, NULL);
	if (error)
		return (error);
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
		*cp = '\0';
		if (strcmp(base, file) == 0)
			return;
		error = rmdir(file);
		if (error)
			return;
	}
}
