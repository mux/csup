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
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/queue.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diff.h"
#include "keyword.h"
#include "stream.h"

enum rcskey {
	RCSKEY_AUTHOR,
	RCSKEY_CVSHEADER,
	RCSKEY_DATE,
	RCSKEY_HEADER,
	RCSKEY_ID,
	RCSKEY_LOCKER,
	RCSKEY_LOG,
	RCSKEY_NAME,
	RCSKEY_RCSFILE,
	RCSKEY_REVISION,
	RCSKEY_SOURCE,
	RCSKEY_STATE
};

typedef enum rcskey rcskey_t;

struct tag {
	char *ident;
	rcskey_t key;
	STAILQ_ENTRY(tag) next;
};

static struct tag	*tag_new(const char *, rcskey_t);
static char		*tag_expand(struct tag *, struct diff *);
static void		 tag_free(struct tag *);

struct keyword {
	STAILQ_HEAD(, tag) keywords;		/* Enabled keywords. */
	STAILQ_HEAD(, tag) aliases;		/* Aliases. */
};

/* Default CVS keywords. */
static struct {
	const char *ident;
	rcskey_t key;
} tag_defaults[] = {
	{ "Author",	RCSKEY_AUTHOR, },
	{ "CVSHeader",	RCSKEY_CVSHEADER, },
	{ "Date",	RCSKEY_DATE, },
	{ "Header",	RCSKEY_HEADER, },
	{ "Id",		RCSKEY_ID, },
	{ "Locker",	RCSKEY_LOCKER, },
	{ "Log",	RCSKEY_LOG, },
	{ "Name",	RCSKEY_NAME, },
	{ "RCSfile",	RCSKEY_RCSFILE,	},
	{ "Revision",	RCSKEY_REVISION,},
	{ "Source",	RCSKEY_SOURCE, },
	{ "State",	RCSKEY_STATE, },
	{ NULL,		0, }
};

struct keyword *
keyword_new(void)
{
	struct keyword *new;

	new = malloc(sizeof(struct keyword));
	if (new == NULL)
		return (NULL);
	STAILQ_INIT(&new->keywords);
	STAILQ_INIT(&new->aliases);
	return (new);
}

void
keyword_free(struct keyword *keyword)
{
	struct tag *tag;

	while (!STAILQ_EMPTY(&keyword->keywords)) {
		tag = STAILQ_FIRST(&keyword->keywords);
		STAILQ_REMOVE_HEAD(&keyword->keywords, next);
		tag_free(tag);
	}
	while (!STAILQ_EMPTY(&keyword->aliases)) {
		tag = STAILQ_FIRST(&keyword->aliases);
		STAILQ_REMOVE_HEAD(&keyword->aliases, next);
		tag_free(tag);
	}
	free(keyword);
}

int
keyword_alias(struct keyword *keyword, char *ident, char *rcskey)
{
	struct tag *new;
	int i;

	i = 0;
	while (tag_defaults[i].ident != NULL) {
		if (strcmp(tag_defaults[i].ident, rcskey) == 0) {
			new = tag_new(ident, tag_defaults[i].key);
			if (new == NULL)
				return (-1);
			STAILQ_INSERT_HEAD(&keyword->aliases, new, next);
			return (0);
		}
		i++;
	}
	errno = ENOENT;
	return (-1);
}

int
keyword_enable(struct keyword *keyword, char *ident)
{
	struct tag *tag;
	int all, i;

	all = 0;
	if (strcmp(ident, ".") == 0)
		all = 1;

	for (i = 0; tag_defaults[i].ident != NULL; i++) {
		if (!all) {
			if (strcmp(tag_defaults[i].ident, ident) != 0)
				continue;
			tag = tag_new(tag_defaults[i].ident,
			    tag_defaults[i].key);
			if (tag == NULL)
				return (-1);
			STAILQ_INSERT_TAIL(&keyword->keywords, tag, next);
			return (0);
		} else {
			tag = tag_new(tag_defaults[i].ident,
			    tag_defaults[i].key);
			if (tag == NULL)
				return (-1);
			STAILQ_INSERT_TAIL(&keyword->keywords, tag, next);
		}
	}
	STAILQ_FOREACH(tag, &keyword->aliases, next) {
		if (!all) {
			if (strcmp(tag->ident, ident) != 0)
				continue;
			tag = tag_new(tag->ident, tag->key);
			if (tag == NULL)
				return (-1);
			STAILQ_INSERT_TAIL(&keyword->keywords, tag, next);
			return (0);
		} else {
			tag = tag_new(tag->ident, tag->key);
			if (tag == NULL)
				return (-1);
			STAILQ_INSERT_TAIL(&keyword->keywords, tag, next);
		}
	}
	if (!all) {
		errno = ENOENT;
		return (-1);
	}
	return (0);
}

