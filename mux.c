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

#include <sys/param.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mux.h"

#define	min(a, b)	((a) > (b) ? (b) : (a))

#define	MUX_MAXCHAN	2

/* Channel states. */
#define	CS_UNUSED	0
#define	CS_LISTENING	1
#define	CS_CONNECTING	2
#define	CS_ESTABLISHED	3
#define	CS_RDCLOSED	4
#define	CS_WRCLOSED	5
#define	CS_CLOSED	6

/* Channel flags. */
#define	CF_CONNECT	0x01
#define	CF_ACCEPT	0x02
#define	CF_RESET	0x04
#define	CF_WINDOW	0x08
#define	CF_DATA		0x10
#define	CF_CLOSE	0x20

#define	CHAN_SBSIZE	(16 * 1024)		/* Send buffer size. */
#define	CHAN_RBSIZE	(16 * 1024)		/* Receive buffer size. */
#define	CHAN_MAXSEGSIZE	1024			/* Maximum segment size. */

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

static int		sock_write(int, void *, size_t);

static int		mux_initproto(int);

static struct chan	*chan_new(void);
static struct chan	*chan_get(int);
static int		chan_insert(struct chan *);
static int		chan_connect(int);

static void		*sender_loop(void *);
static int		sender_waitforwork(int *);
static int		sender_scan(int *);

static void		*receiver_loop(void *);

static pthread_mutex_t mux_lock;
static struct chan *chans[MUX_MAXCHAN];
static int nchans;
static pthread_cond_t newwork;
static pthread_t sender;
static pthread_t receiver;

static int
sock_write(int s, void *buf, size_t size)
{
	char *cp;
	ssize_t nbytes;
	size_t left;

	cp = buf;
	left = size;
	while (left > 0) {
		nbytes = write(s, cp, left);
		if (nbytes == -1) {
			if (errno != EINTR)
				return (-1);
		} else {
			left -= nbytes;
			cp += nbytes;
		}
	}
	return (0);
}

int
mux_open(int s)
{
	int id;

	nchans = 0;
	memset(chans, 0, sizeof(chans));
	pthread_mutex_init(&mux_lock, NULL);
	pthread_cond_init(&newwork, NULL);
	id = mux_initproto(s);
	return (id);
}

/*
 * Initialize the multiplexer.
 *
 * This means negotiating protocol version, starting
 * the receiver and sender threads, creating one
 * channel, connecting it and returning its ID.
 */
static int
mux_initproto(int s)
{
	struct mux_header mh;
	struct chan *chan;
	ssize_t n;
	int error, id;

	mh.type = MUX_STARTUPREQ;
	mh.mh_startup.version = MUX_PROTOVER;
	sock_write(s, &mh, MUX_STARTUPPKTSZ);
	n = recv(s, &mh, MUX_STARTUPPKTSZ, MSG_WAITALL);
	if (n != MUX_STARTUPPKTSZ || mh.type != MUX_STARTUPREP ||
	    mh.mh_startup.version != MUX_PROTOVER)
		return (-1);
	pthread_create(&sender, NULL, sender_loop, &s);
	pthread_create(&receiver, NULL, receiver_loop, &s);
	chan = chan_new();
	id = chan_insert(chan);
	assert(id == 0);
	error = chan_connect(id);
	if (error)
		return (-1);
	return (id);
}

/*
 * Returns the ID of an available channel in the listening state.
 */
int
mux_listen(void)
{
	struct chan *chan;
	int i;

	pthread_mutex_lock(&mux_lock);
	for (i = 0; i < nchans; i++) {
		chan = chans[i];
		pthread_mutex_lock(&chan->lock);
		if (chan->state == CS_UNUSED) {
			chan->state = CS_LISTENING;
			pthread_mutex_unlock(&chan->lock);
			pthread_mutex_unlock(&mux_lock);
			return (i);
		}
		pthread_mutex_unlock(&chan->lock);
	}
	pthread_mutex_unlock(&mux_lock);
	chan = chan_new();
	chan->state = CS_LISTENING;
	i = chan_insert(chan);
	return (i);
}

