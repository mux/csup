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

/*
 * The file types we support and the attributes we support for each
 * one.  This is OS-dependant, but those defaults should work on most
 * UNIX systems out there.  We put this in its own header file so
 * that it's easier if we want to port that code to platforms with
 * different file types support.
 */
struct fileattr_support fileattr_supported = {
	FT_MAX + 1,
	{ /* FT_UNKNOWN */ 0,
	  /* FT_FILE */ FA_FILETYPE | FA_MODTIME | FA_SIZE | FA_OWNER |
	      FA_GROUP | FA_MODE | FA_FLAGS | FA_LINKCOUNT | FA_INODE | FA_DEV,
	  /* FT_DIRECTORY */ FA_FILETYPE | FA_OWNER | FA_GROUP | FA_MODE |
	      FA_FLAGS,
	  /* FT_CDEV */ FA_FILETYPE | FA_RDEV | FA_OWNER | FA_GROUP | FA_MODE |
	      FA_FLAGS | FA_LINKCOUNT | FA_DEV | FA_INODE,
	  /* FT_BDEV */ FA_FILETYPE | FA_RDEV | FA_OWNER | FA_GROUP | FA_MODE |
	      FA_FLAGS | FA_LINKCOUNT | FA_DEV | FA_INODE,
	  /* FT_SYMLINK */ FA_FILETYPE | FA_LINKTARGET }
};
