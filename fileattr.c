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
 *
 * $Id$
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>	/* XXX - Only used by debug function fattr_printf(). */
#include <stdlib.h>
#include <string.h>

#include "fileattr.h"
#include "osdep.h"

#define	FA_MASK		0x0fff

#define	FA_MASKRADIX		16
#define	FA_FILETYPERADIX	10
#define	FA_MODTIMERADIX		10
#define	FA_SIZERADIX		10
#define	FA_MODERADIX		8
#define	FA_FLAGSRADIX		16
#define	FA_LINKCOUNTRADIX	10
#define	FA_INODERADIX		10

#define	FA_PERMMASK		(S_IRWXU | S_IRWXG | S_IRWXO)
#define	FA_SETIDMASK		(S_ISUID | S_ISGID | S_ISVTX)

struct fattr {
	int		mask;
	int		type;
	time_t		modtime;
	off_t		size;
	char		*linktarget;
	dev_t		rdev;
	uid_t		uid;
	gid_t		gid;
	mode_t		mode;
	fflags_t	flags;
	nlink_t		linkcount;
	dev_t		dev;
	ino_t		inode;
};

static struct fattr	*fattr_new(void);
static char		*fattr_scanattr(struct fattr *, int, char *);

struct fattr_support *
fattr_support(void)
{

	return (&fattr_supported);
}

struct fattr *
fattr_fromstat(struct stat *sb)
{
	struct fattr *fa;

	fa = fattr_new();
	if (S_ISREG(sb->st_mode))
		fa->type = FT_FILE;
	else if (S_ISDIR(sb->st_mode))
		fa->type = FT_DIRECTORY;
	else if (S_ISCHR(sb->st_mode))
		fa->type = FT_CDEV;
	else if (S_ISBLK(sb->st_mode))
		fa->type = FT_BDEV;
	else if (S_ISLNK(sb->st_mode))
		fa->type = FT_SYMLINK;
	else
		fa->type = FT_UNKNOWN;

	fa->mask = FA_FILETYPE | fattr_supported.attrs[fa->type];
	if (fa->mask & FA_MODTIME)
		fa->modtime = sb->st_mtime;
	if (fa->mask & FA_SIZE)
		fa->size = sb->st_size;
	if (fa->mask & FA_RDEV)
		fa->rdev = sb->st_rdev;
	if (fa->mask & FA_OWNER)
		fa->uid = sb->st_uid;
	if (fa->mask & FA_GROUP)
		fa->gid = sb->st_gid;
	if (fa->mask & FA_MODE)
		fa->mode = sb->st_mode & (FA_SETIDMASK | FA_PERMMASK);
	if (fa->mask & FA_FLAGS)
		fa->flags = sb->st_flags;
	if (fa->mask & FA_LINKCOUNT)
		fa->linkcount = sb->st_nlink;
	if (fa->mask & FA_DEV)
		fa->dev = sb->st_dev;
	if (fa->mask & FA_INODE)
		fa->inode = sb->st_ino;
	return (fa);
}

struct fattr *
fattr_parse(char *attr)
{
	struct fattr *fa;
	char *next;

	fa = fattr_new();
	next = fattr_scanattr(fa, FA_MASK, attr);
	if (next == NULL || (fa->mask & ~FA_MASK) > 0)
		goto bad;
	if (fa->mask & FA_FILETYPE) {
		next = fattr_scanattr(fa, FA_FILETYPE, next);
		if (next == NULL)
			goto bad;
		if (fa->type < 0 || fa->type > FT_MAX)
			fa->type = FT_UNKNOWN;
	} else {
		/* The filetype attribute is always valid. */
		fa->mask |= FA_FILETYPE;
		fa->type = FT_UNKNOWN;
	}
	if (fa->mask & FA_MODTIME) {
		next = fattr_scanattr(fa, FA_MODTIME, next);
		if (next == NULL)
			goto bad;
	}
	if (fa->mask & FA_SIZE) {
		next = fattr_scanattr(fa, FA_SIZE, next);
		if (next == NULL)
			goto bad;
	}
	if (fa->mask & FA_MODE) {
		next = fattr_scanattr(fa, FA_MODE, next);
		if (next == NULL)
			goto bad;
	}
	return (fa);
bad:
	fattr_free(fa);
	return (NULL);
}

