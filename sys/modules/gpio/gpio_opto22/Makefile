.PATH:  ${.CURDIR}/../../../dev/gpio/

KMOD  =  gpio_opto22g4
SRCS  =  gpio_opto22.c
SRCS +=  device_if.h bus_if.h gpio_if.h gpiobus_if.h opt_platform.h ofw_bus_if.h

CFLAGS+=  -I. -I${.CURDIR}/../../../dev/gpio/

.include <bsd.kmod.mk>

