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

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <netinet/in.h>

#include <assert.h>
#include <errno.h>
#include <err.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "misc.h"
#include "mux.h"

/*
 * Packet types.
 */
#define	MUX_STARTUPREQ		0
#define	MUX_STARTUPREP		1
#define	MUX_CONNECT		2			
#define	MUX_ACCEPT		3
#define	MUX_RESET		4
#define	MUX_DATA		5
#define	MUX_WINDOW		6
#define	MUX_CLOSE		7

/*
 * Header sizes.
 */
#define	MUX_STARTUPHDRSZ	3
#define	MUX_CONNECTHDRSZ	8
#define	MUX_ACCEPTHDRSZ		8
#define	MUX_RESETHDRSZ		2
#define	MUX_DATAHDRSZ		4
#define	MUX_WINDOWHDRSZ		6
#define	MUX_CLOSEHDRSZ		2

#define	MUX_PROTOVER		0		/* Protocol version. */

/*
 * Older FreeBSD versions (and other OSes) don't have __packed,
 * so in this case, define it ourself.  This is a GCC-specific
 * keyword, but since the code wouldn't work without it, we
 * define it even in the !GNUC case.  There are chances other
 * compilers will support it though.
 */
#ifndef __packed
#define	__packed		__attribute__((__packed__))
#endif

struct mux_header {
	uint8_t type;
	union {
		struct {
			uint16_t version;
		} __packed mh_startup;
		struct {
			uint8_t id;
			uint16_t mss;
			uint32_t window;
		} __packed mh_connect;
		struct {
			uint8_t id;
			uint16_t mss;
			uint32_t window;
		} __packed mh_accept;
		struct {
			uint8_t id;
		} __packed mh_reset;
		struct {
			uint8_t id;
			uint16_t len;
		} __packed mh_data;
		struct {
			uint8_t id;
			uint32_t window;
		} __packed mh_window;
		struct {
			uint8_t id;
		} __packed mh_close;
	} mh_u;
} __packed;

#define	mh_startup		mh_u.mh_startup
#define	mh_connect		mh_u.mh_connect
#define	mh_accept		mh_u.mh_accept
#define	mh_reset		mh_u.mh_reset
#define	mh_data			mh_u.mh_data
#define	mh_window		mh_u.mh_window
#define	mh_close		mh_u.mh_close

#define	MUX_MAXCHAN		2

/* Channel states. */
#define	CS_UNUSED		0
#define	CS_LISTENING		1
#define	CS_CONNECTING		2
#define	CS_ESTABLISHED		3
#define	CS_RDCLOSED		4
#define	CS_WRCLOSED		5
#define	CS_CLOSED		6

/* Channel flags. */
#define	CF_CONNECT		0x01
#define	CF_ACCEPT		0x02
#define	CF_RESET		0x04
#define	CF_WINDOW		0x08
#define	CF_DATA			0x10
#define	CF_CLOSE		0x20

#define	CHAN_SBSIZE		(16 * 1024)	/* Send buffer size. */
#define	CHAN_RBSIZE		(16 * 1024)	/* Receive buffer size. */
#define	CHAN_MAXSEGSIZE		1024		/* Maximum segment size. */

/* Circular buffer. */
struct buf {
	uint8_t *data;
	size_t size;
	size_t in;
	size_t out;
};

struct chan {
	int		flags;
	int		state;
	pthread_mutex_t	lock;

	/* Receiver state variables. */
	struct buf	*recvbuf;
	pthread_cond_t	rdready;
	uint32_t	recvseq;
	uint16_t	recvmss;

	/* Sender state variables. */
	struct buf	*sendbuf;
	pthread_cond_t	wrready;
	uint32_t	sendseq;
	uint32_t	sendwin;
	uint16_t	sendmss;
};

static int		sock_writev(int, struct iovec *, int);
static int		sock_write(int, void *, size_t);
static ssize_t		sock_read(int, void *, size_t);
static int		sock_readwait(int, void *, size_t);

static int		mux_initproto(int);

static struct chan	*chan_new(void);
static struct chan	*chan_get(int);
static void		chan_lock(struct chan *);
static void		chan_unlock(struct chan *);
static int		chan_insert(struct chan *);
static int		chan_connect(int);
static void		chan_free(struct chan *);

