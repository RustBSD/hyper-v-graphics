# $FreeBSD$
#
# Out-of-tree build of the Hyper-V synthetic video driver.
#
# Build with:
#	make
#
# The result is hypervgfx.ko, which can be loaded with kldload(8).

SYSDIR?=	/usr/src/sys

.include "${SYSDIR}/conf/kern.opts.mk"

KMOD=	hypervgfx

SRCS=	hypervgfx.c \
	hypervgfx_proto.c \
	hypervgfx_fb.c

SRCS+=	opt_global.h \
	bus_if.h \
	device_if.h

CFLAGS+= -I${SYSDIR}/dev/hyperv/include

.include <bsd.kmod.mk>
