/*-
 * Copyright (c) 2003-2004, Maxime Henrion <mux@FreeBSD.org>
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
__FBSDID("$Id$");

#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "fileattr.h"
#include "parse.h"
#include "y.tab.h"

static struct collection *coll_alloc(void);

extern FILE *yyin;

/* Global variables used in the yacc parser. */
struct collection *cur_coll;

static struct collection *defaults;
static struct config *config;

/*
 * Extract all the configuration information from the config
 * file and some command line parameters.
 */
struct config *
config_init(const char *file, char *host, char *base, char *colldir,
    in_port_t port)
{
	struct collection *cur;
	mode_t mask;
	int error, ret;

	config = malloc(sizeof(struct config));
	if (config == NULL)
		err(1, "malloc");
	config->host = NULL;
	STAILQ_INIT(&config->collections);
	config->supported = fileattr_support();

	defaults = coll_alloc();
	mask = umask(0);
	umask(mask);
	defaults->umask = mask;
	defaults->options = CO_SETMODE | CO_EXACTRCS | CO_CHECKRCS;

	cur_coll = coll_new();
	yyin = fopen(file, "r");
	if (yyin == NULL) {
		fprintf(stderr, "Cannot open \"%s\": %s\n", file,
		    strerror(errno));
		exit(1);
	}
	error = yyparse();
	/* Override host and/or base if necessary. */
	if (host) {
		if (config->host != NULL)
			free(config->host);
		config->host = host;
	}
	STAILQ_FOREACH(cur, &config->collections, next) {
		if (base) {
			if (cur->base != NULL)
				free(cur->base);
			cur->base = base;
		} else if (cur->base == NULL) {
			cur->base = strdup("/usr/local/etc/cvsup");
			if (cur->base == NULL)
				err(1, "malloc");
		}
		if (colldir)
			ret = asprintf(&cur->colldir, "%s/%s", cur->base,
			    colldir);
		else
			ret = asprintf(&cur->colldir, "%s/sup", cur->base);
		if (ret == -1)
			err(1, "asprintf");
		if (cur->prefix == NULL) {
			cur->prefix = strdup(cur->base);
			if (cur->prefix == NULL)
				err(1, "malloc");
		}
		if (cur->tag == NULL && cur->date == NULL) {
			fprintf(stderr, "Client only supports checkout mode\n");
			exit (1);
		}
		cur->options |= CO_CHECKOUTMODE;
		if (cur->tag == NULL) {
			cur->tag = strdup(".");
			if (cur->tag == NULL)
				err(1, "malloc");
		}
		if (cur->date == NULL) {
			cur->date = strdup(".");
			if (cur->date == NULL)
				err(1, "malloc");
		}
	}
	config->port = port;
	coll_free(cur_coll);
	coll_free(defaults);
	if (error)
		return (NULL);
	return (config);
}

/* Create a new collection, inheriting options from the default collection. */
struct collection *
coll_new(void)
{
	struct collection *new;

	new = coll_alloc();
	new->options = defaults->options;
	new->umask = defaults->umask;
	if (defaults->base != NULL) {
		new->base = strdup(defaults->base);
		if (new->base == NULL)
			err(1, "malloc");
	}
	if (defaults->prefix != NULL) {
		new->prefix = strdup(defaults->prefix);
		if (new->prefix == NULL)
			err(1, "malloc");
	}
	if (defaults->release != NULL) {
		new->release = strdup(defaults->release);
		if (new->release == NULL)
			err(1, "malloc");
	}
	if (defaults->tag != NULL) {
		new->tag = strdup(defaults->tag);
		if (new->tag == NULL)
			err(1, "malloc");
	}
	return (new);
}

void
coll_add(struct collection *coll, char *name)
{

	coll->name = name;
	STAILQ_INSERT_TAIL(&config->collections, coll, next);
}

void
coll_free(struct collection *coll)
{

	free(coll->base);
	free(coll->prefix);
	free(coll->release);
	free(coll->tag);
	free(coll->cvsroot);
	free(coll);
}

void
options_set(struct collection *coll, int opt, char *value)
{

	switch (opt) {
	case HOST:
		if (config->host != NULL) {
			fprintf(stderr, "All \"host\" fields in the supfile "
			    "must be the same\n");
			exit(1);
		}
		config->host = value;
		break;
	case BASE:
		if (coll->base != NULL)
			free(coll->base);
		coll->base = value;
		break;
	case DATE:
		if (coll->date != NULL)
			free(coll->date);
		coll->date = value;
		break;
	case PREFIX:
		if (coll->prefix != NULL)
			free(coll->base);
		coll->prefix = value;
		break;
	case RELEASE:
		if (coll->release != NULL)
			free(coll->release);
		coll->release = value;
		break;
	case TAG:
		if (coll->tag != NULL)
			free(coll->tag);
		coll->tag = value;
		break;
	case UMASK:
		coll->umask = strtol(value, NULL, 8);
		break;
	case USE_REL_SUFFIX:
		coll->options |= CO_USERELSUFFIX;
		break;
	case DELETE:
		coll->options |= CO_DELETE;
		break;
	case COMPRESS:
#ifdef notyet
		/* XXX - implement zlib compression */
		coll->options |= CO_COMPRESS;
#endif
		break;
	}
}

/* Set "coll" as being the default collection. */
void
coll_setdef(struct collection *coll)
{

	coll_free(defaults);
	defaults = coll;
}

/* Allocate a zero'ed collection structure. */
static struct collection *
coll_alloc(void)
{
	struct collection *new;

	new = malloc(sizeof(struct collection));
	if (new == NULL)
		err(1, "malloc");
	memset(new, 0, sizeof(*new));
	return (new);
}