static struct buf	*buf_new(size_t);
static size_t		buf_count(struct buf *);
static size_t		buf_avail(struct buf *);
static void		buf_get(struct buf *, void *, size_t);
static void		buf_put(struct buf *, const void *, size_t);
static void		buf_free(struct buf *);

static void		sender_wakeup(void);
static void		*sender_loop(void *);
static int		sender_waitforwork(int *);
static int		sender_scan(int *);

static void		*receiver_loop(void *);

/* Multiplexer data. */
static pthread_mutex_t mux_lock = PTHREAD_MUTEX_INITIALIZER;
static struct chan *chans[MUX_MAXCHAN];
static int nchans;

/* Sender thread data. */
static pthread_cond_t sender_newwork = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sender_ready = PTHREAD_COND_INITIALIZER;
static pthread_t sender;
static struct sender_data {
	int s;
	int error;
} sender_data;
static pthread_t receiver;
static struct receiver_data {
	int s;
	int error;
} receiver_data;

static int
sock_writev(int s, struct iovec *iov, int iovcnt)
{
	ssize_t nbytes;

again:
	nbytes = writev(s, iov, iovcnt);
	if (nbytes != -1) {
		while (nbytes > 0 && (size_t)nbytes >= iov->iov_len) {
			nbytes -= iov->iov_len;
			iov++;
			iovcnt--;
		}
		if (nbytes == 0)
			return (0);
		iov->iov_len -= nbytes;
		iov->iov_base = (char *)iov->iov_base + nbytes;
	} else if (errno != EINTR)
		err(1, "writev");
	goto again;
}

static int
sock_write(int s, void *buf, size_t size)
{
	struct iovec iov;
	int ret;

	iov.iov_base = buf;
	iov.iov_len = size;
	ret = sock_writev(s, &iov, 1);
	return (ret);
}

static ssize_t
sock_read(int s, void *buf, size_t size)
{
	ssize_t nbytes;

again:
	nbytes = read(s, buf, size);
	if (nbytes == -1 && errno == EINTR)
		goto again;
	return (nbytes);
}

static int
sock_readwait(int s, void *buf, size_t size)
{
	char *cp;
	ssize_t nbytes;
	size_t left;

	cp = buf;
	left = size;
	while (left > 0) {
		nbytes = sock_read(s, cp, left);
		if (nbytes <= 0)
			return (-1);
		left -= nbytes;
		cp += nbytes;
	}
	return (0);
}

/* Initialize the multiplexer on the given socket. */
int
mux_init(int s)
{
	int error;

	nchans = 0;
	memset(chans, 0, sizeof(chans));
	error = mux_initproto(s);
	return (error);
}

void
mux_fini(void)
{
	struct chan *chan;
	int i;

	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		if (chan != NULL)
			chan_free(chan);
	}
}

/*
 * Create a new channel, connect it and return its ID.
 */
int
chan_open(void)
{
	struct chan *chan;
	int error, id;

	chan = chan_new();
	id = chan_insert(chan);
	assert(id == 0);
	error = chan_connect(id);
	if (error)
		return (-1);
	return (id);
}

/* Close a channel. */
int
chan_close(int id)
{
	struct chan *chan;

	chan = chan_get(id);
	if (chan->state == CS_ESTABLISHED) {
		chan->state = CS_WRCLOSED;
		chan->flags |= CF_CLOSE;
	} else if (chan->state == CS_RDCLOSED) {
		chan->state = CS_CLOSED;
		chan->flags |= CF_CLOSE;
	} else if (chan->state == CS_WRCLOSED || chan->state == CS_CLOSED) {
		chan_unlock(chan);
		return (0);
	} else {
		chan_unlock(chan);
		return (-1);
	}
	chan_unlock(chan);
	sender_wakeup();
	return (0);
}

void
chan_wait(int id)
{
	struct chan *chan;

	chan = chan_get(id);
	while (chan->state != CS_CLOSED)
		pthread_cond_wait(&chan->rdready, &chan->lock);
	chan_unlock(chan);
}

/* Returns the ID of an available channel in the listening state. */
int
chan_listen(void)
{
	struct chan *chan;
	int i;

	pthread_mutex_lock(&mux_lock);
	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		pthread_mutex_lock(&chan->lock);
		if (chan->state == CS_UNUSED) {
			chan->state = CS_LISTENING;
			chan_unlock(chan);
			pthread_mutex_unlock(&mux_lock);
			return (i);
		}
		chan_unlock(chan);
	}
	pthread_mutex_unlock(&mux_lock);
	chan = chan_new();
	chan->state = CS_LISTENING;
	i = chan_insert(chan);
	return (i);
}

