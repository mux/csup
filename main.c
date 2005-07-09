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

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "misc.h"
#include "proto.h"

#define	USAGE_OPTFMT	"    %-12s %s\n"

int verbose = 1;

static void
usage(char *argv0)
{
	lprintf(-1, "Usage: %s [options] supfile [destDir]\n", basename(argv0));
	lprintf(-1, "  Options:\n");
	lprintf(-1, USAGE_OPTFMT, "-b base", 
	    "Override supfile's \"base\" directory");
	lprintf(-1, USAGE_OPTFMT, "-c collDir",
	    "Subdirectory of \"base\" for collections (default \"sup\")");
	lprintf(-1, USAGE_OPTFMT, "-h host",
	    "Override supfile's \"host\" name");
	lprintf(-1, USAGE_OPTFMT, "-l lockfile",
	    "Lock file during update; fail if already locked");
	lprintf(-1, USAGE_OPTFMT, "-L n",
	    "Verbosity level (0..2, default 1)");
	lprintf(-1, USAGE_OPTFMT, "-p port",
	    "Alternate server port (default 5999)");
	lprintf(-1, USAGE_OPTFMT, "-v", "Print version and exit");
	lprintf(-1, USAGE_OPTFMT, "-z", "Enable compression for all "
	    "collections");
	lprintf(-1, USAGE_OPTFMT, "-Z", "Disable compression for all "
	    "collections");
}

int
main(int argc, char *argv[])
{
	struct config *config;
	char *argv0, *base, *colldir, *host, *file, *lockfile;
	in_port_t port;
	int c, compress, error, lockfd, lflag;

	port = 0;
	compress = 0;
	lflag = 0;
	lockfd = 0;
	argv0 = argv[0];
	base = colldir = host = lockfile = NULL;
	while ((c = getopt(argc, argv, "b:c:gh:l:L:p:P:vzZ")) != -1) {
		switch (c) {
		case 'b':
			base = optarg;
			break;
		case 'c':
			colldir = optarg;
			break;
		case 'g':
			/* For compatibility. */
			break;
		case 'h':
			host = optarg;
			break;
		case 'l':
			lockfile = optarg;
			lflag = 1;
			lockfd = open(lockfile, O_EXLOCK | O_CREAT | O_NONBLOCK,
			    0700);
			if (lockfd == -1 && errno == EAGAIN) {
				lprintf(-1, "\"%s\" is already locked "
				    "by another process\n", lockfile);
				return (1);
			}
			if (lockfd == -1) {
				lprintf(-1, "Error locking \"%s\": %s\n",
				    lockfile, strerror(errno));
				return (1);
			}
			break;
		case 'L':
			errno = 0;
			verbose = strtol(optarg, NULL, 0);
			if (errno == EINVAL) {
				lprintf(-1, "Invalid verbosity\n");
				usage(argv0);
				return (1);
			}
			break;
		case 'p':
			/* Use specified server port. */
			errno = 0;
			port = strtol(optarg, NULL, 0);
			if (errno == EINVAL) {
				lprintf(-1, "Invalid server port\n");
				usage(argv0);
				return (1);
			}
			break;
		case 'P':
			/* For compatibility. */
			if (strcmp(optarg, "m") != 0) {
				lprintf(-1,
				    "Client only supports multiplexed mode\n");
				return (1);
			}
			break;
		case 'v':
			lprintf(-1, "Csup version 0.1\n");
			return (0);
			break;
		case 'z':
			/* Force compression on all collections. */
			compress = 1;
			break;
		case 'Z':
			/* Disables compression on all collections. */
			compress = -1;
			break;
		case '?':
		default:
			usage(argv0);
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage(argv0);
		return (1);
	}

	file = argv[0];
	lprintf(2, "Parsing supfile \"%s\"\n", file);
	config = config_init(file, host, base, colldir, port, compress);
	lprintf(2, "Connecting to %s\n", config->host);
	error = proto_connect(config);
	if (error)
		return (1);
	lprintf(1, "Connected to %s\n", config->host);
	proto_init(config);
	if (lflag) {
		unlink(lockfile);
		close(lockfd);
	}
	return (0);
}
