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

#ifdef __FreeBSD__
#include <osreldate.h>
#endif

/* Define fflags_t if we're on a system that doesn't have it. */
#if !defined(__FreeBSD_version) || __FreeBSD_version < 500030
typedef uint32_t fflags_t;
#endif

/*
 * File types.
 */
#define	FT_UNKNOWN	0			/* Unknown file type. */
#define	FT_FILE		1			/* Regular file. */
#define	FT_DIRECTORY	2			/* Directory. */
#define	FT_CDEV		3			/* Character device. */
#define	FT_BDEV		4			/* Block device. */
#define	FT_SYMLINK	5			/* Symbolic link. */
#define	FT_MAX		FT_SYMLINK		/* Maximum file type number. */
#define	FT_NUMBER	(FT_MAX + 1)		/* Number of file types. */

/*
 * File attributes.
 */
#define	FA_FILETYPE	0x0001		/* True for all supported file types. */
#define	FA_MODTIME	0x0002		/* Last file modification time. */
#define	FA_SIZE		0x0004		/* Size of the file. */
#define	FA_LINKTARGET	0x0008		/* Target of a symbolic link. */
#define	FA_RDEV		0x0010		/* Device for a device node. */
#define	FA_OWNER	0x0020		/* Owner of the file. */
#define	FA_GROUP	0x0040		/* Group of the file. */
#define	FA_MODE		0x0080		/* File permissions. */
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
struct fattr;

struct fattr		*fattr_fromstat(struct stat *);
struct fattr		*fattr_frompath(const char *);
struct fattr		*fattr_fromfd(int);
struct fattr		*fattr_decode(char *);
struct fattr		*fattr_forcheckout(struct fattr *, mode_t);
struct fattr		*fattr_dup(struct fattr *);
void			 fattr_maskout(struct fattr *, int);
void			 fattr_merge(struct fattr *, struct fattr *);
void			 fattr_override(struct fattr *, struct fattr *, int);
int			 fattr_apply(struct fattr *, int);
int			 fattr_cmp(struct fattr *, struct fattr *);
void			 fattr_free(struct fattr *);
int			 fattr_supported(int);