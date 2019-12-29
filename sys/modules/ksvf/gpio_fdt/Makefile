.PATH:  ${.CURDIR}/../../../dev/ksvf/

KMOD  =   ksvf_gpio_fdt
SRCS  =  ksvf_gpio_fdt.c
SRCS +=  device_if.h bus_if.h gpio_if.h gpiobus_if.h opt_platform.h ofw_bus_if.h

CFLAGS+=  -I. -I${.CURDIR}/../../../dev/ksvf/

.include <bsd.kmod.mk>