static void *
sender_loop(void *arg)
{
	struct mux_header mh;
	struct chan *chan;
	size_t size;
	int id, s, what;

	s = *(int *)arg;
again:
	id = sender_waitforwork(&what);
	chan = chan_get(id);
	size = 0;
	switch (what) {
	case CF_CONNECT:
		mh.type = MUX_CONNECT;
		mh.mh_connect.id = id;
		mh.mh_connect.mss = chan->recvmss;
		mh.mh_connect.window = chan->recvseq + chan->recvbuf->size;
		size = MUX_CONNECTPKTSZ;
		break;
	case CF_ACCEPT:
		mh.type = MUX_ACCEPT;
		mh.mh_accept.id = id;
		mh.mh_accept.mss = chan->recvmss;
		mh.mh_accept.window = chan->recvseq + chan->recvbuf->size;
		size = MUX_ACCEPTPKTSZ;
		break;
	case CF_RESET:
		mh.type = MUX_RESET;
		mh.mh_reset.id = id;
		size = MUX_RESETPKTSZ;
		break;
	case CF_WINDOW:
		mh.type = MUX_WINDOW;
		mh.mh_window.id = id;
		mh.mh_window.window = chan->recvseq + chan->recvbuf->size;
		size = MUX_WINDOWPKTSZ;
		break;
	case CF_DATA:
		break;
	case CF_CLOSE:
		mh.type = MUX_CLOSE;
		mh.mh_close.id = id;
		size = MUX_CLOSEPKTSZ;
		break;
	}
	pthread_mutex_unlock(&chan->lock);
	if (size > 0)
		sock_write(s, &mh, size);
	goto again;
	return (NULL);
}

static int
sender_waitforwork(int *what)
{
	int id;

	pthread_mutex_lock(&mux_lock);
	while ((id = sender_scan(what)) == -1)
		pthread_cond_wait(&newwork, &mux_lock);
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
		if (chan->flags != 0) {
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
			assert(*what != 0);
			chan->flags &= ~*what;
			pthread_mutex_unlock(&chan->lock);
			return (i);
		}
		pthread_mutex_unlock(&chan->lock);
	}
	return (-1);
}

void *
receiver_loop(void *arg)
{
	struct kevent kev;
	struct timespec ts;
	int error, kq, s;

	s = *(int *)arg;
	kq = kqueue();
	if (kq == -1)
		return (NULL);
	/* First, register our filter. */
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	EV_SET(&kev, s, EVFILT_READ, EV_ADD, 0, 0, NULL);
	error = kevent(kq, &kev, 1, NULL, 0, &ts);
	if (error)
		abort();
	for (;;) {
		kevent(kq, NULL, 0, &kev, 1, NULL);
		assert(kev.filter == EVFILT_READ);
		/* XXX */
	}
	close(kq);
	return (NULL);
}
 
static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = malloc(sizeof(struct buf));
	if (buf == NULL)
		return (NULL);
	buf->data = malloc(size);
	buf->size = size;
	if (buf->data == NULL) {
		free(buf);
		return (NULL);
	}
	buf->in = 0;
	buf->out = 0;
	return (buf);
}

#if 0
/*
 * Read bytes from a channel.
 */
ssize_t
chan_read(struct chan *chan, void *buf, size_t size)
{
	ssize_t n;

	pthread_mutex_lock(&chan->lock);
	pthread_cond_wait(&chan->rdready, &chan->lock);
	n = min(chan->recvbuf->in, size);
	assert(n >= 0);
	chan->recvbuf->in -= n;
	chan->recvbuf.off += n;
	chan->flags &= CF_WINDOW;
	pthread_mutex_unlock(&chan->lock);
	return (n);
}
#endif

/*
 * Get a channel from its ID.  The channel is returned locked.
 */
static struct chan *
chan_get(int id)
{
	struct chan *chan;

	pthread_mutex_lock(&mux_lock);
	chan = chans[id];
	if (chan != NULL)
		pthread_mutex_lock(&chan->lock);
	pthread_mutex_unlock(&mux_lock);
	return (chan);
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
		return (NULL);
	chan->state = CS_UNUSED;
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
#ifndef NDEBUG
	abort();
#endif
	return (-1);
}

static int
chan_connect(int id)
{
	struct chan *chan;
	int ok;

	chan = chan_get(id);
	chan->state = CS_CONNECTING;
	chan->flags &= CF_CONNECT;
	pthread_mutex_unlock(&chan->lock);
	pthread_cond_signal(&newwork);
	chan = chan_get(id);
	assert(chan != NULL);
	while (chan->state == CS_CONNECTING)
		pthread_cond_wait(&chan->wrready, &chan->lock);
	ok = (chan->state != CS_ESTABLISHED);
	pthread_mutex_unlock(&chan->lock);
	return (ok);
}
