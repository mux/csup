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

#include <sys/time.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>	/* XXX - debug */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fattr.h"
#include "fattr_os.h"

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

int
fattr_supported(int type)
{

	return (fattr_support[type]);
}

/* Returns a new file attribute structure based on a stat structure. */
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

	fa->mask = FA_FILETYPE | fattr_supported(fa->type);
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
fattr_frompath(const char *path)
{
	struct fattr *fa;
	struct stat sb;
	int error;

	error = stat(path, &sb);
	if (error)
		return (NULL);
	fa = fattr_fromstat(&sb);
	return (fa);
}

struct fattr *
fattr_fromfd(int fd)
{
	struct fattr *fa;
	struct stat sb;
	int error;

	error = fstat(fd, &sb);
	if (error)
		return (NULL);
	fa = fattr_fromstat(&sb);
	return (fa);
}

/* Returns a new file attribute structure from its encoded text form. */
struct fattr *
fattr_decode(char *attr)
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
	if (fa->mask & FA_MODTIME)
		next = fattr_scanattr(fa, FA_MODTIME, next);
	if (fa->mask & FA_SIZE)
		next = fattr_scanattr(fa, FA_SIZE, next);
	if (fa->mask & FA_LINKTARGET)
		next = fattr_scanattr(fa, FA_LINKTARGET, next);
	if (fa->mask & FA_RDEV)
		next = fattr_scanattr(fa, FA_RDEV, next);
	if (fa->mask & FA_OWNER)
		next = fattr_scanattr(fa, FA_OWNER, next);
	if (fa->mask & FA_GROUP)
	    next = fattr_scanattr(fa, FA_GROUP, next);
	if (fa->mask & FA_MODE)
		next = fattr_scanattr(fa, FA_MODE, next);
	if (fa->mask & FA_FLAGS)
		next = fattr_scanattr(fa, FA_FLAGS, next);
	if (fa->mask & FA_LINKCOUNT) {
		next = fattr_scanattr(fa, FA_LINKCOUNT, next);
	} else if (fattr_supported(fa->type) & FA_LINKCOUNT) {
		/* If the link count is missing but supported, fake it as 1. */
		fa->mask |= FA_LINKCOUNT;
		fa->linkcount = 1;
	}
	if (fa->mask & FA_DEV)
		next = fattr_scanattr(fa, FA_DEV, next);
	if (fa->mask & FA_INODE)
		next = fattr_scanattr(fa, FA_INODE, next);
	if (next == NULL || *next != '\0')
		goto bad;
	return (fa);
bad:
	fattr_free(fa);
	return (NULL);
}

struct fattr *
fattr_dup(struct fattr *from)
{
	struct fattr *fa;

	fa = fattr_new();
	fattr_override(fa, from, FA_MASK);
	return (fa);
}

void
fattr_free(struct fattr *fa)
{

	free(fa->linktarget);
	free(fa);
}

