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
 * $Id$
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "keyword.h"
#include "misc.h"
#include "parse.h"
#include "token.h"

static struct coll *coll_alloc(void);

extern FILE *yyin;

/* These are globals because I can't think of a better way with yacc. */
static struct coll *cur_coll;
static struct coll *defaults;
static struct config *config;
static const char *cfgfile;

/*
 * Extract all the configuration information from the config
 * file and some command line parameters.
 */
struct config *
config_init(const char *file, char *host, char *base, char *colldir,
    uint16_t port, int compress, int truststatus)
{
	struct coll *cur;
	mode_t mask;
	size_t slen;
	char *prefix;
	int error;

	config = xmalloc(sizeof(struct config));
	config->host = NULL;
	STAILQ_INIT(&config->colls);

	/* Set default collection options. */
	defaults = coll_alloc();
	mask = umask(0);
	umask(mask);
	defaults->co_umask = mask;
	defaults->co_options = CO_SETMODE | CO_EXACTRCS | CO_CHECKRCS;

	/* Extract a list of collections from the configuration file. */
	cur_coll = coll_new();
	yyin = fopen(file, "r");
	if (yyin == NULL) {
		lprintf(-1, "Cannot open \"%s\": %s\n", file,
		    strerror(errno));
		exit(1);
	}
	cfgfile = file;
	error = yyparse();
	fclose(yyin);
	if (error)
		goto bad;

	/* Fixup the list of collections. */
	STAILQ_FOREACH(cur, &config->colls, co_next) {
		if (cur->co_release == NULL) {
			lprintf(-1, "Release not specified for collection "
			    "\"%s\"\n", cur->co_name);
			exit(1);
		}
		if (cur->co_tag == NULL && cur->co_date == NULL) {
			lprintf(-1, "Client only supports checkout mode\n");
			exit(1);
		}
		cur->co_options |= CO_CHECKOUTMODE;
		if (cur->co_tag == NULL)
			cur->co_tag = xstrdup(".");
		if (cur->co_date == NULL)
			cur->co_date = xstrdup(".");
		if (base != NULL) {
			if (cur->co_base != NULL)
				free(cur->co_base);
			cur->co_base = xstrdup(base);
 		} else if (cur->co_base == NULL) {
			cur->co_base = xstrdup("/usr/local/etc/cvsup");
		}
		if (cur->co_prefix == NULL) {
			cur->co_prefix = xstrdup(cur->co_base);
		/*
		 * If prefix is not an absolute pathname, it is
		 * interpreted relative to base.
		 */
		} else if (cur->co_prefix[0] != '/') {
			slen = strlen(cur->co_base);
			if (slen > 0 && cur->co_base[slen - 1] != '/')
				xasprintf(&prefix, "%s/%s", cur->co_base,
				    cur->co_prefix);
			else
				xasprintf(&prefix, "%s%s", cur->co_base,
				    cur->co_prefix);
			free(cur->co_prefix);
			cur->co_prefix = prefix;
		}
		cur->co_prefixlen = strlen(cur->co_prefix);
		if (compress > 0)
			cur->co_options |= CO_COMPRESS;
		else if (compress < 0)
			cur->co_options &= ~CO_COMPRESS;
		if (truststatus)
			cur->co_options |= CO_TRUSTSTATUSFILE;
		if (colldir)
			cur->co_colldir = colldir;
		else
			cur->co_colldir = "sup";
	}

	/* Override host if necessary. */
	if (host) {
		free(config->host);
		config->host = host;
	}
	config->port = port;
	coll_free(cur_coll);
	coll_free(defaults);
	return (config);
bad:
	coll_free(cur_coll);
	coll_free(defaults);
	if (config->host != host)
		free(config->host);
	free(config);
	return (NULL);
}

/*
 * Kludge because it seems I can't pass anything nor get anything back
 * from the yacc parser properly, without resorting to global variables.
 */
