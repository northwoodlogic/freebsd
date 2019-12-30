/*-
 * Copyright (c) 2019 Dave Rush <northwoodogic@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This driver is initially intended to be used with the Raspberry PI Opto22
 * adapter board, OPTO-P1-40P. This board has a 16 I/O points connected via
 * bidirectional bus switch that is used as a 5 to 3.3V level translator. The
 * G4 modules / GPIOs are used in an active low / open drain configuration. In
 * order to avoid driver contention with input modules, the device tree
 * specification MUST specify these GPIO pins as active low.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_platform.h"

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus.h>

#include <dev/gpio/gpiobusvar.h>

#include "gpiobus_if.h"

/*
 * G4 relay racks have up to 24 I/O points. The number instantiated is
 * determined by the device tree.
 */

#define OPTO22_G4_MAX 24

struct opto22_softc {
	device_t	dev;
	int		nio;
	struct cdev*	ctl[OPTO22_G4_MAX];
	gpio_pin_t	pin[OPTO22_G4_MAX];
};

static d_ioctl_t opto22_ioctl;
static d_open_t	opto22_open;
static d_close_t opto22_close;
static d_read_t	opto22_read;
static d_write_t opto22_write;

static int opto22_probe(device_t);
static int opto22_attach(device_t);
static int opto22_detach(device_t);

static struct cdevsw opto22_cdevsw = {
    .d_version  = D_VERSION,
    .d_open	= opto22_open,
    .d_close	= opto22_close,
    .d_read	= opto22_read,
    .d_write	= opto22_write,
    .d_ioctl    = opto22_ioctl,
    .d_name     = "opto22g4",
};

#ifndef FDT
#error This module requires FDT support
#endif

static int
opto22_probe(device_t dev)
{
	if (!ofw_bus_is_compatible(dev, "opto22g4"))
		return (ENXIO);

	device_set_desc(dev, "OPTO22/G4");

	return BUS_PROBE_DEFAULT;
}

static int
opto22_attach(device_t dev)
{
	int i;
	int err;
	phandle_t node;

	struct opto22_softc *sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(sc->dev);

	/* Pull out GPIOs until there are no more, up to a max of 24 */
	for (i = 0; i < OPTO22_G4_MAX; i++) {
		err = gpio_pin_get_by_ofw_idx(sc->dev, node, i, &(sc->pin[i]));
		if (err)
			break;

		sc->ctl[i] = make_dev(&opto22_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600,
					"g4-%d.%d", device_get_unit(sc->dev), i);

		sc->ctl[i]->si_drv1 = sc->pin[i];
		device_printf(sc->dev, "I/O [%d], GPIO %d\n", i, sc->pin[i]->pin);

		if (gpio_pin_setflags(sc->pin[i], GPIO_PIN_INPUT) != 0)
			device_printf(sc->dev, " Unable to initialize pin input: %d\n", i);

		/*
		 * See note above, this depends on device tree setting active
		 * low / high pin flags correctly. Asserting / deassert is
		 * done by toggling pin between output and input states
		 * simulating an open drain configuration.
		 */
		if (gpio_pin_set_active(sc->pin[i], 1) != 0)
			device_printf(sc->dev, " Unable to initialize pin direction: %d\n", i);
	}
	sc->nio = i;

	device_printf(sc->dev, "  G4 Rack: %d I/O\n", sc->nio);
	return 0;
}

static int
opto22_detach(device_t dev)
{
	int i;
	struct opto22_softc *sc = device_get_softc(dev);

	for (i = 0; i < sc->nio; i++) {
		if (sc->ctl[i] == NULL)
			continue;

		destroy_dev(sc->ctl[i]);
		gpio_pin_release(sc->pin[i]);
	}

	return 0;
}

static int
opto22_open(struct cdev *cdev, int oflags, int devtype, struct thread *td)
{
	return 0;
}

static int
opto22_close(struct cdev *cdev, int fflag, int devtype, struct thread *td)
{
	return 0;
}

static int
opto22_read(struct cdev *cdev, struct uio *uio, int ioflag)
{
	int err;
	size_t amt;
	bool val = 0;
	char buf[2] = { '0', '0' };
	gpio_pin_t pin = (gpio_pin_t)cdev->si_drv1;

	err = gpio_pin_is_active(pin, &val);
	if (err)
		return err;

	buf[0] = val ? '1' : '0';

	/* zero byte read signals EOF */
	amt = MIN(uio->uio_resid, uio->uio_offset > 0 ? 0 : 1);
	err = uiomove(buf, amt, uio);
	return err;
}

/* Write function sets I/O mode (input or output) or changes I/O module state.
 * The following ASCII values are valid:
 * - "0" Configure I/O point as input
 * - "1" Configure I/O point as output and assert
 *
 * There is no risk of driver contention because G4 modules use open drain
 * signaling.
 */
static int
opto22_write(struct cdev *cdev, struct uio *uio, int ioflag)
{
	int err;
	size_t amt;
	/*
	 * Allow up to 2 bytes to support 'echo 1 > /dev/g4-0.x' stuff with
	 * newlines.
	 */
	char buf[2] = { '0', '0' };
	gpio_pin_t pin = (gpio_pin_t)cdev->si_drv1;

	/* Only allow writes from beginning */
	if (uio->uio_offset != 0)
		return EINVAL;

	amt = MIN(uio->uio_resid, sizeof(buf));
	err = uiomove(buf, amt, uio);
	if (err)
		return err;

	if (buf[0] == '0' || buf[0] == 0x00) {
		err = gpio_pin_setflags(pin, GPIO_PIN_INPUT);
	} else if (buf[0] == '1' || buf[0] == 0x01) {
		/* pin output register asserted during attach */
		err = gpio_pin_setflags(pin, GPIO_PIN_OUTPUT);
	} else {
		err = EINVAL;
	}

	return err;
}

static int 
opto22_ioctl(struct cdev *cdev, u_long cmd, caddr_t arg, int fflag, 
    struct thread *td)
{
	return ENOTTY;
}

static devclass_t opto22_devclass;

static device_method_t opto22_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		opto22_probe),
	DEVMETHOD(device_attach,	opto22_attach),
	DEVMETHOD(device_detach,	opto22_detach),

	DEVMETHOD_END
};

static driver_t opto22_driver = {
	"opto22g4",
	opto22_methods,
	sizeof(struct opto22_softc),
};

DRIVER_MODULE(opto22g4, ofwbus, opto22_driver, opto22_devclass, 0, 0);
DRIVER_MODULE(opto22g4, simplebus, opto22_driver, opto22_devclass, 0, 0);
MODULE_DEPEND(opto22g4, gpiobus, 1, 1, 1);

