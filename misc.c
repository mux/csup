/*-
 * Copyright (c) 2003-2005, Maxime Henrion <mux@FreeBSD.org>
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

#include <openssl/md5.h>

#include <err.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "misc.h"

int
lprintf(int level, const char *fmt, ...)
{
	FILE *to;
	va_list ap;
	int ret;

	if (level > verbose)
		return (0);
	if (level == -1)
		to = stderr;
	else
		to = stdout;
	va_start(ap, fmt);
	ret = vfprintf(to, fmt, ap);
	va_end(ap);
	return (ret);
}

/*
 * Compute the MD5 checksum of a file.  The md parameter must
 * point to a buffer containing at least MD5_DIGEST_SIZE bytes.
 *
 * Do not confuse OpenSSL's MD5_DIGEST_LENGTH with our own
 * MD5_DIGEST_SIZE macro.
 */
int
MD5_File(char *path, char *md)
{
	char buf[1024];
	MD5_CTX ctx;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return (-1);
	MD5_Init(&ctx);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		MD5_Update(&ctx, buf, n);
	close(fd);
	if (n == -1)
		return (-1);
	MD5_End(md, &ctx);
	return (0);
}

/*
 * Wrapper around MD5_Final() that converts the 128 bits MD5 hash
 * to an ASCII string representing this value in hexadecimal.
 */
void
MD5_End(char *md, MD5_CTX *c)
{
	unsigned char md5[MD5_DIGEST_LENGTH];
	const char hex[] = "0123456789abcdef";
	int i, j;

	MD5_Final(md5, c);
	j = 0;
	for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
		md[j++] = hex[md5[i] >> 4];
		md[j++] = hex[md5[i] & 0xf];
	}
	md[j] = '\0';
}

int
pathcmp(const char *s1, const char *s2)
{
	int c1, c2;

	do {
		c1 = *s1++ & 0xff;
		if (c1 == '/') {
			if (*s1 != '\0')
				c1 = 0x100;
			else
				c1 = 0;
		}
		c2 = *s2++ & 0xff;
		if (c2 == '/') {
			if (*s2 != '\0')
				c2 = 0x100;
			else
				c2 = 0;
		}
	} while (c1 == c2 && c1 != '\0');

	return (c1 - c2);
}

char *
pathlast(char *path)
{
	char *s;

	s = strrchr(path, '/');
	if (s == NULL)
		s = path;
	else
		s++;
	return (s);
}

/*
 * Returns a buffer allocated with malloc() containing the absolute
 * pathname to the checkout file made from the prefix, and the path
 * of the corresponding RCS file relatively to the prefix.  If the
 * filename is not an RCS filename, NULL will be returned.
 */
char *
checkoutpath(const char *prefix, const char *file)
{
	const char *cp;
	char *path;
	size_t len;

	if (file[0] == '/')
		return (NULL);
	cp = file;
	while ((cp = strstr(cp, "..")) != NULL) {
		if (cp == file || cp[2] == '\0' ||
		    (cp[-1] == '/' && cp[2] == '/'))
			return (NULL);
		cp += 2;
	}
	len = strlen(file);
	if (len < 2 || file[len - 1] != 'v' || file[len - 2] != ',')
		return (NULL);
	asprintf(&path, "%s/%.*s", prefix, (int)len - 2, file);
	if (path == NULL)
		err(1, "asprintf");
	return (path);
}
