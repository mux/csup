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

#include <openssl/md5.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "main.h"
#include "misc.h"

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

/*
 * Compute the MD5 checksum of a file.  The md parameter must
 * point to a buffer containing at least MD5_DIGEST_LEN + 1
 * bytes.  Warning, MD5_DIGEST_LEN is not the same as
 * MD5_DIGEST_LENGTH.  The former is defined to 32 in misc.h 
 * and represents the size of the digest when stored as a
 * string containing the digest in hexadecimal notation.
 * The latter is an OpenSSL define and its value is 16.
 */
int
MD5file(char *path, char *md)
{
	char buf[1024];
	unsigned char md5[MD5_DIGEST_LENGTH];
	const char hex[] = "0123456789abcdef";
	MD5_CTX ctx;
	ssize_t n;
	int fd, i, j;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return (-1);
	MD5_Init(&ctx);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		MD5_Update(&ctx, buf, n);
	close(fd);
	if (n == -1)
		return (-1);
	MD5_Final(md5, &ctx);
	/* Now convert the message digest to a string. */
	j = 0;
	for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
		md[j++] = hex[md5[i] >> 4];
		md[j++] = hex[md5[i] & 0xf];
	}
	md[j] = '\0';
	return (0);
}