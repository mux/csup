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

#include <sys/cdefs.h>

/*
 * Packet types.
 */
#define	MUX_STARTUPREQ	0
#define	MUX_STARTUPREP	1
#define	MUX_CONNECT	2			
#define	MUX_ACCEPT	3
#define	MUX_RESET	4
#define	MUX_DATA	5
#define	MUX_WINDOW	6
#define	MUX_CLOSE	7

/*
 * Misc defines.
 */
#define	MUX_PROTOVER	0			/* Protocol version. */
/*
 * Defines for fixed-size packets.
 */
#define	MUX_STARTUPPKTSZ	3
#define	MUX_CONNECTPKTSZ	8
#define	MUX_ACCEPTPKTSZ		8
#define	MUX_RESETPKTSZ		2
#define	MUX_WINDOWPKTSZ		6
#define	MUX_CLOSEPKTSZ		2

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

#define	mh_startup	mh_u.mh_startup
#define	mh_connect	mh_u.mh_connect
#define	mh_accept	mh_u.mh_accept
#define	mh_reset	mh_u.mh_reset
#define	mh_data		mh_u.mh_data
#define	mh_window	mh_u.mh_window
#define	mh_close	mh_u.mh_close

int	mux_open(int);
int	mux_listen(void);
size_t	chan_read(int, void *, size_t);
void	chan_write(int, const void *, size_t);
