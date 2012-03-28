/*-
 * Copyright (c) 2008-2009, Ulf Lilleengen <lulf@FreeBSD.org>
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

#include <stdio.h>
#include <stdlib.h>

#include "misc.h"
#include "queue.h"
#include "rcsfile.h"
#include "rcsparse.h"
#include "rcstokenizer.h"

/*
 * This is an RCS-parser using lex for tokenizing and makes sure the RCS syntax
 * is correct as it constructs an RCS file that is used by csup.
 */

static int	parse_admin(struct rcsfile *, yyscan_t);
static int	parse_deltas(struct rcsfile *, yyscan_t, int);
static int	parse_deltatexts(struct rcsfile *, yyscan_t, int);
static char	*duptext(yyscan_t, int *);

struct string {
	char *str;
	STAILQ_ENTRY(string) next;
};

static char *
duptext(yyscan_t sp, int *arglen)
{
	char *tmp, *val;
	int len;

	tmp = rcsget_text(sp);
	len = rcsget_leng(sp);
	val = xmalloc(len + 1);
	memcpy(val, tmp, len);
	val[len] = '\0';
	if (arglen != NULL)
		*arglen = len;
	return (val);
}

/*
 * Start up parser, and use the rcsfile hook to add objects.
 */
int
rcsparse_run(struct rcsfile *rf, FILE *infp, int ro)
{
	yyscan_t scanner;
	char *desc;
	int error, tok;

	error = 0;
	rcslex_init(&scanner);
	rcsset_in(infp, scanner);
	tok = parse_admin(rf, scanner);
	if (tok == -1)
		rcslex_destroy(scanner);
		return (-1);
	tok = parse_deltas(rf, scanner, tok);
	if (tok != KEYWORD || rcslex(scanner) != STRING)
		rcslex_destroy(scanner);
		return (-1);
	desc = duptext(scanner, NULL);
	rcsfile_setval(rf, RCSFILE_DESC, desc);
	free(desc);
	tok = rcslex(scanner);
	/* Parse deltatexts if we need to edit. */
	if (!ro)
		error = parse_deltatexts(rf, scanner, tok);
	rcslex_destroy(scanner);
	return (error);
}

/*
 * Parse the admin part of a RCS file.
 */
static int
parse_admin(struct rcsfile *rf, yyscan_t sp)
{
	char *branch, *comment, *expand, *head, *id, *revnum, *tag, *tmp;
	int strict, token;

	strict = 0;
	branch = NULL;

	/* head {num}; */
	if (rcslex(sp) != KEYWORD || rcslex(sp) != NUM)
		return (-1);
	head = duptext(sp, NULL);
	rcsfile_setval(rf, RCSFILE_HEAD, head);
	free(head);
	if (rcslex(sp) != SEMIC)
		return (-1);

	/* { branch {num}; } */
	token = rcslex(sp);
	if (token == KEYWORD_TWO) {
		if (rcslex(sp) != NUM)
			return (-1);
		branch = duptext(sp, NULL);
		rcsfile_setval(rf, RCSFILE_BRANCH, branch);
		free(branch);
		if (rcslex(sp) != SEMIC)
			return (-1);
		token = rcslex(sp);
	}

	/* access {id}*; */
	if (token != KEYWORD)
		return (-1);
	while ((token = rcslex(sp)) == ID) {
		id = duptext(sp, NULL);
		rcsfile_addaccess(rf, id);
		free(id);
	}
	if (token != SEMIC)
		return (-1);

	/* symbols {sym : num}*; */
	if (rcslex(sp) != KEYWORD)
		return (-1);
	while ((token = rcslex(sp)) == ID) {
		tag = duptext(sp, NULL);
		if (rcslex(sp) != COLON || rcslex(sp) != NUM) {
			free(tag);
			return (-1);
		}
		revnum = duptext(sp, NULL);
		rcsfile_importtag(rf, tag, revnum);
		free(tag);
		free(revnum);
	}
	if (token != SEMIC)
		return (-1);

	/* locks {id : num}*; */
	if (rcslex(sp) != KEYWORD)
		return (-1);
	while ((token = rcslex(sp)) == ID) {
		/* XXX: locks field is skipped */
		if (rcslex(sp) != COLON || rcslex(sp) != NUM)
			return (-1);
	}
	if (token != SEMIC)
		return (-1);
	token = rcslex(sp);
	while (token == KEYWORD) {
		tmp = rcsget_text(sp);

		/* {strict  ;} */
		if (!strcmp(tmp, "strict")) {
			rcsfile_setval(rf, RCSFILE_STRICT, tmp);
			if (rcslex(sp) != SEMIC)
				return (-1);
		/* { comment {string}; } */
		} else if (!strcmp(tmp, "comment")) {
			token = rcslex(sp);
			if (token == STRING) {
				comment = duptext(sp, NULL);
				rcsfile_setval(rf, RCSFILE_COMMENT, comment);
				free(comment);
			}
			if (rcslex(sp) != SEMIC)
				return (-1);
		/* { expand {string}; } */
		} else if (!strcmp(tmp, "expand")) {
			token = rcslex(sp);
			if (token == STRING) {
				expand = duptext(sp, NULL);
				rcsfile_setval(rf, RCSFILE_EXPAND, expand);
				free(expand);
			}
			if (rcslex(sp) != SEMIC)
				return (-1);
		}
		/* {newphrase }* */
		while ((token = rcslex(sp)) == ID) {
			token = rcslex(sp);
			/* XXX: newphrases ignored */
			while (token == ID || token == NUM || token == STRING ||
			    token == COLON) {
				token = rcslex(sp);
			}
			if (rcslex(sp) != SEMIC)
				return (-1);
		}
	}
	return (token);
}

