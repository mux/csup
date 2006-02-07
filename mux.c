/*-
 * Copyright (c) 2003-2006, Maxime Henrion <mux@FreeBSD.org>
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
 * $FreeBSD: projects/csup/mux.c,v 1.56 2006/02/07 04:00:30 mux Exp $
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <netinet/in.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
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

static int		 sock_writev(int, struct iovec *, int);
static int		 sock_write(int, void *, size_t);
static ssize_t		 sock_read(int, void *, size_t);
static int		 sock_readwait(int, void *, size_t);

static int		 mux_initproto(int);
static void		 mux_shutdown(int);
static void		 mux_lock(void);
static void		 mux_unlock(void);

static struct chan	*chan_new(void);
static struct chan	*chan_get(int);
static void		 chan_lock(struct chan *);
static void		 chan_unlock(struct chan *);
static int		 chan_insert(struct chan *);
static int		 chan_connect(int);
static void		 chan_free(struct chan *);

static struct buf	*buf_new(size_t);
static size_t		 buf_count(struct buf *);
static size_t		 buf_avail(struct buf *);
static void		 buf_get(struct buf *, void *, size_t);
static void		 buf_put(struct buf *, const void *, size_t);
static void		 buf_free(struct buf *);

static void		 sender_wakeup(void);
static void		*sender_loop(void *);
static int		 sender_waitforwork(int *);
static int		 sender_scan(int *);
static void		 sender_cleanup(void *);

static void		*receiver_loop(void *);

/* We use one static multiplexer for now. */
static int mux_closed;
static int mux_sock;
static pthread_mutex_t mux_mtx;
static struct chan *chans[MUX_MAXCHAN];
static int nchans;

/* Sender thread data. */
static pthread_t sender;
static pthread_cond_t sender_newwork;
static pthread_cond_t sender_started;
static int sender_waiting;
static int sender_ready;

/* Receiver thread data. */
static pthread_t receiver;

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
	} else if (errno != EINTR) {
		return (-1);
	}
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
		if (nbytes == 0) {
			lprintf(-1, "Receiver: Connection reset by peer\n");
			return (-1);
		}
		if (nbytes < 0) {
			lprintf(-1, "Receiver: %s\n", strerror(errno));
			return (-1);
		}
		left -= nbytes;
		cp += nbytes;
	}
	return (0);
}

static void
mux_lock(void)
{
	int error;

	error = pthread_mutex_lock(&mux_mtx);
	assert(!error);
}

static void
mux_unlock(void)
{
	int error;

	error = pthread_mutex_unlock(&mux_mtx);
	assert(!error);
}

/* Initialize the multiplexer on the given socket. */
int
mux_init(int sock)
{
	int error;

	nchans = 0;
	memset(chans, 0, sizeof(chans));
	mux_closed = 0;
	mux_sock = sock;

	pthread_mutex_init(&mux_mtx, NULL);
	pthread_cond_init(&sender_newwork, NULL);
	pthread_cond_init(&sender_started, NULL);

	error = mux_initproto(mux_sock);
	return (error);
}

