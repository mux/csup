# $FreeBSD$

PROG=	csup
SRCS=	config.c config.h detailer.c detailer.h fileattr.c fileattr.h \
	lister.c lister.h main.c main.h misc.c misc.h mux.c mux.h osdep.h \
	parse.h parse.y proto.h proto.c token.l updater.c updater.h y.tab.h 
CFLAGS+=-g	# -DNDEBUG
WARNS?=	6
NOMAN=	yes
DPADD=	${LIBL} ${LIBCRYPTO}
LDADD=	-ll -lcrypto

DPADD+=	${LIBKSE}
LDADD+=	-lkse

.include <bsd.prog.mk>
