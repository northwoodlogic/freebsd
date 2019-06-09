.PATH:  ${.CURDIR}/../../../dev/ksvf/

KMOD=   ksvf_ppbus
SRCS=   ksvf_ppbus.c
SRCS+=  device_if.h bus_if.h ppbus_if.h 

CFLAGS+=  -I. -I${.CURDIR}/../../../dev/ksvf/

.include <bsd.kmod.mk>

