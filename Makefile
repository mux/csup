# $FreeBSD$

PROG=	cvsup
SRCS=	main.h main.c proto.h proto.c token.l parse.y y.tab.h parse.h \
	config.h config.c mux.h
#CFLAGS+=-DDEBUG -g
CFLAGS+=-g
WARNS?=	6
NOMAN=	yes
LDADD=	-ll

.include <bsd.prog.mk>