int
keyword_disable(struct keyword *keyword, char *ident)
{
	struct tag *tag;
	int all;

	all = 0;
	if (strcmp(ident, ".") == 0)
		all = 1;

	if (all) {
		while (!STAILQ_EMPTY(&keyword->keywords)) {
			tag = STAILQ_FIRST(&keyword->keywords);
			STAILQ_REMOVE_HEAD(&keyword->keywords, next);
			tag_free(tag);
		}
		return (0);
	}
	STAILQ_FOREACH(tag, &keyword->keywords, next) {
		if (strcmp(tag->ident, ident) != 0)
		       continue;
		STAILQ_REMOVE(&keyword->keywords, tag, tag, next);
		tag_free(tag);
		return (0);
	}
	errno = ENOENT;
	return (-1);
}

/*
 * Expand appropriate RCS keywords.  If there's no tag to expand,
 * keyword_expand() returns line, otherwise it returns a malloc()'ed
 * string that needs to be freed by the caller after use.
 */
char *
keyword_expand(struct keyword *keyword, struct diff *diff, char *line)
{
	struct tag *tag;
	char *dollar, *keystart, *valstart, *vallim;
	char *newline, *newval;

	dollar = strchr(line, '$');
	if (dollar == NULL)
		return (line);
	keystart = dollar + 1;
	vallim = strchr(keystart, '$');
	if (vallim == NULL || vallim == keystart)
		return (line);
	valstart = strchr(keystart, ':');
	if (valstart == keystart)
		return (line);
	if (valstart == NULL || valstart > vallim)
		valstart = vallim;
	STAILQ_FOREACH(tag, &keyword->keywords, next) {
		if (strncmp(tag->ident, keystart, valstart - keystart) == 0) {
			newval = tag_expand(tag, diff);
			*dollar = '\0';
			*valstart = '\0';
			*vallim = '\0';
			if (newval == NULL) {
				asprintf(&newline, "%s$%s:  $%s", line,
				    keystart, vallim + 1);
			} else {
				asprintf(&newline, "%s$%s: %s $%s", line,
				    keystart, newval, vallim + 1);
				free(newval);
			}
			return (newline);
		}
	}
	return (line);
}

static struct tag *
tag_new(const char *ident, rcskey_t key)
{
	struct tag *new;

	new = malloc(sizeof(struct tag));
	if (new == NULL)
		return (NULL);
	new->ident = strdup(ident);
	if (new->ident == NULL) {
		free(new);
		return (NULL);
	}
	new->key = key;
	return (new);
}

static void
tag_free(struct tag *tag)
{

	free(tag->ident);
	free(tag);
}

/*
 * Expand a specific tag and return the new value.  If NULL
 * is returned, the tag is empty.
 */
static char *
tag_expand(struct tag *tag, struct diff *diff)
{
	/*
	 * CVS formats dates as "XXXX/XX/XX XX:XX:XX".  32 bytes
	 * is big enough until year 10,000,000,000,000,000 :-).
	 */
	char cvsdate[32];
	struct tm tm;
	char *filename, *val;

	if (strptime(diff->d_revdate, "%Y.%m.%d.%H.%M.%S", &tm) == NULL)
		err(1, "strptime");
	if (strftime(cvsdate, sizeof(cvsdate), "%Y/%m/%d %H:%M:%S", &tm) == 0)
		err(1, "strftime");
	filename = strrchr(diff->d_rcsfile, '/');
	if (filename == NULL)
		filename = diff->d_rcsfile;
	else
		filename++;

	switch (tag->key) {
	case RCSKEY_AUTHOR:
		asprintf(&val, "%s", diff->d_author);
		break;
	case RCSKEY_CVSHEADER:
		asprintf(&val, "%s %s %s %s %s", diff->d_rcsfile,
		    diff->d_revnum, cvsdate, diff->d_author, diff->d_state);
		break;
	case RCSKEY_DATE:
		asprintf(&val, "%s", cvsdate);
		break;
	case RCSKEY_HEADER:
		asprintf(&val, "%s/%s %s %s %s %s", diff->d_cvsroot,
		    diff->d_rcsfile, diff->d_revnum, cvsdate, diff->d_author,
		    diff->d_state);
		break;
	case RCSKEY_ID:
		asprintf(&val, "%s %s %s %s %s", filename, diff->d_revnum,
		    cvsdate, diff->d_author, diff->d_state);
		break;
	case RCSKEY_LOCKER:
		/*
		 * Unimplemented even in CVSup sources.  It seems we don't
		 * even have this information sent by the server.
		 */
		return (NULL);
	case RCSKEY_NAME:
		if (diff->d_tag != NULL)
			asprintf(&val, "%s", diff->d_tag);
		else
			return (NULL);
		break;
	case RCSKEY_RCSFILE:
		asprintf(&val, "%s", filename);
		break;
	case RCSKEY_REVISION:
		asprintf(&val, "%s", diff->d_revnum);
		break;
	case RCSKEY_SOURCE:
		asprintf(&val, "%s/%s", diff->d_cvsroot, diff->d_rcsfile);
		break;
	case RCSKEY_STATE:
		asprintf(&val, "%s", diff->d_state);
		break;
	default:
		printf("%s: Unexpected tag %d.\n", __func__, tag->key);
		return (NULL);
	}
	if (val == NULL)
		err(1, "asprintf");
	return (val);
}
