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
#include <sys/time.h>

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
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
	uint8_t		id;

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

static uint8_t		chan_alloc(void);
static struct chan	*chan_new(uint8_t);
static void		*sender_loop(void *);
static void		*receiver_loop(void *);

static pthread_mutex_t mux_lock;
static struct chan *chans[MUX_MAXCHAN];
static int nchans;
static pthread_t sender;
static pthread_t receiver;

uint8_t
mux_open(int s)
{
	uint8_t id;

	nchans = 0;
	pthread_mutex_init(&mux_lock, NULL);
	id = chan_alloc();
	assert(id == 0);
	pthread_create(&sender, NULL, sender_loop, &s);
	pthread_create(&receiver, NULL, receiver_loop, &s);
	return (id);
}

/*
 * Returns the ID of an available channel in the listening state.
 */
uint8_t
mux_listen(void)
{

	return (chan_alloc());
}

void *
sender_loop(void *arg)
{
	int kq, s;

	s = *(int *)arg;
	kq = kqueue();
	if (kq == -1)
		return (NULL);
	close(kq);
	return (NULL);
}

void *
receiver_loop(void *arg)
{
	int kq, s;

	s = *(int *)arg;
	kq = kqueue();
	if (kq == -1)
		return (NULL);
	close(kq);
	return (NULL);
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

static struct buf *
buf_new(size_t size)
{
	struct buf *buf;

	buf = malloc(sizeof(struct buf));
	assert(buf != NULL);
	buf->data = malloc(size);
	buf->size = size;
	assert(buf->data != NULL);
	buf->in = 0;
	buf->out = 0;
	return (buf);
}

static struct chan *
chan_new(uint8_t id)
{
	struct chan *chan;

	chan = calloc(1, sizeof(struct chan));
	assert(chan != NULL);
	chan->id = id;
	chan->state = CS_UNUSED;
	chan->recvmss = CHAN_MAXSEGSIZE;
	chan->sendbuf = buf_new(CHAN_SBSIZE);
	chan->recvbuf = buf_new(CHAN_RBSIZE);
	pthread_mutex_init(&chan->lock, NULL);
	pthread_cond_init(&chan->rdready, NULL);
	pthread_cond_init(&chan->wrready, NULL);
	return (chan);
}

/* Return an available channel in the listening state. */
static uint8_t
chan_alloc(void)
{
	struct chan *chan;
	uint8_t id;

	pthread_mutex_lock(&mux_lock);
	assert(nchans < MUX_MAXCHAN);
	id = nchans++;
	chan = chan_new(id);
	chan->state = CS_LISTENING;
	chans[id] = chan;
	pthread_mutex_unlock(&mux_lock);
	return (id);
}