int
chan_accept(int id)
{
	struct chan *chan;
	int ok;

	chan = chan_get(id);
	while (chan->state == CS_LISTENING)
		pthread_cond_wait(&chan->rdready, &chan->lock);
	ok = (chan->state != CS_ESTABLISHED);
	chan_unlock(chan);
	return (ok);
}

/* Read bytes from a channel. */
ssize_t
chan_read(int id, void *buf, size_t size)
{
	struct chan *chan;
	char *cp;
	size_t count, n;

	count = 0;	/* Quiet bogus warning from GCC. */
	cp = buf;
	chan = chan_get(id);
	while ((chan->state == CS_ESTABLISHED || chan->state == CS_WRCLOSED) &&
	    (count = buf_count(chan->recvbuf)) == 0)
		pthread_cond_wait(&chan->rdready, &chan->lock);
	if (chan->state == CS_RDCLOSED || chan->state == CS_CLOSED)
		return (0);
	if (chan->state != CS_ESTABLISHED && chan->state != CS_WRCLOSED) {
		errno = EBADF;
		return (-1);
	}
	n = min(count, size);
	buf_get(chan->recvbuf, cp, n);
	chan->recvseq += n;
	chan->flags |= CF_WINDOW;
	chan_unlock(chan);
	/* We need to wake up the sender so that it sends a window update. */
	sender_wakeup();
	return (n);
}

/* Write bytes to a channel. */
ssize_t
chan_write(int id, const void *buf, size_t size)
{
	struct chan *chan;
	const char *cp;
	size_t avail, n, pos;

	pos = 0;
	avail = 0;	/* Quiet bogus warning from GCC. */
	cp = buf;
	chan = chan_get(id);
	while (pos < size) {
		while ((chan->state == CS_ESTABLISHED ||
		    chan->state == CS_RDCLOSED) && 
		    (avail = buf_avail(chan->sendbuf)) == 0)
			pthread_cond_wait(&chan->wrready, &chan->lock);
		if (chan->state != CS_ESTABLISHED &&
		    chan->state != CS_RDCLOSED) {
			errno = EPIPE;
			return (-1);
		}
		n = min(avail, size - pos);
		buf_put(chan->sendbuf, cp + pos, n);
		pos += n;
	}
	chan_unlock(chan);
	sender_wakeup();
	return (size);
}

/* 
 * Internal channel API.
 */

static int
chan_connect(int id)
{
	struct chan *chan;
	int ok;

	chan = chan_get(id);
	chan->state = CS_CONNECTING;
	chan->flags |= CF_CONNECT;
	chan_unlock(chan);
	sender_wakeup();
	chan = chan_get(id);
	while (chan->state == CS_CONNECTING)
		pthread_cond_wait(&chan->wrready, &chan->lock);
	ok = (chan->state != CS_ESTABLISHED);
	chan_unlock(chan);
	return (ok);
}

/*
 * Get a channel from its ID.  The channel is returned locked.
 */
static struct chan *
chan_get(int id)
{
	struct chan *chan;

	assert(id < MUX_MAXCHAN);
	pthread_mutex_lock(&mux_lock);
	chan = chans[id];
	chan_lock(chan);
	pthread_mutex_unlock(&mux_lock);
	return (chan);
}

/* Lock a channel. */
static void
chan_lock(struct chan *chan)
{

	pthread_mutex_lock(&chan->lock);
}

/* Unlock a channel.  */
static void
chan_unlock(struct chan *chan)
{

	pthread_mutex_unlock(&chan->lock);
}

/*
 * Create a new channel.
 */
static struct chan *
chan_new(void)
{
	struct chan *chan;

	chan = malloc(sizeof(struct chan));
	if (chan == NULL)
		err(1, "malloc");
	chan->state = CS_UNUSED;
	chan->flags = 0;
	chan->sendbuf = buf_new(CHAN_SBSIZE);
	chan->sendseq = 0;
	chan->sendwin = 0;
	chan->sendmss = 0;
	chan->recvbuf = buf_new(CHAN_RBSIZE);
	chan->recvseq = 0;
	chan->recvmss = CHAN_MAXSEGSIZE;
	pthread_mutex_init(&chan->lock, NULL);
	pthread_cond_init(&chan->rdready, NULL);
	pthread_cond_init(&chan->wrready, NULL);
	return (chan);
}