/*
 * Parse RCS deltas.
 */
static int
parse_deltas(struct rcsfile *rf, yyscan_t sp, int token)
{
	STAILQ_HEAD(, string) branchlist;
	char *revnum, *revdate, *author, *state, *next;

	/* In case we don't have deltas. */
	if (token != NUM)
		return (token);
	do {
		next = NULL;
		state = NULL;

		/* num */
		revnum = duptext(sp, NULL);
		/* date num; */
		if (rcslex(sp) != KEYWORD || rcslex(sp) != NUM) {
			free(revnum);
			return (-1);
		}
		revdate = duptext(sp, NULL);
		if (rcslex(sp) != SEMIC) {
			free(revdate);
			free(revnum);
			return (-1);
		}
		/* author id; */
		if (rcslex(sp) != KEYWORD || rcslex(sp) != ID) {
			free(revdate);
			free(revnum);
			return (-1);
		}
		author = duptext(sp, NULL);
		/* state {id}; */
		if (rcslex(sp) != SEMIC || rcslex(sp) != KEYWORD) {
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		token = rcslex(sp);
		if (token == ID) {
			state = duptext(sp, NULL);
			token = rcslex(sp);
		}
		if (token != SEMIC) {
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		/* branches {num}*; */
		if (rcslex(sp) != KEYWORD) {
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		token = rcslex(sp);
		STAILQ_INIT(&branchlist);
		while (token == NUM)
			token = rcslex(sp);
		if (token != SEMIC) {
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		/* next {num}; */
		if (rcslex(sp) != KEYWORD) {
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		token = rcslex(sp);
		if (token == NUM) {
			next = duptext(sp, NULL);
			token = rcslex(sp);
		}
		if (token != SEMIC) {
			if (next != NULL)
				free(next);
			free(author);
			free(revdate);
			free(revnum);
			return (-1);
		}
		/* {newphrase }* */
		token = rcslex(sp);
		while (token == ID) {
			token = rcslex(sp);
			/* XXX: newphrases ignored. */
			while (token == ID || token == NUM || token == STRING ||
			    token == COLON) {
				token = rcslex(sp);
			}
			if (rcslex(sp) != SEMIC) {
				if (next != NULL)
					free(next);
				free(author);
				free(revdate);
				free(revnum);
				return (-1);
			}
			token = rcslex(sp);
		}
		rcsfile_importdelta(rf, revnum, revdate, author, state, next);
		free(revnum);
		free(revdate);
		free(author);
		if (state != NULL)
			free(state);
		if (next != NULL)
			free(next);
	} while (token == NUM);

	return (token);
}

/*
 * Parse RCS deltatexts.
 */
static int
parse_deltatexts(struct rcsfile *rf, yyscan_t sp, int token)
{
	struct delta *d;
	char *log, *revnum, *text;
	int error, len;

	error = 0;
	/* In case we don't have deltatexts. */
	if (token != NUM)
		return (-1);
	do {
		/* num */
		revnum = duptext(sp, NULL);
		/* Get delta we're adding text to. */
		d = rcsfile_getdelta(rf, revnum);
		free(revnum);

		/*
		 * XXX: The RCS file is corrupt, but lie and say it is ok.
		 * If it is actually broken, then the MD5 mismatch will
		 * trigger a fixup.
		 */
		if (d == NULL)
			return (0);

		/* log string */
		if (rcslex(sp) != KEYWORD || rcslex(sp) != STRING)
			return (-1);
		log = duptext(sp, &len);
		error = rcsdelta_addlog(d, log, len);
		free(log);
		if (error)
			return (-1);
		/* { newphrase }* */
		token = rcslex(sp);
		while (token == ID) {
			token = rcslex(sp);
			/* XXX: newphrases ignored. */
			while (token == ID || token == NUM || token == STRING ||
			    token == COLON) {
				token = rcslex(sp);
			}
			if (rcslex(sp) != SEMIC)
				return (-1);
			token = rcslex(sp);
		}
		/* text string */
		if (token != KEYWORD || rcslex(sp) != STRING)
			return (-1);
		text = duptext(sp, &len);
		error = rcsdelta_addtext(d, text, len);
		/*
		 * If this happens, something is wrong with the RCS file, and it
		 * should be resent.
		 */
		free(text);
		if (error)
			return (-1);
		token = rcslex(sp);
	} while (token == NUM);

	return (0);
}
