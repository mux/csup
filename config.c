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
#include "parse.h"
#include "y.tab.h"

extern FILE *yyin;

/* Global variables used in the yacc parser. */
struct collection *cur_collec;

static struct collection *defaults;
static struct config *config;

/*
 * Extract all the configuration information from the config
 * file and some commnd line parameters.
 */
struct config *
config_init(const char *file, char *host, char *base, in_port_t port)
{
	struct collection *cur;
	mode_t mask;
	int error;

	config = malloc(sizeof(struct config));
	if (config == NULL)
		err(1, "malloc");
	config->host = NULL;
	STAILQ_INIT(&config->collections);
	/* 
	 * Initialize the supported file types, that is define which
	 * attributes we support for each file type.  This is OS
	 * specific, and should be put elsewhere if we ever want this
	 * code to be portable on other platforms.  However, these
	 * settings should be correct for every UNIX system out there.
	 */
	config->ftypes[FT_UNKNOWN] = 0;
	config->ftypes[FT_FILE] = FA_FILETYPE | FA_MODTIME | FA_SIZE |
	    FA_OWNER | FA_GROUP | FA_MODE | FA_FLAGS | FA_LINKCOUNT |
	    FA_INODE | FA_DEV;
	config->ftypes[FT_DIRECTORY] = FA_FILETYPE | FA_OWNER | FA_GROUP
	    | FA_MODE | FA_FLAGS;
	config->ftypes[FT_CDEV] = FA_FILETYPE | FA_RDEV | FA_OWNER | FA_GROUP |
	    FA_MODE | FA_FLAGS | FA_LINKCOUNT | FA_DEV | FA_INODE;
	config->ftypes[FT_BDEV] = FA_FILETYPE | FA_RDEV | FA_OWNER | FA_GROUP |
	    FA_MODE | FA_FLAGS | FA_LINKCOUNT | FA_DEV | FA_INODE;
	config->ftypes[FT_SYMLINK] = FA_FILETYPE | FA_LINKTARGET;

	defaults = malloc(sizeof(struct collection));
	if (defaults == NULL)
		err(1, "malloc");
	mask = umask(0);
	umask(mask);
	defaults->base = NULL;
	defaults->umask = mask;
	defaults->prefix = NULL;
	defaults->release = NULL;
	defaults->tag = NULL;
	defaults->options = CO_SETMODE | CO_EXACTRCS | CO_CHECKRCS;

	cur_collec = collec_new();
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
	collec_free(cur_collec);
	collec_free(defaults);
	if (error)
		return (NULL);
	return (config);
}

struct collection *
collec_new(void)
{
	struct collection *new;

	new = calloc(1, sizeof(struct collection));
	if (new == NULL)
		err(1, "malloc");
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
collec_add(struct collection *collec, char *name)
{

	collec->name = name;
	STAILQ_INSERT_TAIL(&config->collections, collec, next);
}

#ifdef DEBUG
void
config_print(struct config *cfg)
{
	struct collection *cur;
	int i;

	printf("Host \"%s\"\n", cfg->host);
	printf("Printing all collections\n");
	STAILQ_FOREACH(cur, &cfg->collections, next)
		printf("Collection %s\n  base=%s\n  prefix=%s\n  tag=%s\n"
		    "  release=%s\n  umask=%#o\n  options=%d\n",
		    cur->name, cur->base, cur->prefix, cur->tag, cur->release,
		    cur->umask, cur->options);
	printf("File attributes\n");
	for (i = 0; i < FT_NUMBER; i++)
		printf("  %x\n", cfg->ftypes[i]);
}
#endif

/*
 * XXX - relies on a C99 compliant free() function,
 * ie: free(NULL) is allowed.
 */
void
collec_free(struct collection *collec)
{

	free(collec->base);
	free(collec->prefix);
	free(collec->release);
	free(collec->tag);
	free(collec);
}

void
options_set(struct collection *collec, int opt, char *value)
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
		if (collec->base != NULL)
			free(collec->base);
		collec->base = value;
		break;
	case DATE:
		if (collec->date != NULL)
			free(collec->date);
		collec->date = value;
		break;
	case PREFIX:
		if (collec->prefix != NULL)
			free(collec->base);
		collec->prefix = value;
		break;
	case RELEASE:
		if (collec->release != NULL)
			free(collec->release);
		collec->release = value;
		break;
	case TAG:
		if (collec->tag != NULL)
			free(collec->tag);
		collec->tag = value;
		break;
	case UMASK:
		collec->umask = strtol(value, NULL, 8);
		break;
	case USE_REL_SUFFIX:
		collec->options |= CO_USERELSUFFIX;
		break;
	case DELETE:
		collec->options |= CO_DELETE;
		break;
	case COMPRESS:
#ifdef notyet
		/* XXX - implement zlib compression */
		collec->options |= CO_COMPRESS;
#endif
		break;
	}
}

void
collec_setdef(struct collection *collec)
{

	collec_free(defaults);
	defaults = collec;
}
