/*-
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rcslex.h"

struct rcslex {
	int fd;
	char *data;
	size_t datalen;
	int eof;
	char *last;
	size_t offset;
	struct rcstok tok;
};

/* Map the RCS file in memory and initialize state variables. */
struct rcslex *
rcslex_new(const char *path)
{
	struct stat sb;
	struct rcslex *lex;
	void *addr;
	int error;

	lex = malloc(sizeof(struct rcslex));
	if (lex == NULL)
		return (NULL);
	memset(lex, 0, sizeof(struct rcslex));

	lex->fd = open(path, O_RDONLY);
	if (lex->fd == -1) {
		rcslex_free(lex);
		return (NULL);
	}

	error = fstat(lex->fd, &sb);
	if (error) {
		rcslex_free(lex);
		return (NULL);
	}

	lex->datalen = sb.st_size;
	addr = mmap(NULL, lex->datalen, PROT_READ, 0, lex->fd, 0);
	if (addr == MAP_FAILED) {
		rcslex_free(lex);
		return (NULL);
	}
	lex->data = addr;
	lex->last = lex->data + lex->datalen - 1;
	lex->eof = 0;
	lex->offset = 0;
	memset(&lex->tok, 0, sizeof(lex->tok));
	return (lex);
}

/*
 * Lex and return the next token. There aren't many things the lexer can
 * distinguish from because most of the token types depend on the structure
 * of the RCS file. So we mostly eat whitespace and match semicolons, RCS
 * strings or regular tokens.
 */
struct rcstok *
rcslex_get(struct rcslex *lex)
{
	char *cp, *sep;
	struct rcstok *tok;

	if (lex->eof)
		return (NULL);

	/* Eat whitespace. */
	cp = lex->data + lex->offset;
	while (cp <= lex->last && isspace(*cp))
		cp++;

	/* Return 0 if we hit EOF. */
	if (cp >= lex->last) {
		lex->eof = 1;
		return (NULL);
	}

	tok = &lex->tok;
	tok->value = cp;
	if (*cp == '@') {
		/* This is a possibly binary RCS string, find its end. */
		cp++;
		tok->value = cp;
		tok->type = -1;
		while (tok->type == -1 && cp <= lex->last) {
			sep = memchr(cp, '@', lex->last - cp);
			if (sep == NULL)
				return (NULL);
			if (sep == lex->last || sep[1] != '@') {
				tok->type = RCSLEX_STRING;
				tok->len = sep - tok->value;
				cp = sep + 1;
			} else {
				cp = sep + 2;
			}
		}
		if (tok->type == -1)
			return (NULL);
	} else if (*cp == ';') {
		tok->type = RCSLEX_SCOLON;
		tok->len = 1;
		cp++;
	} else if (*cp == ':') {
		tok->type = RCSLEX_COLON;
		tok->len = 1;
		cp++;
	} else {
		/* This is a regular symbol (sym, num, id or a keyword). */
		while (cp <= lex->last && *cp != '@' && *cp != ';' &&
		    *cp != ':' && !isspace(*cp))
			cp++;
		tok->type = RCSLEX_ID;
		tok->len = cp - tok->value;
	}

	lex->offset = cp - lex->data;
	return (tok);
}

/* Get and check that the returned token matches what we want. */
struct rcstok *
rcslex_want(struct rcslex *lex, int type, size_t len, const char *value)
{
	struct rcstok *tok;

	tok = rcslex_get(lex);
	if (tok == NULL)
		return (NULL);
	if (tok->type != type)
		return (NULL);
	if (len > 0 && tok->len != len)
		return (NULL);
	if (value != NULL && memcmp(value, tok->value, tok->len) != 0)
		return (NULL);
	return (tok);
}

char *
rcslex_get_num(struct rcslex *lex)
{
	struct rcstok *tok;

	tok = rcslex_want(lex, RCSLEX_ID, 0, NULL);
	if (tok == NULL)
		return (NULL);
	if (!rcstok_validate_num(tok))
		return (NULL);
	return (rcslex_dup(lex, NULL));
}

