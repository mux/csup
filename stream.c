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

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "stream.h"

#define	STREAM_BUFSIZ	4096

typedef ssize_t	(*readfn_t)(int, void *, size_t);
typedef ssize_t	(*writefn_t)(int, const void *, size_t);
typedef int	(*closefn_t)(int);

struct buf {
	char *buf;
	size_t size;
	size_t in;
	size_t off;
};

struct stream {
	int id;
	struct buf *rdbuf;
	struct buf *wrbuf;
	readfn_t readfn;
	writefn_t writefn;
	closefn_t closefn;
};

static struct buf	*buf_new(size_t);
static void		buf_delete(struct buf *);

static ssize_t		stream_refill(struct stream *);

static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = malloc(sizeof(struct buf));
	if (buf == NULL)
		return (NULL);
	buf->buf = malloc(size);
	if (buf->buf == NULL) {
		free(buf);
		return (NULL);
	}
	buf->size = size;
	buf->in = 0;
	buf->off = 0;
	return (buf);
}

static void
buf_delete(struct buf *buf)
{

	free(buf->buf);
	free(buf);
}

struct stream *
stream_open(int id, readfn_t readfn, writefn_t writefn, closefn_t closefn)
{
	struct stream *stream;

	stream = malloc(sizeof(struct stream));
	stream->rdbuf = buf_new(STREAM_BUFSIZ);
	if (stream->rdbuf == NULL) {
		free(stream);
		return (NULL);
	}
	stream->wrbuf = buf_new(STREAM_BUFSIZ);
	if (stream->wrbuf == NULL) {
		buf_delete(stream->rdbuf);
		free(stream);
		return (NULL);
	}
	stream->id = id;
	stream->readfn = readfn;
	stream->writefn = writefn;
	stream->closefn = closefn;
	return (stream);
}

ssize_t
stream_read(struct stream *stream, void *buf, size_t size)
{
	struct buf *rdbuf;
	ssize_t ret;
	size_t n;

	rdbuf = stream->rdbuf;
	if (rdbuf->in == 0) {
		assert(rdbuf->off == 0);
		ret = stream_refill(stream);
		if (ret <= 0)
			return (-1);
	}
	n = min(size, rdbuf->in);
	memcpy(buf, rdbuf->buf + rdbuf->off, n);
	rdbuf->in -= n;
	if (rdbuf->in == 0)
		rdbuf->off = 0;
	else
		rdbuf->off += n;
	return (n);
}

char *
stream_getln(struct stream *stream)
{
	struct buf *buf;
	char *c, *s;
	ssize_t n;
	size_t done, size;

	buf = stream->rdbuf;
	if (buf->in == 0) {
		n = stream_refill(stream);
		if (n <= 0)
			return (NULL);
	}
	c = memchr(buf->buf + buf->off, '\n', buf->in);
	for (done = buf->in; c == NULL; done += n) {
		if (buf->in == buf->size)
			return (NULL);
		if (buf->off + buf->in == buf->size) {
			memmove(buf->buf, buf->buf + buf->off, buf->in);
			buf->off = 0;
		}
		n = stream_refill(stream);
		if (n < 0)
			return (NULL);
		if (n == 0) {
			/*
			 * XXX - The stream doesn't end with a newline.
			 * In this particular case the last character of
			 * the line is lost but it doesn't matter here
			 * because the server is supposed to always
			 * terminate its lines with newline characters.
			 */
			c = buf->buf + buf->off + buf->in - 1;
		} else {
			c = memchr(buf->buf + buf->off + done, '\n',
			    buf->in - done);
		}
	}
	*c = '\0';
	s = buf->buf + buf->off;
	size = c - s + 1;
	assert(size <= buf->in);
	buf->in -= size;
	if (buf->in == 0)
		buf->off = 0;
	else
		buf->off += size;
	return (s);
}

int
stream_printf(struct stream *stream, const char *fmt, ...)
{
	struct buf *buf;
	va_list ap;
	int ret;

	buf = stream->wrbuf;
again:
	va_start(ap, fmt);
	ret = vsnprintf(buf->buf + buf->off + buf->in,
	    buf->size - buf->off - buf->in, fmt, ap);
	va_end(ap);
	if (ret < 0)
		return (ret);
	if (ret > (signed)(buf->size - buf->in)) {
		stream_flush(stream);
		goto again;
	}
	buf->in += ret;
	return (ret);
}

int
stream_flush(struct stream *stream)
{
	struct buf *buf;
	ssize_t n;

	buf = stream->wrbuf;
	while (buf->in > 0) {
again:
		n = (*stream->writefn)(stream->id, buf->buf + buf->off,
		    buf->in);
		if (n == -1 && errno == EINTR)
			goto again;
		if (n <= 0)
			return (-1);
		buf->in -= n;
		buf->off += n;
	}
	buf->off = 0;
	return (0);
}

int
stream_close(struct stream *stream)
{
	int error;

	error = 0;
	if (stream->closefn != NULL)
		error = (*stream->closefn)(stream->id);
	buf_delete(stream->rdbuf);
	buf_delete(stream->wrbuf);
	free(stream);
	return (error);
}

static ssize_t
stream_refill(struct stream *stream)
{
	struct buf *buf;
	ssize_t n;

	buf = stream->rdbuf;
	assert(buf->off + buf->in < buf->size);
	n = (*stream->readfn)(stream->id, buf->buf + buf->off + buf->in,
	    buf->size - buf->off - buf->in);
	if (n < 0)
		return (-1);
	buf->in += n;
	return (n);
}
