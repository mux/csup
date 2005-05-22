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
#ifndef _STREAM_H_
#define _STREAM_H_

/* Stream filters. */
typedef enum {
	STREAM_FILTER_NULL,
	STREAM_FILTER_ZLIB,
	STREAM_FILTER_MD5
} stream_filter_t;

struct stream;

typedef ssize_t	(*stream_readfn_t)(int, void *, size_t);
typedef ssize_t	(*stream_writefn_t)(int, const void *, size_t);
typedef int	(*stream_closefn_t)(int);

struct stream	*stream_fdopen(int, stream_readfn_t, stream_writefn_t,
		     stream_closefn_t);
struct stream	*stream_open_file(const char *, int, ...);
ssize_t		 stream_read(struct stream *, void *, size_t);
ssize_t		 stream_write(struct stream *, const void *, size_t);
char		*stream_getln(struct stream *, size_t *);
int		 stream_printf(struct stream *, const char *, ...);
int		 stream_flush(struct stream *);
int		 stream_sync(struct stream *);
int		 stream_truncate(struct stream *, off_t);
int		 stream_truncate_rel(struct stream *, off_t);
int		 stream_rewind(struct stream *);
int		 stream_close(struct stream *);
int		 stream_filter_start(struct stream *, stream_filter_t, void *);
void		 stream_filter_stop(struct stream *);

#endif /* !_STREAM_H_ */
