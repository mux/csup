/*-
 * Copyright (c) 2004-2006, Maxime Henrion <mux@FreeBSD.org>
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
 * $FreeBSD: projects/csup/threads.c,v 1.5 2006/02/07 04:00:30 mux Exp $
 */

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#include "misc.h"
#include "queue.h"
#include "threads.h"

/*
 * This API is a wrapper around the pthread(3) API, which mainly
 * allows me to wait for multiple threads to exit.  We use a
 * condition variable to signal a thread's death.  All threads
 * created with this API have a common entry/exit point, so we
 * don't need to add any code in the threads themselves.
 */

/* Structure describing a thread. */
struct thread {
	pthread_t thread;
	void *(*start)(void *);
	void *data;
	struct threads *threads;
	LIST_ENTRY(thread) runlist;
	STAILQ_ENTRY(thread) deadlist;
};

/* A set of threads. */
struct threads {
	pthread_mutex_t threads_mtx;
	pthread_cond_t thread_exited;
	LIST_HEAD(, thread) threads_running;
	STAILQ_HEAD(, thread) threads_dead;
};

static void	*thread_start(void *);	/* Common entry point for threads. */

static void	 threads_lock(struct threads *);
static void	 threads_unlock(struct threads *);

static void
threads_lock(struct threads *tds)
{
	int error;

	error = pthread_mutex_lock(&tds->threads_mtx);
	assert(!error);
}

static void
threads_unlock(struct threads *tds)
{
	int error;

	error = pthread_mutex_unlock(&tds->threads_mtx);
	assert(!error);
}

/* Create a new set of threads. */
struct threads *
threads_new(void)
{
	struct threads *tds;

	tds = xmalloc(sizeof(struct threads));
	pthread_mutex_init(&tds->threads_mtx, NULL);
	pthread_cond_init(&tds->thread_exited, NULL);
	LIST_INIT(&tds->threads_running);
	STAILQ_INIT(&tds->threads_dead);
	return (tds);
}

/*
 * Create a new thread in this set.  The returned void * uniquely
 * identifies the newly created thread.  The caller will need it
 * later after threads_wait() to know which of the threads exited.
 */
void *
threads_create(struct threads *tds, void *(*start)(void *), void *data)
{
	struct thread *td;
	int error;

	td = xmalloc(sizeof(struct thread));
	td->threads = tds;
	td->start = start;
	td->data = data;
	threads_lock(tds);
	error = pthread_create(&td->thread, NULL, thread_start, td);
	if (error) {
		pthread_mutex_unlock(&tds->threads_mtx);
		free(td);
		return (NULL);
	}
	LIST_INSERT_HEAD(&tds->threads_running, td, runlist);
	threads_unlock(tds);
	return (td);
}

/* Wait for any of the threads in the set to exit. */
void *
threads_wait(struct threads *tds)
{
	struct thread *td;

	threads_lock(tds);
	while (STAILQ_EMPTY(&tds->threads_dead)) {
		assert(!LIST_EMPTY(&tds->threads_running));
		pthread_cond_wait(&tds->thread_exited, &tds->threads_mtx);
	}
	td = STAILQ_FIRST(&tds->threads_dead);
	STAILQ_REMOVE_HEAD(&tds->threads_dead, deadlist);
	threads_unlock(tds);
	free(td);
	return (td);
}

/* Free a threads set. */
void
threads_free(struct threads *tds)
{

	assert(LIST_EMPTY(&tds->threads_running));
	assert(STAILQ_EMPTY(&tds->threads_dead));
	pthread_cond_destroy(&tds->thread_exited);
	pthread_mutex_destroy(&tds->threads_mtx);
	free(tds);
}

/*
 * Common entry point for threads.  This just calls the real start
 * routine, and then signals the thread's death, after having
 * removed the thread from the list.
 */
static void *
thread_start(void *data)
{
	struct threads *tds;
	struct thread *td;

	td = data;
	tds = td->threads;
	td->start(td->data);
	threads_lock(tds);
	LIST_REMOVE(td, runlist);
	STAILQ_INSERT_TAIL(&tds->threads_dead, td, deadlist);
	pthread_cond_signal(&tds->thread_exited);
	threads_unlock(tds);
	return (NULL);
}
