/*-
 * Copyright (c) 2008-2009, Ulf Lilleengen <lulf@FreeBSD.org>
 * Copyright (c) 2012, Maxime Henrion <mux@FreeBSD.org>
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

#include <stdlib.h>

#include "keyword.h"
#include "misc.h"
#include "rcsfile.h"
#include "rcslex.h"
#include "rcsparse.h"

static int	parse_admin(struct rcsfile *, struct rcslex *);
static int	parse_deltas(struct rcsfile *, struct rcslex *);
static int	parse_deltatexts(struct rcsfile *, struct rcslex *);

int
rcsparse(struct rcsfile *rf, const char *path, int ro)
{
	struct rcslex *lex;
	char *desc;
	size_t len;
	int error;

	lex = rcslex_new(path);
	if (lex == NULL)
		return (-1);

	error = parse_admin(rf, lex);
	if (!error)
		error = parse_deltas(rf, lex);

	if (!error) {
		if (rcslex_want_kw(lex, "desc") == NULL ||
		    (desc = rcslex_get_string(lex, &len)) == NULL)
			error = -1;
		else
			rcsfile_setval(rf, RCSFILE_DESC, desc, len);
	}
	/* Parse deltatexts only if we need to edit. */
	if (!error && !ro)
		error = parse_deltatexts(rf, lex);
	rcslex_free(lex);
	return (error);
}

/*
 * Parse the admin part of a RCS file.
 */
static int
parse_admin(struct rcsfile *rf, struct rcslex *lex)
{
	struct rcstok *tok;
	char *num, *id, *sym, *str;
	int expand;
	size_t len;

	/* head {num}; */
	if (rcslex_want_kw(lex, "head") == NULL ||
	    (num = rcslex_get_num(lex)) == NULL)
		return (-1);
	rcsfile_setval(rf, RCSFILE_HEAD, num, 0);
	if (rcslex_want_scolon(lex) == NULL)
		return (-1);

	/* { branch {num}; } */
	if ((tok = rcslex_want_id(lex)) == NULL)
		return (-1);
	if (rcstok_is_kw(tok, "branch")) {
		num = rcslex_get_num(lex);
		if (num == NULL)
			return (-1);
		rcsfile_setval(rf, RCSFILE_BRANCH, num, 0);
		if (rcslex_want_scolon(lex) == NULL)
			return (-1);
		tok = rcslex_get(lex);
	}

	/* access {id}*; */
	if (!rcstok_is_kw(tok, "access"))
		return (-1);
	while ((tok = rcslex_get(lex)) != NULL && tok->type == RCSLEX_ID) {
		id = rcslex_dup(lex, NULL);
		rcsfile_addaccess(rf, id);
	}
	if (tok == NULL || tok->type != RCSLEX_SCOLON)
		return (-1);

	/* symbols {sym : num}*; */
	if (rcslex_want_kw(lex, "symbols") == NULL)
		return (-1);
	while ((tok = rcslex_get(lex)) != NULL && tok->type == RCSLEX_ID) {
		sym = rcslex_dup(lex, NULL);
		if (sym == NULL)
			return (-1);
		if (rcslex_want_colon(lex) == NULL ||
		    (num = rcslex_get_num(lex)) == NULL) {
			free(sym);
			return (-1);
		}
		rcsfile_importtag(rf, sym, num);
	}
	if (tok == NULL || tok->type != RCSLEX_SCOLON)
		return (-1);

	/* locks {id : num}*; */
	/* XXX The locks are skipped, is this correct? */
	if (rcslex_want_kw(lex, "locks") == NULL)
		return (-1);
	while ((tok = rcslex_get(lex)) != NULL && tok->type == RCSLEX_ID) {
		if (rcslex_want_colon(lex) == NULL ||
		    /* Technically, the following token is a "num", but we
		       skip it anyway so we don't validate it. */
		    rcslex_want_id(lex) == NULL)
			return (-1);
	}
	if (tok == NULL || tok->type != RCSLEX_SCOLON)
		return (-1);

	while ((tok = rcslex_get(lex)) != NULL && tok->type == RCSLEX_ID) {
		/* { strict; } */
		if (rcstok_is_kw(tok, "strict")) {
			rcsfile_setstrict(rf);
			if (rcslex_want_scolon(lex) == NULL)
				return (-1);
		/* { comment {string}; } */
		} else if (rcstok_is_kw(tok, "comment")) {
			str = rcslex_get_string(lex, &len);
			if (str == NULL)
				return (-1);
			rcsfile_setval(rf, RCSFILE_COMMENT, str, len);
			if (rcslex_want_scolon(lex) == NULL)
				return (-1);
		/* { expand {string}; } */
		} else if (rcstok_is_kw(tok, "expand")) {
			str = rcslex_get_string(lex, &len);
			if (str == NULL)
				return (-1);
			expand = keyword_decode_expand(str);
			free(str);
			if (expand == -1)
				return (-1);
			rcsfile_setexpand(rf, expand);
			if (rcslex_want_scolon(lex) == NULL)
				return (-1);
		/* { newphrase }* */
		} else if (rcstok_validate_id(tok)) {
			while ((tok = rcslex_get(lex)) != NULL &&
			    (tok->type == RCSLEX_ID ||
			     tok->type == RCSLEX_STRING ||
			     tok->type == RCSLEX_COLON))
				;
			if (tok == NULL || tok->type != RCSLEX_SCOLON)
				return (-1);
		} else {
			rcslex_unget(lex);
			break;
		}
	}
	return (0);
}

