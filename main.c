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

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "proto.h"
#include "main.h"

#define	USAGE_OPTFMT	"    %-12s %s\n"

int verbose = 1;

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [options] supfile [destDir]\n",
	    getprogname());
	fprintf(stderr, "  Options:\n");
	fprintf(stderr, USAGE_OPTFMT, "-b base", 
	    "Override supfile's \"base\" directory");
	fprintf(stderr, USAGE_OPTFMT, "-h host",
	    "Override supfile's \"host\" name");
	fprintf(stderr, USAGE_OPTFMT, "-L n",
	    "Verbosity level (0..2, default 1)");
	fprintf(stderr, USAGE_OPTFMT, "-p port",
	    "Alternate server port (default 5999)");
	fprintf(stderr, USAGE_OPTFMT, "-v", "Print version and exit");
}

int
main(int argc, char *argv[])
{
	FILE *f;
	struct config *config;
	char *base, *host, *file;
	in_port_t port;
	char c;

	port = 0;
	base = host = NULL;
	while ((c = getopt(argc, argv, "b:gh:L:p:P:v")) != -1) {
		switch (c) {
		case 'b':
			base = optarg;
			break;
		case 'g':
			/* For compatibility. */
			break;
		case 'h':
			host = optarg;
			break;
		case 'L':
			errno = 0;
			verbose = strtol(optarg, NULL, 0);
			if (errno == EINVAL) {
				fprintf(stderr, "Invalid verbosity\n");
				usage();
				return (1);
			}
			break;
		case 'p':
			errno = 0;
			port = strtol(optarg, NULL, 0);
			if (errno == EINVAL) {
				fprintf(stderr, "Invalid server port\n");
				usage();
				return (1);
			}
			break;
		case 'P':
			/* For compatibility. */
			if (strcmp(optarg, "m") != 0) {
				fprintf(stderr,
				    "Client only supports multiplexed mode\n");
				return (1);
			}
			break;
		case 'v':
			fprintf(stderr, "CVSup lite version 0.1\n");
			return (0);
			break;
		case '?':
		default:
			usage();
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return (1);
	}

	file = argv[0];
	lprintf(2, "Parsing supfile \"%s\"\n", file);
	config = config_init(file, host, base, port);

#ifdef DEBUG
	config_print(config);
#endif
	lprintf(2, "Connecting to %s\n", config->host);
	f = cvsup_connect(config);
	if (f == NULL) {
		lprintf(0, "Cannot connect to %s: %s\n", config->host,
		    strerror(errno));
		return (1);
	}
	lprintf(1, "Connected to %s\n", config->host);
	cvsup_init(f, config);
	return (0);
}

int
lprintf(int level, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (level <= verbose) {
		va_start(ap, fmt);
		ret = vprintf(fmt, ap);
		va_end(ap);
		return (ret);
	}
	return (0);
}