/* Free any resources associated with a channel. */
static void
chan_free(struct chan *chan)
{

	pthread_cond_destroy(&chan->rdready);
	pthread_cond_destroy(&chan->wrready);
	pthread_mutex_destroy(&chan->lock);
	buf_free(chan->recvbuf);
	buf_free(chan->sendbuf);
	free(chan);
}

/* Insert the new channel in the channel list and return its ID. */
static int
chan_insert(struct chan *chan)
{
	int i;

	pthread_mutex_lock(&mux_lock);
	for (i = 0; i < MUX_MAXCHAN; i++) {
		if (chans[i] == NULL) {
			chans[i] = chan;
			nchans++;
			pthread_mutex_unlock(&mux_lock);
			return (i);
		}
	}
	errx(1, "%s: no free channel", __func__);
}

/*
 * Initialize the multiplexer protocol.
 *
 * This means negotiating protocol version and starting
 * the receiver and sender threads.
 */
static int
mux_initproto(int s)
{
	struct mux_header mh;
	int error;

	mh.type = MUX_STARTUPREQ;
	mh.mh_startup.version = htons(MUX_PROTOVER);
	error = sock_write(s, &mh, MUX_STARTUPHDRSZ);
	if (error)
		return (-1);
	error = sock_readwait(s, &mh, MUX_STARTUPHDRSZ);
	if (error)
		return (-1);
	if (mh.type != MUX_STARTUPREP ||
	    ntohs(mh.mh_startup.version) != MUX_PROTOVER)
		return (-1);
	sender_data.s = s;
	sender_data.error = 0;
	error = pthread_create(&sender, NULL, sender_loop, &sender_data);
	if (error)
		return (-1);
	/* Make sure the sender has run and is waiting for new work. */
	pthread_mutex_lock(&mux_lock);
	pthread_cond_wait(&sender_ready, &mux_lock);
	pthread_mutex_unlock(&mux_lock);
	receiver_data.s = s;
	receiver_data.error = 0;
	error = pthread_create(&receiver, NULL, receiver_loop, &receiver_data);
	if (error)
		return (-1);
	return (0);
}

static void
sender_wakeup(void)
{

	pthread_mutex_lock(&mux_lock);
	pthread_cond_signal(&sender_newwork);
	pthread_mutex_unlock(&mux_lock);
}

static void *
sender_loop(void *arg)
{
	struct sender_data *sd;
	struct iovec iov[3];
	struct mux_header mh;
	struct chan *chan;
	struct buf *buf;
	uint32_t winsize;
	uint16_t hdrsize, size, len;
	int id, iovcnt, s, what;

	sd = arg;
	s = sd->s;
again:
	id = sender_waitforwork(&what);
	chan = chan_get(id);
	hdrsize = size = 0;
	switch (what) {
	case CF_CONNECT:
		mh.type = MUX_CONNECT;
		mh.mh_connect.id = id;
		mh.mh_connect.mss = htons(chan->recvmss);
		mh.mh_connect.window = htonl(chan->recvseq +
		    chan->recvbuf->size);
		hdrsize = MUX_CONNECTHDRSZ;
		break;
	case CF_ACCEPT:
		mh.type = MUX_ACCEPT;
		mh.mh_accept.id = id;
		mh.mh_accept.mss = htons(chan->recvmss);
		mh.mh_accept.window = htonl(chan->recvseq +
		    chan->recvbuf->size);
		hdrsize = MUX_ACCEPTHDRSZ;
		break;
	case CF_RESET:
		mh.type = MUX_RESET;
		mh.mh_reset.id = id;
		hdrsize = MUX_RESETHDRSZ;
		break;
	case CF_WINDOW:
		mh.type = MUX_WINDOW;
		mh.mh_window.id = id;
		mh.mh_window.window = htonl(chan->recvseq +
		    chan->recvbuf->size);
		hdrsize = MUX_WINDOWHDRSZ;
		break;
	case CF_DATA:
		mh.type = MUX_DATA;
		mh.mh_data.id = id;
		size = min(buf_count(chan->sendbuf), chan->sendmss);
		winsize = chan->sendwin - chan->sendseq;
		if (winsize < size)
			size = winsize;
		mh.mh_data.len = htons(size);
		hdrsize = MUX_DATAHDRSZ;
		break;
	case CF_CLOSE:
		mh.type = MUX_CLOSE;
		mh.mh_close.id = id;
		hdrsize = MUX_CLOSEHDRSZ;
		break;
	}
	if (size > 0) {
		assert(mh.type == MUX_DATA);
		/*
		 * Older FreeBSD versions (and maybe other OSes) have the
		 * iov_base field defined as char *.  Cast to char * to
		 * silence a warning in this case.
		 */
		iov[0].iov_base = (char *)&mh;
		iov[0].iov_len = hdrsize;
		iovcnt = 1;
		/* We access the buffer directly to avoid some copying. */
		buf = chan->sendbuf;
		len = min(size, buf->size + 1 - buf->out);
		iov[iovcnt].iov_base = buf->data + buf->out;
		iov[iovcnt].iov_len = len;
		iovcnt++;
		if (size > len) {
			/* Wrapping around. */
			iov[iovcnt].iov_base = buf->data;
			iov[iovcnt].iov_len = size - len;
			iovcnt++;
		}
		/*
		 * Since we're the only thread sending bytes from the
		 * buffer and modifying buf->out, it's safe to unlock
		 * here during I/O.  It avoids keeping the channel lock
		 * too long, since write() might block.
		 */
		chan_unlock(chan);
		sock_writev(s, iov, iovcnt);
		chan_lock(chan);
		chan->sendseq += size;
		buf->out += size;
		if (buf->out > buf->size)
			buf->out -= buf->size + 1;
		pthread_cond_signal(&chan->wrready);
		chan_unlock(chan);
	} else {
		chan_unlock(chan);
		sock_write(s, &mh, hdrsize);
	}
	goto again;
	return (NULL);
}

