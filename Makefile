# $FreeBSD$

PROG=	csup
SRCS=	main.h main.c proto.h proto.c token.l parse.y y.tab.h parse.h \
	file.h config.h config.c mux.h mux.c lister.h lister.c \
	detailer.h detailer.c
#CFLAGS+=-DNDEBUG -g
CFLAGS+=-g
WARNS?=	6
NOMAN=	yes
#DPADD=	${LIBL} ${LIBKSE}
#LDADD=	-ll -lkse
DPADD=	${LIBL} ${LIBTHR}
LDADD=	-ll -lthr

.include <bsd.prog.mk>