/*
 * Parse RCS deltas.
 */
static int
parse_deltas(struct rcsfile *rf, struct rcslex *lex)
{
	char *revnum, *revdate, *author, *state, *next;
	struct rcstok *tok;
	int error;

	revnum = NULL;
	revdate = NULL;
	author = NULL;
	state = NULL;
	next = NULL;

	error = 0;
	tok = rcslex_get(lex);
	while (tok != NULL && !error) {
		/* num */
		if (!rcstok_validate_num(tok)) {
			/* We reached the end of the deltas. */
			rcslex_unget(lex);
			break;
		}
		revnum = rcslex_dup(lex, NULL);

		error = -1;
		/* date num; */
		if (revnum == NULL ||
		    rcslex_want_kw(lex, "date") == NULL ||
		    (revdate = rcslex_get_num(lex)) == NULL ||
		    rcslex_want_scolon(lex) == NULL)
			break;
		/* author id; */
		if (rcslex_want_kw(lex, "author") == NULL ||
		    (author = rcslex_get_id(lex)) == NULL ||
		    rcslex_want_scolon(lex) == NULL)
			break;
		/* state {id}; */
		if (rcslex_want_kw(lex, "state") == NULL ||
		    (tok = rcslex_get(lex)) == NULL)
			break;
		if (tok->type == RCSLEX_ID && rcstok_validate_id(tok)) {
			state = rcslex_dup(lex, NULL);
			tok = rcslex_get(lex);
		}
		if (tok == NULL || tok->type != RCSLEX_SCOLON)
			break;
		/* branches {num}*; */
		if (rcslex_want_kw(lex, "branches") == NULL ||
		    (tok = rcslex_get(lex)) == NULL)
			break;
		while (tok != NULL && tok->type == RCSLEX_ID &&
		    rcstok_validate_num(tok)) {
			/* XXX We ignore branch revisions here, is this ok? */
			tok = rcslex_get(lex);
		}
		if (tok == NULL || tok->type != RCSLEX_SCOLON)
			break;
		/* next {num}; */
		if (rcslex_want_kw(lex, "next") == NULL ||
		    (tok = rcslex_get(lex)) == NULL)
			break;
		if (tok->type == RCSLEX_ID && rcstok_validate_num(tok)) {
			next = rcslex_dup(lex, NULL);
			tok = rcslex_get(lex);
		}
		if (tok == NULL || tok->type != RCSLEX_SCOLON)
			break;
		/* { newphrase }* */
		tok = rcslex_get(lex);
		while (tok != NULL && !rcstok_is_kw(tok, "desc") &&
		    rcstok_validate_id(tok)) {
			while ((tok = rcslex_get(lex)) != NULL &&
			    (tok->type == RCSLEX_ID ||
			     tok->type == RCSLEX_STRING ||
			     tok->type == RCSLEX_COLON))
				;
			if (tok == NULL || tok->type != RCSLEX_SCOLON) {
				tok = NULL;
				break;
			}
			tok = rcslex_get(lex);
		}
		if (tok == NULL)
			break;
		rcsfile_importdelta(rf, revnum, revdate, author, state, next);
		error = 0;

		free(revnum);
		free(revdate);
		free(author);
		if (state != NULL)
			free(state);
		if (next != NULL)
			free(next);
		revnum = NULL;
		revdate = NULL;
		author = NULL;
		state = NULL;
		next = NULL;
	}

	if (revnum != NULL)
		free(revnum);
	if (revdate != NULL)
		free(revdate);
	if (author != NULL)
		free(author);
	if (state != NULL)
		free(state);
	if (next != NULL)
		free(next);
	return (error);
}

/*
 * Parse RCS deltatexts.
 */
static int
parse_deltatexts(struct rcsfile *rf, struct rcslex *lex)
{
	struct delta *d;
	char *revnum;
	struct rcstok *tok;
	int error;

	/* num */
	while ((revnum = rcslex_get_num(lex)) != NULL) {
		d = rcsfile_getdelta(rf, revnum);
		free(revnum);

		/*
		 * XXX: The RCS file is corrupt, but lie and say it is ok.
		 * If it is actually broken, then the MD5 mismatch will
		 * trigger a fixup.
		 */
		if (d == NULL)
			return (0);

		error = -1;
		/* log string */
		if (rcslex_want_kw(lex, "log") == NULL ||
		    (tok = rcslex_want_string(lex)) == NULL)
			return (-1);
		error = rcsdelta_addlog(d, tok->value, tok->len);
		if (error)
			return (-1);

		/* { newphrase }* */
		tok = rcslex_get(lex);
		while (tok != NULL && !rcstok_is_kw(tok, "text") &&
		    rcstok_validate_id(tok)) {
			while ((tok = rcslex_get(lex)) != NULL &&
			    (tok->type == RCSLEX_ID ||
			     tok->type == RCSLEX_STRING ||
			     tok->type == RCSLEX_COLON))
				;
			if (tok == NULL || tok->type != RCSLEX_SCOLON) {
				tok = NULL;
				break;
			}
			tok = rcslex_get(lex);
		}
		/* text string */
		if (tok == NULL || !rcstok_is_kw(tok, "text") ||
		    (tok = rcslex_want_string(lex)) == NULL)
			return (-1);
		error = rcsdelta_addtext(d, tok->value, tok->len);
		/*
		 * If this happens, something is wrong with the RCS file, and
		 * it should be resent.
		 */
		if (error)
			return (-1);
	}
	error = !rcslex_eof(lex);
	return (error);
}
