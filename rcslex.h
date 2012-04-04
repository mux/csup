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
#ifndef _RCSLEX_H_
#define	_RCSLEX_H_

#include <string.h>	/* For strlen() and memcmp() in the macros. */

/* Token types for the lexer. */
#define	RCSLEX_SCOLON		0
#define	RCSLEX_COLON		1
#define	RCSLEX_ID		2
#define	RCSLEX_STRING		3


struct rcslex;

struct rcstok {
	int type;
	char *value;
	size_t len;
};

struct rcslex	*rcslex_new(const char *);
struct rcstok	*rcslex_get(struct rcslex *);
char		*rcslex_dup(struct rcslex *, size_t *);
struct rcstok	*rcslex_want(struct rcslex *, int, size_t, const char *);
void		 rcslex_unget(struct rcslex *);
int		 rcslex_eof(struct rcslex *);
void		 rcslex_free(struct rcslex *);

char		*rcslex_get_num(struct rcslex *);
char		*rcslex_get_id(struct rcslex *);
char		*rcslex_get_sym(struct rcslex *);
char		*rcslex_get_string(struct rcslex *, size_t *);

#define	rcslex_want_kw(lex, kw)	rcslex_want(lex, RCSLEX_ID, strlen(kw), kw)
#define	rcslex_want_id(lex)	rcslex_want(lex, RCSLEX_ID, 0, NULL)
#define	rcslex_want_string(lex)	rcslex_want(lex, RCSLEX_STRING, 0, NULL)
#define	rcslex_want_scolon(lex)	rcslex_want(lex, RCSLEX_SCOLON, 1, NULL)
#define	rcslex_want_colon(lex)	rcslex_want(lex, RCSLEX_COLON, 1, NULL)

#define	rcstok_is_kw(tok, kw)	(tok->type == RCSLEX_ID &&	\
				 tok->len == strlen(kw) &&	\
				 memcmp(tok->value, kw, tok->len) == 0)

int	rcstok_validate_id(struct rcstok *);
int	rcstok_validate_num(struct rcstok *);

#endif /* !_RCSLEX_H_ */
