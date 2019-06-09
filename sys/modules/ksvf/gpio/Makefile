.PATH:  ${.CURDIR}/../../../dev/ksvf/

KMOD=   ksvf_gpio
SRCS=   ksvf_gpio.c
SRCS+=  device_if.h bus_if.h gpio_if.h gpiobus_if.h 

CFLAGS+=  -I. -I${.CURDIR}/../../../dev/ksvf/

.include <bsd.kmod.mk>

