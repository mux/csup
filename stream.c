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
#include <zlib.h>
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

/*
 * This is because buf_new() will always allocate size + 1 bytes,
 * so our buffer sizes will still be power of 2 values.
 */
#define	STREAM_BUFSIZ	1023

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
	stream_readfn_t readfn;
	stream_writefn_t writefn;
	stream_closefn_t closefn;
	stream_filter_t filter;
	void *fdata;
};

/* Low-level buffer API. */
#define	buf_avail(buf)		((buf)->size - (buf)->off - (buf)->in)
#define	buf_count(buf)		((buf)->in)
#define	buf_size(buf)		((buf)->size)

static struct buf	*buf_new(size_t);
static void		 buf_more(struct buf *, size_t);
static void		 buf_less(struct buf *, size_t);
static void		 buf_free(struct buf *);
static void		 buf_grow(struct buf *, size_t);

static ssize_t		 stream_refill(struct stream *);
static ssize_t		 stream_refill_buf(struct stream *, struct buf *);
static int		 stream_flush_buf(struct stream *, struct buf *);

/* Used by the zlib filter to keep state. */
struct zfilter {
	struct buf *rdbuf;
	struct buf *wrbuf;
	z_stream *rdstate;
	z_stream *wrstate;
};

static int		 zfilter_init(struct stream *);
static int		 zfilter_inflate(struct stream *, int);
static int		 zfilter_deflate(struct stream *, int);
static void		 zfilter_finish(struct stream *);

/* Create a new buffer. */
static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = malloc(sizeof(struct buf));
	if (buf == NULL)
		err(1, "malloc");
	/*
	 * We keep one spare byte so that stream_getln() can put a '\0'
	 * there in case the stream doesn't have an ending newline.
	 */
	buf->buf = malloc(size + 1);
	if (buf->buf == NULL) {
		free(buf);
		err(1, "malloc");
	}
	buf->size = size;
	buf->in = 0;
	buf->off = 0;
	return (buf);
}

/*
 * Grow the size of the buffer.  If "need" is 0, bump its size to the
 * next power of 2 value.  Otherwise, bump it to the next power of 2
 * value bigger than "need".
 */
static void
buf_grow(struct buf *buf, size_t need)
{
	char *tmp;

	if (need == 0)
		buf->size = buf->size * 2 + 1; /* Account for the spare byte. */
	else {
		assert(need > buf->size);
		while (buf->size < need)
			buf->size = buf->size * 2 + 1;
	}
	tmp = realloc(buf->buf, buf->size + 1);
	if (tmp == NULL)
		err(1, "realloc");
	buf->buf = tmp;
}

/* Make more room in the buffer if needed. */
static void
buf_makeroom(struct buf *buf)
{

	if (buf_count(buf) == buf_size(buf))
		buf_grow(buf, 0);
	if (buf_count(buf) > 0 && buf_avail(buf) == 0) {
		memmove(buf->buf, buf->buf + buf->off, buf_count(buf));
		buf->off = 0;
	}
}

/* Account for "n" bytes being added in the buffer. */
static void
buf_more(struct buf *buf, size_t n)
{

	assert(n <= buf_avail(buf));
	buf->in += n;
}

/* Account for "n" bytes having been read in the buffer. */
static void
buf_less(struct buf *buf, size_t n)
{

	assert(n <= buf_count(buf));
	buf->in -= n;
	if (buf->in == 0)
		buf->off = 0;
	else
		buf->off += n;
}

/* Free a buffer. */
static void
buf_free(struct buf *buf)
{

	free(buf->buf);
	free(buf);
}