void
mux_fini(void)
{
	struct chan *chan;
	int i;

	if (!mux_closed)
		mux_shutdown(0);
	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		if (chan != NULL)
			chan_free(chan);
	}
	pthread_cond_destroy(&sender_started);
	pthread_cond_destroy(&sender_newwork);
	pthread_mutex_destroy(&mux_mtx);
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

	mux_lock();
	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		chan_lock(chan);
		if (chan->state == CS_UNUSED) {
			chan->state = CS_LISTENING;
			chan_unlock(chan);
			mux_unlock();
			return (i);
		}
		chan_unlock(chan);
	}
	mux_unlock();
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

	cp = buf;
	chan = chan_get(id);
	for (;;) {
		if (chan->state == CS_RDCLOSED || chan->state == CS_CLOSED) {
			chan_unlock(chan);
			return (0);
		}
		if (chan->state != CS_ESTABLISHED &&
		    chan->state != CS_WRCLOSED) {
			chan_unlock(chan);
			errno = EBADF;
			return (-1);
		}
		count = buf_count(chan->recvbuf);
		if (count > 0)
			break;
		pthread_cond_wait(&chan->rdready, &chan->lock);
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
	cp = buf;
	chan = chan_get(id);
	while (pos < size) {
		for (;;) {
			if (chan->state != CS_ESTABLISHED &&
			    chan->state != CS_RDCLOSED) {
				chan_unlock(chan);
				errno = EPIPE;
				return (-1);
			}
			avail = buf_avail(chan->sendbuf);
			if (avail > 0)
				break;
			pthread_cond_wait(&chan->wrready, &chan->lock);
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
	mux_lock();
	chan = chans[id];
	chan_lock(chan);
	mux_unlock();
	return (chan);
}

/* Lock a channel. */
static void
chan_lock(struct chan *chan)
{
	int error;

	error = pthread_mutex_lock(&chan->lock);
	assert(!error);
}

/* Unlock a channel.  */
static void
chan_unlock(struct chan *chan)
{
	int error;

	error = pthread_mutex_unlock(&chan->lock);
	assert(!error);
}

/*
 * Create a new channel.
 */
static struct chan *
chan_new(void)
{
	struct chan *chan;

	chan = xmalloc(sizeof(struct chan));
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

	mux_lock();
	for (i = 0; i < MUX_MAXCHAN; i++) {
		if (chans[i] == NULL) {
			chans[i] = chan;
			nchans++;
			mux_unlock();
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
	sender_ready = 0;
	mux_lock();
	error = pthread_create(&sender, NULL, sender_loop, &mux_sock);
	if (error) {
		mux_unlock();
		return (-1);
	}
	/*
	 * Make sure the sender thread has run and is waiting for new work
	 * before going on.  Otherwise, it might lose the race and a
	 * request, which will cause a deadlock.
	 */
	while (!sender_ready)
		pthread_cond_wait(&sender_started, &mux_mtx);
	mux_unlock();
	error = pthread_create(&receiver, NULL, receiver_loop, &mux_sock);
	if (error)
		return (-1);
	return (0);
}

/*
 * Close all the channels, terminate the sender and receiver thread.
 * If "error" is 0, it is a normal shutdown; if it's >0 then it is an
 * errno value corresponding to the error that happened and if it's -1
 * the error was a protocol error.
 */
static void
mux_shutdown(int error)
{
	struct chan *chan;
	const char *name;
	pthread_t self;
	void *val;
	int i;

	mux_lock();
	for (i = 0; i < MUX_MAXCHAN; i++) {
		if (chans[i] != NULL) {
			chan = chans[i];
			chan_lock(chan);
			if (chan->state != CS_UNUSED) {
				chan->state = CS_CLOSED;
				chan->flags = 0;
				pthread_cond_broadcast(&chan->rdready);
				pthread_cond_broadcast(&chan->wrready);
			}
			chan_unlock(chan);
		}
	}
	mux_unlock();
	self = pthread_self();
	if (!pthread_equal(self, receiver)) {
		pthread_cancel(receiver);
		pthread_join(receiver, &val);
		assert(val == PTHREAD_CANCELED);
	}
	if (!pthread_equal(self, sender)) {
		pthread_cancel(sender);
		pthread_join(sender, &val);
		assert(val == PTHREAD_CANCELED);
	}

	mux_closed = 1;
	if (!error)
		return;

	if (pthread_equal(self, sender)) {
		name = "Sender";
	} else {
		/* Only the sender and receiver threads report errors. */
		assert(pthread_equal(self, receiver));
		name = "Receiver";
	}
	if (error == -1)
		lprintf(-1, "%s: Protocol error\n", name);
	else
		lprintf(-1, "%s: %s\n", name, strerror(error));
}

static void
sender_wakeup(void)
{
	int waiting;

	mux_lock();
	waiting = sender_waiting;
	mux_unlock();
	/*
	 * We don't care about the race here: if the sender was
	 * waiting and is not anymore, we'll just send a useless
	 * signal; if he wasn't waiting then he won't go to sleep
	 * before having sent what we want him to.
	 */
	if (waiting)
		pthread_cond_signal(&sender_newwork);
}

static void *
sender_loop(void *arg)
{
	struct iovec iov[3];
	struct mux_header mh;
	struct chan *chan;
	struct buf *buf;
	uint32_t winsize;
	uint16_t hdrsize, size, len;
	int error, id, iovcnt, s, what;

	s = *(int *)arg;
	sender_waiting = 0;
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
		error = sock_writev(s, iov, iovcnt);
		if (error)
			goto bad;
		chan_lock(chan);
		chan->sendseq += size;
		buf->out += size;
		if (buf->out > buf->size)
			buf->out -= buf->size + 1;
		pthread_cond_signal(&chan->wrready);
		chan_unlock(chan);
	} else {
		chan_unlock(chan);
		error = sock_write(s, &mh, hdrsize);
		if (error)
			goto bad;
	}
	goto again;
bad:
	mux_shutdown(errno);
	return (NULL);
}

static void
sender_cleanup(void __unused *arg)
{

	mux_unlock();
}

static int
sender_waitforwork(int *what)
{
	int id;

	mux_lock();
	pthread_cleanup_push(sender_cleanup, NULL);
	if (!sender_ready) {
		pthread_cond_signal(&sender_started);
		sender_ready = 1;
	}
	while ((id = sender_scan(what)) == -1) {
		sender_waiting = 1;
		pthread_cond_wait(&sender_newwork, &mux_mtx);
	}
	sender_waiting = 0;
	pthread_cleanup_pop(1);
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
		chan_lock(chan);
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
	struct mux_header mh;
	struct chan *chan;
	struct buf *buf;
	uint16_t size, len;
	int error, s;

	s = *(int *)arg;
	while ((error = sock_readwait(s, &mh.type, sizeof(mh.type))) == 0) {
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
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_RESETHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			mux_shutdown(-1);
			return (NULL);
		case MUX_WINDOW:
			error = sock_readwait(s, (char *)&mh + sizeof(mh.type),
			    MUX_WINDOWHDRSZ - sizeof(mh.type));
			if (error)
				goto bad;
			chan = chan_get(mh.mh_window.id);
			if (chan->state == CS_ESTABLISHED ||
			    chan->state == CS_RDCLOSED) {
				chan->sendwin = ntohl(mh.mh_window.window);
				chan_unlock(chan);
				sender_wakeup();
			} else {
				chan_unlock(chan);
			}
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
				mux_shutdown(-1);
				return (NULL);
			}
			/*
			 * Similarly to the sender code, it's safe to
			 * unlock the channel here.
			 */
			chan_unlock(chan);
			size = min(buf->size + 1 - buf->in, len);
			error = sock_readwait(s, buf->data + buf->in, size);
			if (error)
				goto bad;
			if (len > size) {
				/* Wrapping around. */
				error = sock_readwait(s, buf->data, len - size);
				if (error)
					goto bad;
			}
			chan_lock(chan);
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
			else  {
				mux_shutdown(-1);
				return (NULL);
			}
			pthread_cond_signal(&chan->rdready);
			chan_unlock(chan);
			break;
		default:
			mux_shutdown(-1);
			return (NULL);
		}
	}
bad:
	mux_shutdown(errno);
	return (NULL);
}

/*
 * Circular buffers API.
 */

static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = xmalloc(sizeof(struct buf));
	buf->data = xmalloc(size + 1);
	buf->size = size;
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
