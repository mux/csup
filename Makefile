# $FreeBSD: projects/csup/Makefile,v 1.44 2006/03/06 00:36:23 mux Exp $

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin
MANDIR?=	${PREFIX}/man/man

UNAME!=		/usr/bin/uname -s

PROG=	csup
SRCS=	attrstack.c auth.c config.c detailer.c diff.c fattr.c fixups.c fnmatch.c \
	globtree.c idcache.c keyword.c lister.c main.c misc.c mux.c parse.y \
	pathcomp.c proto.c status.c stream.c threads.c token.l updater.c \
	rcsfile.c rcsparse.c lex.rcs.c rsyncfile.c

CFLAGS+=	-I. -I${.CURDIR} -g -pthread -DHAVE_FFLAGS -DNDEBUG
WARNS?=		1

# A bit of tweaking is needed to get this Makefile working
# with the bsd.prog.mk of all the *BSD OSes...
.if (${UNAME} == "NetBSD")
LDFLAGS+=	-pthread
YHEADER=	yes

.elif (${UNAME} == "OpenBSD")
# I bet there's a better way to do this with the OpenBSD mk
# framework but well, this works and I got bored.
LDFLAGS+=	-pthread
YFLAGS=		-d
CLEANFILES+=	parse.c parse.h y.tab.h

config.c:	parse.h

token.l:	parse.h

y.tab.h:	parse.c

parse.h:	y.tab.h
	cp ${.ALLSRC} ${.TARGET}

.endif

DPADD=	${LIBZ}
LDADD=	-lz

.if (${UNAME} == "FreeBSD")
DPADD+=	${LIBMD}
LDADD+=	-lmd
.else
DPADD+=	${LIBCRYPTO}
LDADD+=	-lcrypto
.endif

SCRIPTS=	cpasswd.sh
MAN=		csup.1 cpasswd.1

.include <bsd.prog.mk>