void
fattr_maskout(struct fattr *fa, int mask)
{

	/* Don't forget to free() the linktarget attribute if we remove it. */
	if (mask & FA_LINKTARGET && fa->mask & FA_LINKTARGET) {
		free(fa->linktarget);
		fa->linktarget = NULL;
	}
	fa->mask &= ~mask;
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

	if (attr == NULL)
		return (NULL);
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

/* Return a file attribute structure built from the RCS file attributes. */
struct fattr *
fattr_forcheckout(struct fattr *rcsattr, mode_t mask)
{
	struct fattr *fa;

	fa = fattr_new();
	fattr_merge(fa, rcsattr);
	if (rcsattr->mask & FA_MODE) {
		if ((rcsattr->mode & 0111) > 0)
			fa->mode = 0777;
		else
			fa->mode = 0666;
		fa->mode &= ~mask;
		fa->mask |= FA_MODE;
	}
	fa->mask |= FA_FILETYPE;
	fa->type = FT_FILE;

	/*
	 * If the link count attribute is supported by this file type,
	 * set it to 1.
	 */
	if (fattr_supported(FT_FILE) & FA_LINKCOUNT) {
		fa->linkcount = 1;
		fa->mask |= FA_LINKCOUNT;
	}
	return (fa);
}

/* Merge attributes from "from" that aren't present in "fa". */
void
fattr_merge(struct fattr *fa, struct fattr *from)
{
	
	fattr_override(fa, from, from->mask & ~fa->mask);
}

/* Override selected attributes of "fa" with values from "from". */
void
fattr_override(struct fattr *fa, struct fattr *from, int mask)
{

	mask &= from->mask;
	fa->mask |= mask;
	if (mask & FA_FILETYPE)
		fa->type = from->type;
	if (mask & FA_MODTIME)
		fa->modtime = from->modtime;
	if (mask & FA_SIZE)
		fa->size = from->size;
	if (mask & FA_LINKTARGET) {
		free(fa->linktarget);
		fa->linktarget = strdup(from->linktarget);
		if (fa->linktarget == NULL)
			err(1, "strdup");
	}
	if (mask & FA_RDEV)
		fa->rdev = from->rdev;
	if (mask & FA_OWNER)
		fa->uid = from->uid;
	if (mask & FA_GROUP)
		fa->gid = from->gid;
	if (mask & FA_MODE)
		fa->mode = from->mode;
	if (mask & FA_FLAGS)
		fa->flags = from->flags;
	if (mask & FA_LINKCOUNT)
		fa->linkcount = from->linkcount;
	if (mask & FA_DEV)
		fa->dev = from->dev;
	if (mask & FA_INODE)
		fa->inode = from->inode;
}

/*
 * Changes those attributes we can change.  Returns -1 on error,
 * 0 if no update was needed, and 1 if an update was needed and
 * it has been applied successfully.
 */
int
fattr_apply(struct fattr *fa, int fd)
{
	struct timeval tv[2];
	struct fattr *old;
	int error;
	mode_t mask, newmode;
	uid_t uid;
	gid_t gid;

	old = fattr_fromfd(fd);
	if (old == NULL)
		return (-1);
	if (fattr_cmp(fa, old) == 0)
		return (0);
	printf("%s: need to update\n", __func__);
	if (fa->mask & FA_OWNER && fa->mask & FA_GROUP)
		mask = FA_SETIDMASK | FA_PERMMASK;
	else
		mask = FA_PERMMASK;
	if (fa->mask & FA_MODTIME) {
		gettimeofday(tv, NULL);		/* Access time. */
		tv[1].tv_sec = fa->modtime;	/* Modification time. */
		tv[1].tv_usec = 0;
		error = futimes(fd, tv);
		if (error)
			return (-1);
	}
	if (fa->mask & FA_OWNER || fa->mask & FA_GROUP) {
		uid = -1;
		gid = -1;
		if (fa->mask & FA_OWNER)
			uid = fa->uid;
		if (fa->mask & FA_GROUP)
			gid = fa->gid;
		error = fchown(fd, uid, gid);
		if (error)
			return (-1);
	}
	if (fa->mask & FA_MODE) {
		newmode = fa->mode & mask;
		/* Merge in set*id bits from the old attribute. */
		if (old->mask & FA_MODE) {
			newmode |= (old->mode &~ mask);
			newmode &= (FA_SETIDMASK | FA_PERMMASK);
		}
		error = fchmod(fd, newmode);
		if (error)
			return (-1);
	}
	if (fa->mask & FA_FLAGS) {
		error = fchflags(fd, fa->flags);
		if (error)
			return (-1);
	}
	return (1);
}

/*
 * Returns 0 if both attributes are equal, and -1 otherwise.
 *
 * This function only compares attributes that are valid in both
 * files.  A file of unknown type ("FT_UNKNOWN") is unequal to
 * anything, including itself.
 */
int
fattr_cmp(struct fattr *fa1, struct fattr *fa2)
{
	int mask;

	mask = fa1->mask & fa2->mask;
	assert(mask & FA_FILETYPE);
	if (fa1->type == FT_UNKNOWN || fa2->type == FT_UNKNOWN)
		return (-1);
	if (mask & FA_MODTIME)
		if (fa1->modtime != fa2->modtime)
			return (-1);
	if (mask & FA_SIZE)
		if (fa1->size != fa2->size)
			return (-1);
	if (mask & FA_LINKTARGET)
		if (strcmp(fa1->linktarget, fa2->linktarget) != 0)
			return (-1);
	if (mask & FA_RDEV)
		if (fa1->rdev != fa2->rdev)
			return (-1);
	if (mask & FA_OWNER)
		if (fa1->uid != fa2->uid)
			return (-1);
	if (mask & FA_GROUP)
		if (fa1->gid != fa2->gid)
			return (-1);
	if (mask & FA_MODE)
		if (fa1->mode != fa2->mode)
			return (-1);
	if (mask & FA_FLAGS)
		if (fa1->flags != fa2->flags)
			return (-1);
	if (mask & FA_LINKCOUNT)
		if (fa1->linkcount != fa2->linkcount)
			return (-1);
	if (mask & FA_DEV)
		if (fa1->dev != fa2->dev)
			return (-1);
	if (mask & FA_INODE)
		if (fa1->inode != fa2->inode)
			return (-1);
	return (0);
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