# $FreeBSD$

PROG=	csup
SRCS=	main.h main.c proto.h proto.c token.l parse.y y.tab.h parse.h \
	config.h config.c mux.h mux.c
#CFLAGS+=-DNDEBUG -g
CFLAGS+=-g
WARNS?=	6
NOMAN=	yes
DPADD=	${LIBL} ${LIBKSE}
LDADD=	-ll -lkse

.include <bsd.prog.mk>
