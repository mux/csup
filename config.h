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

#include <sys/queue.h>
#include <sys/types.h>

#include <fcntl.h>

/*
 * Collection options.
 */
#define	CO_BACKUP		0x00000001
#define	CO_DELETE		0x00000002
#define	CO_KEEP			0x00000004
#define	CO_OLD			0x00000008
#define	CO_UNLINKBUSY		0x00000010
#define	CO_NOUPDATE		0x00000020
#define	CO_COMPRESS		0x00000040
#define	CO_USERELSUFFIX		0x00000080
#define	CO_EXACTRCS		0x00000100
#define	CO_CHECKRCS		0x00000200
#define	CO_SKIP			0x00000400
#define	CO_CHECKOUTMODE		0x00000800
#define	CO_NORSYNC		0x00001000
#define	CO_KEEPBADFILES		0x00002000
#define	CO_EXECUTE		0x00004000
#define	CO_SETOWNER		0x00008000
#define	CO_SETMODE		0x00010000
#define	CO_SETFLAGS		0x00020000
#define	CO_NORCS		0x00040000
#define	CO_STRICTCHECKRCS	0x00080000
#define	CO_TRUSTSTATUSFILE	0x00100000
#define	CO_DODELETESONLY	0x00200000
#define	CO_DETAILALLRCSFILES	0x00400000

/* Options that the server is allowed to set. */
#define	CO_SERVMAYSET		(CO_SKIP | CO_NORSYNC | CO_NORCS)
/* Options that the server is allowed to clear. */
#define	CO_SERVMAYCLEAR		CO_CHECKRCS

struct collection {
	char *name;
	char *base;
	char *date;
	char *prefix;
	char *release;
	char *tag;
	int options;
	mode_t umask;
	STAILQ_ENTRY(collection) next;
};

struct config {
	char *host;
	uint16_t port;
	STAILQ_HEAD(, collection) collections;
	struct fileattr_support *supported;
	int chan0;
	int chan1;
};

struct config	*config_init(const char *, char *, char *, in_port_t);
#ifdef DEBUG
void		config_print(struct config *);
void		config_delete(struct config *);
#endif
struct collection *collec_new(void);
void		collec_add(struct collection *, char *);
void		collec_free(struct collection *);
void		collec_setdef(struct collection *);
void		options_set(struct collection *, int, char *);
