# $FreeBSD$

PROG=	csup
SRCS=	config.c config.h \
	detailer.c detailer.h \
	diff.c diff.h \
	fileattr.c fileattr.h \
	keyword.c keyword.h \
	lister.c lister.h \
	main.c main.h \
	misc.c misc.h \
	mux.c mux.h \
	osdep.h \
	parse.h parse.y \
	proto.c proto.h \
	stream.c stream.h \
	token.l \
	updater.c updater.h \
	y.tab.h 
CFLAGS+=-g -pthread # -DNDEBUG
WARNS?=	6
NOMAN=	yes
DPADD=	${LIBL} ${LIBCRYPTO}
LDADD=	-ll -lcrypto

.include <bsd.prog.mk>
