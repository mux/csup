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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "config.h"
#include "parse.h"

extern struct collection *cur_collec;

%}

%union {
	char *str;
	int i;
}

%token DEFAULT
%token <i> BASE HOST PREFIX RELEASE TAG UMASK
%token <i> COMPRESS DELETE USE_REL_SUFFIX
%token EQUAL
%token <str> STRING

%type <i> boolean
%type <i> name

%%

config_file	
	: config_list
       	|
       	;

config_list
        : config
        | config_list config
        ;

config
	: default_line
	| collection
	;

default_line
	: DEFAULT options
		{
			collec_setdef(cur_collec);
			cur_collec = collec_new();
		}
	;

collection
	: STRING options
		{
			collec_add(cur_collec, $1);
			cur_collec = collec_new();
		}
	;

options
	:
	| options option
	;

option
	: boolean
		{ options_set(cur_collec, $1, NULL); }
	| value
	;

boolean
	: COMPRESS
	| DELETE
	| USE_REL_SUFFIX
	;

value
	: name EQUAL STRING
		{ options_set(cur_collec, $1, $3); }
	;

name
	: BASE
	| HOST
	| PREFIX
	| RELEASE
	| TAG
	| UMASK
	;

%%