static int
sender_waitforwork(int *what)
{
	int id;

	pthread_mutex_lock(&mux_lock);
	pthread_cond_signal(&sender_ready);
	while ((id = sender_scan(what)) == -1)
		pthread_cond_wait(&sender_newwork, &mux_lock);
	pthread_mutex_unlock(&mux_lock);
	return (id);
}

/*
 * Scan for work to do for the sender.  Has to be
 * called with the mux_lock held.
 */
static int
sender_scan(int *what)
{
	struct chan *chan;
	int i;

	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		pthread_mutex_lock(&chan->lock);
		if (chan->state != CS_UNUSED) {
			if (chan->sendseq != chan->sendwin &&
			    buf_count(chan->sendbuf) > 0)
				chan->flags |= CF_DATA;
			if (chan->flags) {
				/* By order of importance. */
				if (chan->flags & CF_CONNECT)
					*what = CF_CONNECT;
				else if (chan->flags & CF_ACCEPT)
					*what = CF_ACCEPT;
				else if (chan->flags & CF_RESET)
					*what = CF_RESET;
				else if (chan->flags & CF_WINDOW)
					*what = CF_WINDOW;
				else if (chan->flags & CF_DATA)
					*what = CF_DATA;
				else if (chan->flags & CF_CLOSE)
					*what = CF_CLOSE;
				chan->flags &= ~*what;
				chan_unlock(chan);
				return (i);
			}
		}
		chan_unlock(chan);
	}
	return (-1);
}