char *
rcslex_get_id(struct rcslex *lex)
{
	struct rcstok *tok;

	tok = rcslex_want(lex, RCSLEX_ID, 0, NULL);
	if (tok == NULL)
		return (NULL);
	if (!rcstok_validate_id(tok))
		return (NULL);
	return (rcslex_dup(lex, NULL));
}

char *
rcslex_get_sym(struct rcslex *lex)
{
	const char *special = "$,.:;@";
	struct rcstok *tok;
	size_t i;
	int c, idchar;

	tok = rcslex_want(lex, RCSLEX_ID, 0, NULL);
	if (tok == NULL)
		return (NULL);

	/* Validate that this is a proper "sym" token. */
	idchar = 0;
	for (i = 0; i < tok->len; i++) {
		c = tok->value[i];
		if (strchr(special, c) != NULL || !isprint(c))
			return (NULL);
		if (!idchar && !isdigit(c))
			idchar = 1;
	}
	/* There needs to be at least one "idchar" character. */
	if (!idchar)
		return (NULL);
	return (rcslex_dup(lex, NULL));
}

char *
rcslex_get_string(struct rcslex *lex, size_t *outlen)
{
	struct rcstok *tok;

	tok = rcslex_want(lex, RCSLEX_STRING, 0, NULL);
	if (tok == NULL)
		return (NULL);
	return (rcslex_dup(lex, outlen));
}

/* Extract/convert the raw token in newly allocated memory. */
char *
rcslex_dup(struct rcslex *lex, size_t *outlen)
{
	struct rcstok *tok;
	char *value;
	size_t len;

	/* We do not actually convert the doubled '@' characters in the
	   RCS strings because they are expected this way later on. */
	tok = &lex->tok;
	/* We always terminate tokens with a NUL character, even RCS strings
	   that can legitimately be binary strings. This is because it is
	   convenient to treat some tokens (such as the parameter of "expand")
	   as C-style strings. */
	len = tok->len + 1;
	value = malloc(len);
	if (value == NULL)
		return (NULL);
	memcpy(value, tok->value, tok->len);
	value[tok->len] = '\0';
	if (outlen != NULL) {
		if (tok->type == RCSLEX_STRING)
			len--;
		*outlen = len;
	}
	return (value);
}

void
rcslex_unget(struct rcslex *lex)
{
	struct rcstok *tok;

	tok = &lex->tok;
	lex->offset -= tok->len;
}

/* Did we hit EOF? */
int
rcslex_eof(struct rcslex *lex)
{

	return (lex->eof);
}

/* Release all resources associated with the lexer. */
void
rcslex_free(struct rcslex *lex)
{
	int error;

	if (lex->data != NULL) {
		error = munmap(lex->data, lex->datalen);
		assert(!error);
	}
	if (lex->fd != -1) {
		error = close(lex->fd);
		assert(!error);
	}
	free(lex);
}

/* Validate that we have indeed an "id" token. */
int
rcstok_validate_id(struct rcstok *tok)
{
	const char *special = "$,:;@";
	int idchar, c;
	size_t i;

	if (tok->type != RCSLEX_ID)
		return (0);
	idchar = 0;
	for (i = 0; i < tok->len; i++) {
		c = tok->value[i];
		if (strchr(special, c) != NULL || !isprint(c))
			return (0);
		if (!idchar && !isdigit(c) && c != '.')
			idchar = 1;
	}
	/* There needs to be at least one "idchar" character. */
	if (!idchar)
		return (0);
	return (1);
}

/* Validate that we have indeed a "num" token. */
int
rcstok_validate_num(struct rcstok *tok)
{
	size_t i;
	int c;

	if (tok->type != RCSLEX_ID)
		return (0);
	for (i = 0; i < tok->len; i++) {
		c = tok->value[i];
		if (!isdigit(c) && c != '.')
			return (0);
	}
	return (1);
}
