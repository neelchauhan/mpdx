# $Id$

PREFIX?=	/usr/local
BINDIR?=	/etc/mpdx

BINOWN!=	/usr/bin/id -n -u
BINGRP!=	/usr/bin/id -n -g

MPD_SAMPLES+=	mpd.conf.sample
MPD_SAMPLES+=	mpd.secret.sample
MPD_SAMPLES+=	mpd.script.sample
NOOBJ=
NO_OBJ=

all clean cleandir depend:

afterinstall:
	[ -d ${DESTDIR}${PREFIX}${BINDIR} ] || mkdir ${DESTDIR}${PREFIX}${BINDIR}
	${INSTALL} -c -o ${BINOWN} -g ${BINGRP} -m ${SHAREMODE} ${MPD_SAMPLES} \
		${DESTDIR}${PREFIX}${BINDIR}

.include <bsd.prog.mk>
