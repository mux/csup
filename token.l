%{
/*-
 * Copyright (c) 2003, Maxime Henrion <mux@FreeBSD.org>
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

#include <err.h>
#include <stdio.h>
#include <string.h>

#include "y.tab.h"
#include "parse.h"

#define	YY_NO_UNPUT

int lineno = 1;

%}

%%

[ \t]+			;
#.*$			;
\*default		{ return DEFAULT; }
base			{ yylval.i = BASE; return BASE; }
date			{ yylval.i = DATE; return DATE; }
host			{ yylval.i = HOST; return HOST; }
prefix			{ yylval.i = PREFIX; return PREFIX; }
release			{ yylval.i = RELEASE; return RELEASE; }
tag			{ yylval.i = TAG; return TAG; }
umask			{ yylval.i = UMASK; return UMASK; }
=			{ return EQUAL; }
compress		{ yylval.i = COMPRESS; return COMPRESS; }
delete			{ yylval.i = DELETE; return DELETE; }
use-rel-suffix		{ yylval.i = USE_REL_SUFFIX; return USE_REL_SUFFIX; }
[a-zA-Z0-9./_-]+	{
			  yylval.str = strdup(yytext);
			  if (yylval.str == NULL)
			  	err(1, "strdup");
			  return STRING;
			}
\n			lineno++;

%%

void
yyerror(const char *s)
{
	errx(1, "Parse error line %d: %s\n%s", lineno, s, yytext);
}
