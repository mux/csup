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
#include <sys/stat.h>

#include <stdlib.h>

#include "fileattr.h"
#include "osdep.h"

struct fileattr_support *
fileattr_support(void)
{

	return (&fileattr_supported);
}

struct fileattr *
fileattr_fromstat(struct stat *sb)
{
	struct fileattr *fa;

	fa = malloc(sizeof(struct fileattr));
	if (fa == NULL)
		return (NULL);
	switch (sb->st_mode & S_IFMT) {
	case S_IFREG:
		fa->type = FT_FILE;
		break;
	case S_IFDIR:
		fa->type = FT_DIRECTORY;
		break;
	case S_IFCHR:
		fa->type = FT_CDEV;
		break;
	case S_IFBLK:
		fa->type = FT_BDEV;
		break;
	case S_IFLNK:
		fa->type = FT_SYMLINK;
		break;
	default:
		fa->type = FT_UNKNOWN;
		break;
	}
	fa->mask = FA_FILETYPE | fileattr_supported.attrs[fa->type];
	if (fa->mask & FA_MODTIME)
		fa->modtime = sb->st_mtime;
	if (fa->mask & FA_SIZE)
		fa->size = sb->st_size;
	if (fa->mask & FA_RDEV)
		fa->rdev = sb->st_rdev;
	if (fa->mask & FA_OWNER)
		fa->owner = sb->st_uid;
	if (fa->mask & FA_GROUP)
		fa->group = sb->st_gid;
	if (fa->mask & FA_MODE)
		fa->mode = sb->st_mode & (S_IRWXU | S_IRWXG | S_IRWXO |
		    S_ISUID | S_ISGID | S_ISTXT);
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