void
fattr_free(struct fattr *fa)
{

	free(fa->linktarget);
	free(fa);
}

static struct fattr *
fattr_new(void)
{
	struct fattr *new;

	new = malloc(sizeof(struct fattr));
	if (new == NULL)
		err(1, "malloc");
	memset(new, 0, sizeof(struct fattr));
	return (new);
}

/*
 * Eat the specified attribute and put it in the file attribute
 * structure.  Returns NULL on error, or a pointer to the next
 * attribute to parse.
 *
 * This would be much prettier if we had strnto{l, ul, ll, ull}().
 * Besides, we need to use a (unsigned) long long types here because
 * some attributes may need 64bits to fit.
 */
static char *
fattr_scanattr(struct fattr *fa, int type, char *attr)
{
	struct passwd *pw;
	char *attrend, *attrstart, *end;
	size_t len;
	unsigned long attrlen;
	int modemask;
	char tmp;

	errno = 0;
	attrlen = strtoul(attr, &end, 10);
	if (errno || *end != '#')
		return (NULL);
	len = strlen(attr);
	attrstart = end + 1;
	attrend = attrstart + attrlen;
	tmp = *attrend;
	*attrend = '\0';
	switch (type) {
	/* Using FA_MASK here is a bit bogus semantically. */
	case FA_MASK:
		errno = 0;
		fa->mask = (int)strtol(attrstart, &end, FA_MASKRADIX);
		if (errno || end != attrend)
			return (NULL);
		break;
	case FA_FILETYPE:
		errno = 0;
		fa->type = (int)strtol(attrstart, &end, FA_FILETYPERADIX);
		break;
	case FA_MODTIME:
		errno = 0;
		fa->modtime = (time_t)strtoll(attrstart, &end, FA_MODTIMERADIX);
		if (errno || end != attrend)
			return (NULL);
		break;
	case FA_SIZE:
		errno = 0;
		fa->size = (off_t)strtoll(attrstart, &end, FA_SIZERADIX);
		if (errno || end != attrend)
			return (NULL);
		break;
	case FA_LINKTARGET:
		fa->linktarget = strdup(attrstart);
		if (fa->linktarget == NULL)
			err(1, "strdup");
		break;
	case FA_RDEV:
		break;
	case FA_OWNER:
		pw = getpwnam(attrstart);
		if (pw != NULL)
			fa->uid = pw->pw_uid;
		else
			fa->mask &= ~FA_OWNER;
		break;
	case FA_GROUP:
		pw = getpwnam(attrstart);
		if (pw != NULL)
			fa->gid = pw->pw_gid;
		else
			fa->mask &= ~FA_GROUP;
		break;
	case FA_MODE:
		errno = 0;
		fa->mode = (mode_t)strtol(attrstart, &end, FA_MODERADIX);
		if (errno || end != attrend)
			return (NULL);
		if (fa->mask & FA_OWNER && fa->mask & FA_GROUP)
			modemask = FA_SETIDMASK | FA_PERMMASK;
		else
			modemask = FA_PERMMASK;
		fa->mode &= modemask;
		break;
	case FA_FLAGS:
		errno = 0;
		fa->flags = (fflags_t)strtoul(attrstart, &end, FA_FLAGSRADIX);
		if (errno || end != attrend)
			return (NULL);
		break;
	case FA_LINKCOUNT:
		errno = 0;
		fa->linkcount = (nlink_t)strtol(attrstart, &end, FA_FLAGSRADIX);
		if (errno || end != attrend)
			return (NULL);
		break;
	case FA_DEV:
		break;
	case FA_INODE:
		break;
	}
	*attrend = tmp;
	return (attrend);
}

#if 0
void
fattr_print(struct fattr *fa)
{

	printf("Mask is %#x\n", fa->mask);
	if (fa->mask & FA_FILETYPE)
		printf("Type is %d\n", fa->type);
	if (fa->mask & FA_MODTIME)
		printf("Modification time is %s\n", ctime(&fa->modtime));
	if (fa->mask & FA_SIZE)
		printf("Size is %lld\n", (long long)fa->size);
	if (fa->mask & FA_MODE)
		printf("Mode is %o\n", fa->mode);
}
#endif