void *
receiver_loop(void *arg)
{
	struct receiver_data *rd;
	struct mux_header mh;
	struct chan *chan;
	struct buf *buf;
	uint16_t size, len;
	int error, s;

	rd = arg;
	s = rd->s;
	while (!(error = sock_readwait(s, &mh.type, sizeof(mh.type)))) {
		switch (mh.type) {
		case MUX_CONNECT:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_CONNECTHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_connect.id);
			if (chan->state == CS_LISTENING) {
				chan->state = CS_ESTABLISHED;
				chan->sendmss = ntohs(mh.mh_connect.mss);
				chan->sendwin = ntohl(mh.mh_connect.window);
				chan->flags |= CF_ACCEPT;
				pthread_cond_signal(&chan->rdready);
			} else
				chan->flags |= CF_RESET;
			chan_unlock(chan);
			sender_wakeup();
			break;
		case MUX_ACCEPT:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_ACCEPTHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_accept.id);
			if (chan->state == CS_CONNECTING) {
				chan->sendmss = ntohs(mh.mh_accept.mss);
				chan->sendwin = ntohl(mh.mh_accept.window);
				chan->state = CS_ESTABLISHED;
				pthread_cond_signal(&chan->wrready);
				chan_unlock(chan);
			} else {
				chan->flags |= CF_RESET;
				chan_unlock(chan);
				sender_wakeup();
			}
			break;
		case MUX_RESET:
			goto bad;
			break;
		case MUX_WINDOW:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_WINDOWHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_window.id);
			if (chan->state == CS_ESTABLISHED ||
			    chan->state == CS_RDCLOSED)
				chan->sendwin = ntohl(mh.mh_window.window);
			chan_unlock(chan);
			sender_wakeup();
			break;
		case MUX_DATA:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_DATAHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_data.id);
			len = ntohs(mh.mh_data.len);
			buf = chan->recvbuf;
			if ((chan->state != CS_ESTABLISHED &&
			     chan->state != CS_WRCLOSED) ||
			    (len > buf_avail(buf) ||
			     len > chan->recvmss)) {
				chan_unlock(chan);
				goto bad;
			}
			/*
			 * Similarly to the sender code, it's safe to
			 * unlock the channel here.
			 */
			chan_unlock(chan);
			size = min(buf->size + 1 - buf->in, len);
			error = sock_readwait(s, buf->data + buf->in, size);
			if (len > size) {
				/* Wrapping around. */
				error = sock_readwait(s, buf->data, len - size);
			}
			pthread_mutex_lock(&chan->lock);
			buf->in += len;
			if (buf->in > buf->size)
				buf->in -= buf->size + 1;
			pthread_cond_signal(&chan->rdready);
			chan_unlock(chan);
			break;
		case MUX_CLOSE:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_CLOSEHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_close.id);
			if (chan->state == CS_ESTABLISHED)
				chan->state = CS_RDCLOSED;
			else if (chan->state == CS_WRCLOSED)
				chan->state = CS_CLOSED;
			else 
				goto bad;
			pthread_cond_signal(&chan->rdready);
			chan_unlock(chan);
			break;
		default:
			goto bad;
			break;
		}
	}
	warn("Receiver: socket error");
	return (NULL);
bad:
	warnx("Receiver: protocol error");
	return (NULL);
}

/*
 * Circular buffers API.
 */
 
static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = malloc(sizeof(struct buf));
	if (buf == NULL)
		err(1, "malloc");
	buf->data = malloc(size + 1);
	buf->size = size;
	if (buf->data == NULL) {
		free(buf);
		err(1, "malloc");
	}
	buf->in = 0;
	buf->out = 0;
	return (buf);
}

static void
buf_free(struct buf *buf)
{

	free(buf->data);
	free(buf);
}

/* Number of bytes stored in the buffer. */
static size_t
buf_count(struct buf *buf)
{
	size_t count;

	if (buf->in >= buf->out)
		count = buf->in - buf->out;
	else
		count = buf->size + 1 + buf->in - buf->out;
	return (count);
}

/* Number of bytes available in the buffer. */
static size_t
buf_avail(struct buf *buf)
{
	size_t avail;

	if (buf->out > buf->in)
		avail = buf->out - buf->in - 1;
	else
		avail = buf->size + buf->out - buf->in;
	return (avail);
}

static void
buf_put(struct buf *buf, const void *data, size_t size)
{
	const char *cp;
	size_t len;

	assert(size > 0);
	assert(buf_avail(buf) >= size);
	cp = data;
	len = buf->size + 1 - buf->in;
	if (len < size) {
		/* Wrapping around. */
		memcpy(buf->data + buf->in, cp, len);
		memcpy(buf->data, cp + len, size - len);
	} else {
		/* Not wrapping around. */
		memcpy(buf->data + buf->in, cp, size);
	}
	buf->in += size;
	if (buf->in > buf->size)
		buf->in -= buf->size + 1;
}

static void
buf_get(struct buf *buf, void *data, size_t size)
{
	char *cp;
	size_t len;

	assert(size > 0);
	assert(buf_count(buf) >= size);
	cp = data;
	len = buf->size + 1 - buf->out;
	if (len < size) {
		/* Wrapping around. */
		memcpy(cp, buf->data + buf->out, len);
		memcpy(cp + len, buf->data, size - len);
	} else {
		/* Not wrapping around. */
		memcpy(cp, buf->data + buf->out, size);
	}
	buf->out += size;
	if (buf->out > buf->size)
		buf->out -= buf->size + 1;
}
