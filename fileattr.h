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
 * $FreeBSD$
 */
#include <sys/types.h>
#include <sys/queue.h>

/*
 * File types.
 */
#define	FT_UNKNOWN	0			/* Unknown file type. */
#define	FT_FILE		1			/* Regular file. */
#define	FT_DIRECTORY	2			/* Directory. */
#define	FT_CDEV		3			/* Character device. */
#define	FT_BDEV		4			/* Block device. */
#define	FT_SYMLINK	5			/* Symbolic link. */
#define FT_MAX		FT_SYMLINK

/*
 * File attributes.
 */
#define	FA_FILETYPE	0x0001		/* True for all supported file types. */
#define	FA_MODTIME	0x0002
#define	FA_SIZE		0x0004
#define	FA_LINKTARGET	0x0008		/* Target of a symbolic link. */
#define	FA_RDEV		0x0010		/* Device for a device node. */
#define	FA_OWNER	0x0020
#define	FA_GROUP	0x0040
#define	FA_MODE		0x0080
#define	FA_FLAGS	0x0100		/* 4.4BSD flags, a la chflags(2). */
#define	FA_LINKCOUNT	0x0200		/* Hard link count. */
#define	FA_DEV		0x0400		/* Device holding the inode. */
#define	FA_INODE	0x0800		/* Inode number. */

#define	FA_MASK		0x0fff

/* Attributes that we might be able to change. */
#define	FA_CHANGEABLE	(FA_MODTIME | FA_OWNER | FA_GROUP | FA_MODE | FA_FLAGS)

/*
 * Attributes that we don't want to save in the "checkouts" file
 * when in checkout mode.
 */
#define	FA_COIGNORE	(FA_MASK & ~(FA_FILETYPE|FA_MODTIME|FA_SIZE|FA_MODE))

struct stat;

struct fileattr_support {
	int number;
	int attrs[FT_MAX + 1];
};

struct fileattr {
	char		*name;
	int		mask;
	int		type;
	time_t		modtime;
	off_t		size;
	char		*linktarget;
	dev_t		rdev;
	uid_t		owner;
	gid_t		group;
	mode_t		mode;
	fflags_t	flags;
	nlink_t		linkcount;
	dev_t		dev;
	ino_t		inode;
	STAILQ_ENTRY(file) next;
};

struct fileattr		*fileattr_fromstat(struct stat *);
struct fileattr_support	*fileattr_support(void);