int
config_sethost(char *host)
{

	if (config->host != NULL) {
		lprintf(-1, "All \"host\" fields in the supfile "
		    "must be the same\n");
		exit(1);
	}
	config->host = host;
	return (0);
}

/* Create a new collection, inheriting options from the default collection. */
struct coll *
coll_new(void)
{
	struct coll *new;

	new = coll_alloc();
	new->co_options = defaults->co_options;
	new->co_umask = defaults->co_umask;
	if (defaults->co_base != NULL)
		new->co_base = xstrdup(defaults->co_base);
	if (defaults->co_date != NULL)
		new->co_date = xstrdup(defaults->co_date);
	if (defaults->co_prefix != NULL)
		new->co_prefix = xstrdup(defaults->co_prefix);
	if (defaults->co_release != NULL)
		new->co_release = xstrdup(defaults->co_release);
	if (defaults->co_tag != NULL)
		new->co_tag = xstrdup(defaults->co_tag);
	return (new);
}

char *
coll_statuspath(struct coll *coll)
{
	char *path;

	if (coll->co_options & CO_USERELSUFFIX) {
		xasprintf(&path, "%s/%s/%s/checkouts.%s:%s", coll->co_base,
		    coll->co_colldir, coll->co_name, coll->co_release,
		    coll->co_tag);
	} else {
		xasprintf(&path, "%s/%s/%s/checkouts", coll->co_base,
		    coll->co_colldir, coll->co_name);
	}
	return (path);
}

void
coll_add(char *name)
{

	cur_coll->co_name = name;
	STAILQ_INSERT_TAIL(&config->colls, cur_coll, co_next);
	cur_coll = coll_new();
}

void
coll_free(struct coll *coll)
{

	if (coll == NULL)
		return;
	if (coll->co_base != NULL)
		free(coll->co_base);
	if (coll->co_base != NULL)
		free(coll->co_date);
	if (coll->co_base != NULL)
		free(coll->co_prefix);
	if (coll->co_base != NULL)
		free(coll->co_release);
	if (coll->co_base != NULL)
		free(coll->co_tag);
	if (coll->co_base != NULL)
		free(coll->co_cvsroot);
	if (coll->co_base != NULL)
		free(coll->co_name);
	keyword_free(coll->co_keyword);
	free(coll);
}

void
coll_setopt(int opt, char *value)
{
	struct coll *coll;

	coll = cur_coll;
	switch (opt) {
	case PT_BASE:
		if (coll->co_base != NULL)
			free(coll->co_base);
		coll->co_base = value;
		break;
	case PT_DATE:
		if (coll->co_base != NULL)
			free(coll->co_date);
		coll->co_date = value;
		break;
	case PT_PREFIX:
		if (coll->co_base != NULL)
			free(coll->co_prefix);
		coll->co_prefix = value;
		break;
	case PT_RELEASE:
		if (coll->co_base != NULL)
			free(coll->co_release);
		coll->co_release = value;
		break;
	case PT_TAG:
		if (coll->co_base != NULL)
			free(coll->co_tag);
		coll->co_tag = value;
		break;
	case PT_UMASK:
		errno = 0;
		coll->co_umask = strtol(value, NULL, 8);
		free(value);
		if (errno) {
			lprintf(-1, "Parse error in \"%s\": Invalid "
			    "umask value\n", cfgfile);
			exit(1);
		}
		break;
	case PT_USE_REL_SUFFIX:
		coll->co_options |= CO_USERELSUFFIX;
		break;
	case PT_DELETE:
		coll->co_options |= CO_DELETE;
		break;
	case PT_COMPRESS:
		coll->co_options |= CO_COMPRESS;
		break;
	}
}

/* Set "coll" as being the default collection. */
void
coll_setdef(void)
{

	coll_free(defaults);
	defaults = cur_coll;
	cur_coll = coll_new();
}

/* Allocate a zero'ed collection structure. */
static struct coll *
coll_alloc(void)
{
	struct coll *new;

	new = xmalloc(sizeof(struct coll));
	memset(new, 0, sizeof(struct coll));
	return (new);
}
