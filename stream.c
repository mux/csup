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
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "misc.h"
#include "stream.h"

/*
 * Simple stream API to make my life easier.  If the fgetln() and
 * funopen() functions were standard and if funopen() wasn't using
 * wrong types for the function pointers, I could have just used
 * stdio, but life sucks.
 *
 * For now, streams are always block-buffered.
 */

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
		err(1, "malloc");
	buf->buf = malloc(size);
	if (buf->buf == NULL) {
		free(buf);
		err(1, "malloc");
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

/* Associate a file descriptor with a stream. */
struct stream *
stream_fdopen(int id, readfn_t readfn, writefn_t writefn, closefn_t closefn)
{
	struct stream *stream;

	stream = malloc(sizeof(struct stream));
	if (stream == NULL)
		err(1, "malloc");
	stream->rdbuf = buf_new(STREAM_BUFSIZ + 1);
	/*
	 * We keep one spare byte in the read buffer so that
	 * stream_getln() can put a '\0' there in case the stream
	 * doesn't have an ending newline.
	 */
	stream->rdbuf->buf[--stream->rdbuf->size] = '\0';
	stream->wrbuf = buf_new(STREAM_BUFSIZ);
	stream->id = id;
	stream->readfn = readfn;
	stream->writefn = writefn;
	stream->closefn = closefn;
	return (stream);
}

/* Like open() but returns a stream. */
struct stream *
stream_open_file(char *path, int flags, ...)
{
	struct stream *stream;
	va_list ap;
	mode_t mode;
	int fd;

	va_start(ap, flags);
	if (flags & O_CREAT) {
		/*
		 * GCC says I should not be using mode_t here since it's
		 * promoted to an int when passed through `...'.
		 */
		mode = va_arg(ap, int);
		fd = open(path, flags, mode);
	} else
		fd = open(path, flags);
	va_end(ap);
	if (fd == -1)
		return (NULL);
	stream = stream_fdopen(fd, read, write, close);
	if (stream == NULL)
		close(fd);
	return (stream);
}

/* Read some bytes from the stream. */
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

/*
 * Read a line from the stream and return a pointer to it.
 *
 * If len is non-NULL, the length of the string will be put into it.
 * The pointer is only valid until the next stream API call.  The
 * line can be modified by the caller, provided he doesn't write
 * before or after it.
 *
 * This is somewhat similar to the BSD fgetln() function, except
 * that it terminates the string by overwriting the '\n' character
 * with a NUL character.  Furthermore, len can be NULL here - but
 * be warned that one can't handle binary lines without knowing
 * the size since those can contain other NUL characters.
 */
char *
stream_getln(struct stream *stream, size_t *len)
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
		if (buf->in == buf->size) {
			printf("%s: Implement buffer resizing\n", __func__);
			return (NULL);
		}
		if (buf->off + buf->in == buf->size) {
			memmove(buf->buf, buf->buf + buf->off, buf->in);
			buf->off = 0;
		}
		n = stream_refill(stream);
		if (n < 0)
			return (NULL);
		if (n == 0) {
			/* This is OK since we keep one spare byte. */
			c = buf->buf + buf->off + buf->in;
		} else {
			c = memchr(buf->buf + buf->off + done, '\n',
			    buf->in - done);
		}
	}
	s = buf->buf + buf->off;
	size = c - s + 1;
	if (*c != '\n')
		size--;
	assert(size <= buf->in);
	buf->in -= size;
	if (buf->in == 0)
		buf->off = 0;
	else
		buf->off += size;
	if (len != NULL)
		*len = size - 1;
	*c = '\0';
	return (s);
}

/* Write some bytes to a stream. */
ssize_t
stream_write(struct stream *stream, const void *src, size_t nbytes)
{
	struct buf *buf;
	int error;

	buf = stream->wrbuf;
	if (nbytes > buf->size) {
		printf("%s: Implement buffer resizing\n", __func__);
		return (-1);
	}
	if (nbytes > buf->size - buf->off - buf->in) {
		error = stream_flush(stream);
		if (error)
			return (-1);
	}
	memcpy(buf->buf + buf->off + buf->in, src, nbytes);
	buf->in += nbytes;
	return (nbytes);
}

/* Formatted output to a stream. */
int
stream_printf(struct stream *stream, const char *fmt, ...)
{
	struct buf *buf;
	va_list ap;
	int error, ret;

	buf = stream->wrbuf;
again:
	va_start(ap, fmt);
	ret = vsnprintf(buf->buf + buf->off + buf->in,
	    buf->size - buf->off - buf->in, fmt, ap);
	va_end(ap);
	if (ret < 0)
		return (ret);
	if ((unsigned)ret >= buf->size - buf->off - buf->in) {
		if ((unsigned)ret >= buf->size) {
			printf("%s: Implement buffer resizing\n", __func__);
			return (-1);
		}
		error = stream_flush(stream);
		if (error)
			return (-1);
		goto again;
	}
	buf->in += ret;
	return (ret);
}

/* Flush the write buffer. */
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

/* Flush the write buffer and call fsync() on the file descriptor. */
int
stream_sync(struct stream *stream)
{
	int error;
	
	error = stream_flush(stream);
	if (error)
		return (-1);
	error = fsync(stream->id);
	return (error);
}

/* Like truncate() but on a stream. */
int
stream_truncate(struct stream *stream, off_t size)
{
	int error;

	error = stream_flush(stream);
	if (error)
		return (-1);
	error = ftruncate(stream->id, size);
	return (error);
}

/* Like stream_truncate() except the off_t parameter is an offset. */
int
stream_truncate_rel(struct stream *stream, off_t off)
{
	struct stat sb;
	int error;

	error = stream_flush(stream);
	if (error)
		return (-1);
	error = fstat(stream->id, &sb);
	if (error)
		return (-1);
	error = stream_truncate(stream, sb.st_size + off);
	return (error);
}

int
stream_rewind(struct stream *stream)
{
	int error;

	error = lseek(stream->id, 0, SEEK_SET);
	return (error);
}

/* Close a stream and free any resources held by it. */
int
stream_close(struct stream *stream)
{
	int error;

	error = stream_flush(stream);
	if (error)
		return (-1);
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