/* Associate a file descriptor with a stream. */
struct stream *
stream_fdopen(int id, stream_readfn_t readfn, stream_writefn_t writefn,
    stream_closefn_t closefn)
{
	struct stream *stream;

	stream = malloc(sizeof(struct stream));
	if (stream == NULL)
		err(1, "malloc");
	if (readfn == NULL && writefn == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	if (readfn != NULL)
		stream->rdbuf = buf_new(STREAM_BUFSIZ);
	else
		stream->rdbuf = NULL;
	if (writefn != NULL)
		stream->wrbuf = buf_new(STREAM_BUFSIZ);
	else
		stream->wrbuf = NULL;
	stream->id = id;
	stream->readfn = readfn;
	stream->writefn = writefn;
	stream->closefn = closefn;
	stream->filter = SF_NONE;
	stream->fdata = NULL;
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
	if (buf_count(rdbuf) == 0) {
		ret = stream_refill(stream);
		if (ret <= 0)
			return (-1);
	}
	n = min(size, buf_count(rdbuf));
	memcpy(buf, rdbuf->buf + rdbuf->off, n);
	buf_less(rdbuf, n);
	return (n);
}

/*
 * Read a line from the stream and return a pointer to it.
 *
 * If "len" is non-NULL, the length of the string will be put into it.
 * The pointer is only valid until the next stream API call.  The line
 * can be modified by the caller, provided he doesn't write before or
 * after it.
 *
 * This is somewhat similar to the BSD fgetln() function, except that
 * "len" can be NULL here.  In that case the string is terminated by
 * overwriting the '\n' character with a NUL character.  If it's the
 * last line in the stream and it has no ending newline, we can still
 * add '\0' after it, because we keep one spare byte in the buffers.
 *
 * However, be warned that one can't handle binary lines properly
 * without knowing the size of the string since those can contain
 * NUL characters.
 */
char *
stream_getln(struct stream *stream, size_t *len)
{
	struct buf *buf;
	char *c, *s;
	ssize_t n;
	size_t done, size;

	buf = stream->rdbuf;
	if (buf_count(buf) == 0) {
		n = stream_refill(stream);
		if (n <= 0)
			return (NULL);
	}
	c = memchr(buf->buf + buf->off, '\n', buf_count(buf));
	for (done = buf_count(buf); c == NULL; done += n) {
		n = stream_refill(stream);
		if (n < 0)
			return (NULL);
		if (n == 0) {
			/*
			 * This is OK since we keep one spare byte, and
			 * this is the last line in the stream.
			 */
			c = buf->buf + buf->off + buf->in;
			*c = '\0';
		} else {
			c = memchr(buf->buf + buf->off + done, '\n',
			    buf_count(buf) - done);
		}
	}
	s = buf->buf + buf->off;
	size = c - s;
	if (*c == '\n')
		buf_less(buf, size + 1);
	else
		buf_less(buf, size);
	if (len != NULL)
		*len = size;
	else
		*c = '\0';	/* Always terminate when len == NULL. */
	return (s);
}

/* Write some bytes to a stream. */
ssize_t
stream_write(struct stream *stream, const void *src, size_t nbytes)
{
	struct buf *buf;
	int error;

	buf = stream->wrbuf;
	if (nbytes > buf_size(buf))
		buf_grow(buf, nbytes);
	if (nbytes > buf_avail(buf)) {
		error = stream_flush(stream);
		if (error)
			return (-1);
	}
	memcpy(buf->buf + buf->off + buf->in, src, nbytes);
	buf_more(buf, nbytes);
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
	ret = vsnprintf(buf->buf + buf->off + buf->in, buf_avail(buf), fmt, ap);
	va_end(ap);
	if (ret < 0)
		return (ret);
	if ((unsigned)ret >= buf_avail(buf)) {
		if ((unsigned)ret > buf_size(buf))
			buf_grow(buf, ret);
		else {
			error = stream_flush(stream);
			if (error)
				return (-1);
		}
		goto again;
	}
	buf_more(buf, ret);
	return (ret);
}

/* Flush a given buffer. */
static int
stream_flush_buf(struct stream *stream, struct buf *buf)
{
	ssize_t n;

	while (buf_count(buf) > 0) {
again:
		n = (*stream->writefn)(stream->id, buf->buf + buf->off,
		    buf_count(buf));
		if (n == -1 && errno == EINTR)
			goto again;
		if (n <= 0)
			return (-1);
		buf_less(buf, n);
	}
	return (0);
}

/* Flush the stream. */
int
stream_flush(struct stream *stream)
{
	struct zfilter *zf;
	struct buf *buf;
	int error, rv;

	buf = stream->wrbuf;
	if (buf_count(buf) == 0)
		return (0);
	if (stream->filter == SF_ZLIB) {
		zf = stream->fdata;
		rv = zfilter_deflate(stream, Z_SYNC_FLUSH);
		if (rv != Z_OK)
			errx(1, "deflate: %s", zf->wrstate->msg);
		return (0);
	}
	error = stream_flush_buf(stream, buf);
	return (error);
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

/* Rewind the stream. */
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

	if (stream == NULL)
		return (0);

	error = 0;
	if (stream->wrbuf != NULL)
		error = stream_flush(stream);
	if (stream->filter == SF_ZLIB)
		zfilter_finish(stream);
	if (stream->closefn != NULL)
		/*
		 * We might overwrite a previous error from stream_flush(),
		 * but we have no choice, because wether it had worked or
		 * not, we need to close the file descriptor.
		 */
		error = (*stream->closefn)(stream->id);
	if (stream->rdbuf != NULL)
		buf_free(stream->rdbuf);
	if (stream->wrbuf != NULL)
		buf_free(stream->wrbuf);
	free(stream);
	return (error);
}

static ssize_t
stream_refill_buf(struct stream *stream, struct buf *buf)
{
	ssize_t n;

	buf_makeroom(buf);
	n = (*stream->readfn)(stream->id, buf->buf + buf->off + buf->in,
	    buf_avail(buf));
	if (n < 0)
		return (-1);
	buf_more(buf, n);
	return (n);
}

/*
 * Refill the read buffer.  This function is not permitted to return
 * without having made more bytes available, unless there was an error.
 * Moreover, stream_refill() returns the number of bytes added.
 */
static ssize_t
stream_refill(struct stream *stream)
{
	struct zfilter *zf;
	struct buf *buf;
	size_t count;
	ssize_t n;
	int rv;

	buf = stream->rdbuf;
	if (stream->filter == SF_ZLIB) {
		count = buf_count(buf);
		zf = stream->fdata;
		rv = zfilter_inflate(stream, Z_SYNC_FLUSH);
		if (rv != Z_OK && rv != Z_STREAM_END)
			return (-1);
		n = buf_count(buf) - count;
		assert(n > 0);
		return (n);
	}
	n = stream_refill_buf(stream, buf);
	return (n);
}

/*
 * Set a specific filter on a stream.  If SF_NONE is set, it disables
 * the filter.  There's only a SF_ZLIB filter for now, which allows to
 * read/write a zlib compressed stream.
 */
int
stream_filter(struct stream *stream, stream_filter_t filter)
{

	if (filter == stream->filter)
		return (0);
	/* Disable the old filter. */
	if (stream->filter == SF_ZLIB)
		zfilter_finish(stream);
	stream->fdata = NULL;
	/* Enable the new filter. */
	if (filter == SF_ZLIB)
		zfilter_init(stream);
	stream->filter = filter;
	return (0);
}

static int
zfilter_init(struct stream *stream)
{
	struct zfilter *zf;
	struct buf *buf;
	z_stream *state;
	int rv;

	zf = malloc(sizeof(struct zfilter));
	if (zf == NULL)
		err(1, "malloc");
	memset(zf, 0, sizeof(struct zfilter));
	if (stream->rdbuf != NULL) {
		state = malloc(sizeof(z_stream));
		if (state == NULL)
			err(1, "malloc");
		state->zalloc = Z_NULL;
		state->zfree = Z_NULL;
		state->opaque = Z_NULL;
		rv = inflateInit(state);
		if (rv != Z_OK)
			errx(1, "inflateInit: %s", state->msg);
		buf = buf_new(buf_size(stream->rdbuf));
		zf->rdbuf = stream->rdbuf;
		stream->rdbuf = buf;
		zf->rdstate = state;
	}
	if (stream->wrbuf != NULL) {
		state = malloc(sizeof(z_stream));
		if (state == NULL)
			err(1, "malloc");
		state->zalloc = Z_NULL;
		state->zfree = Z_NULL;
		state->opaque = Z_NULL;
		rv = deflateInit(state, Z_BEST_SPEED);
		if (rv != Z_OK)
			errx(1, "deflateInit: %s", state->msg);
		buf = buf_new(buf_size(stream->wrbuf));
		zf->wrbuf = stream->wrbuf;
		stream->wrbuf = buf;
		zf->wrstate = state;
	}
	stream->fdata = zf;
	return (0);
}

static void
zfilter_finish(struct stream *stream)
{
	struct zfilter *zf;
	struct buf *zbuf;
	z_stream *state;
	int rv;

	zf = stream->fdata;
	if (zf->rdbuf != NULL) {
		state = zf->rdstate;
		zbuf = zf->rdbuf;
		/* Be sure to eat all the compressed bytes. */
		if (buf_count(zbuf) > 0) {
			do {
				rv = zfilter_inflate(stream, Z_FINISH);
			} while (rv == Z_OK);
			if (rv != Z_STREAM_END)
				errx(1, "inflate: %s (%d)", state->msg, rv);
		}
		inflateEnd(state);
		free(state);
		buf_free(stream->rdbuf);
		stream->rdbuf = zbuf;
	}
	if (zf->wrbuf != NULL) {
		state = zf->wrstate;
		zbuf = zf->wrbuf;
		/* Compress the remaining bytes in the buffer, if any. */
		if (buf_count(stream->wrbuf) > 0) {
			do {
				rv = zfilter_deflate(stream, Z_FINISH);
				stream_flush_buf(stream, zbuf);
			} while (rv == Z_OK);
			if (rv != Z_STREAM_END)
				errx(1, "deflate: %s", state->msg);
		}
		deflateEnd(state);
		free(state);
		buf_free(stream->wrbuf);
		stream->wrbuf = zbuf;
	}
	free(zf);
}

static int
zfilter_deflate(struct stream *stream, int flags)
{
	struct zfilter *zf;
	struct buf *buf, *zbuf;
	z_stream *state;
	size_t lastin, lastout;
	int error, rv;

	zf = stream->fdata;
	state = zf->wrstate;
	buf = stream->wrbuf;
	zbuf = zf->wrbuf;

	if (buf_avail(zbuf) == 0) {
		error = stream_flush_buf(stream, zbuf);
		if (error != 0)
			return (Z_ERRNO);
	}
	state->next_in = buf->buf + buf->off;
	state->avail_in = buf_count(buf);
	state->next_out = zbuf->buf + zbuf->off + zbuf->in;
	state->avail_out = buf_avail(zbuf);
	lastin = state->avail_in;
	lastout = state->avail_out;
	rv = deflate(state, flags);
	buf_less(buf, lastin - state->avail_in);
	buf_more(zbuf, lastout - state->avail_out);
	return (rv);
}

static int
zfilter_inflate(struct stream *stream, int flags)
{
	struct zfilter *zf;
	struct buf *buf, *zbuf;
	z_stream *state;
	size_t lastin, lastout;
	ssize_t n;
	int rv;

	zf = stream->fdata;
	state = zf->rdstate;
	buf = stream->rdbuf;
	zbuf = zf->rdbuf;

	buf_makeroom(buf);
	if (buf_count(zbuf) == 0) {
		n = stream_refill_buf(stream, zbuf);
		if (n == -1)
			return (Z_ERRNO);
		if (n == 0)
			return (Z_OK);
	}
again:
	state->next_in = zbuf->buf + zbuf->off;
	assert(buf_count(zbuf) > 0);
	state->avail_in = buf_count(zbuf);
	state->next_out = buf->buf + buf->off + buf->in;
	state->avail_out = buf_avail(buf);
	lastin = state->avail_in;
	lastout = state->avail_out;
	rv = inflate(state, flags);
	buf_less(zbuf, lastin - state->avail_in);
	if (lastout - state->avail_out == 0 && rv != Z_STREAM_END) {
		n = stream_refill_buf(stream, zbuf);
		if (n == -1)
			return (Z_ERRNO);
		if (n == 0)
			return (Z_OK);
		goto again;
	}
	buf_more(buf, lastout - state->avail_out);
	return (rv);
}
