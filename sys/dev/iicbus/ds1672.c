/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2006 Sam Leffler.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");
/*
 * Dallas Semiconductor DS1672 RTC sitting on the I2C bus.
 */

#include "opt_platform.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/clock.h>
#include <sys/time.h>
#include <sys/bus.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#ifdef FDT
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include <dev/iicbus/iiconf.h>

#include "iicbus_if.h"
#include "clock_if.h"

#define	IIC_M_WR	0	/* write operation */

#define	DS1672_ADDR	0xd0	/* slave address */

#define	DS1672_COUNTER	0	/* counter (bytes 0-3) */
#define	DS1672_CTRL	4	/* control (1 byte) */
#define	DS1672_TRICKLE	5	/* trickle charger (1 byte) */

#define	DS1672_CTRL_EOSC	(1 << 7)	/* Stop/start flag. */

#define	MAX_IIC_DATA_SIZE	4

struct ds1672_softc {
	device_t		sc_dev;
};

static int
ds1672_gettime(device_t dev, struct timespec *ts);

static int
ds1672_sysctl_seconds(SYSCTL_HANDLER_ARGS)
{
	int error;
	uint32_t tod = 0;
	struct timespec ts;
	struct ds1672_softc *sc;
	sc = (struct ds1672_softc*)arg1;

	if ((error = ds1672_gettime(sc->sc_dev, &ts)) != 0)
		device_printf(sc->sc_dev, "Error reading time of day\n");
	else
		tod = (uint32_t)ts.tv_sec;

	error = sysctl_handle_int(oidp, &tod, sizeof(tod), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	return (0);
}

static int
ds1672_probe(device_t dev)
{
	/*
	 * With FDT enabled BUS_PROBE_NOWILDCARD prevents attachment
	 * even though the data in the device tree says it's here.
	 * If not using FDT edit device.hints or loader.conf to
	 * include:
	 *    hint.ds1672_rtc.0.at="iicbus1"
	 * This puts the rtc0 on iicbus1
	 */
	int rc;
#ifdef FDT
	if (!ofw_bus_is_compatible(dev, "dallas,ds1672"))
		return (ENXIO);
	rc = BUS_PROBE_GENERIC;
#else
	rc = BUS_PROBE_NOWIDLCARD;
#endif
	device_set_desc(dev, "Dallas Semiconductor DS1672 RTC");
	return (rc);
}

static int
ds1672_read(device_t dev, uint8_t addr, uint8_t *data, uint8_t size)
{
	struct iic_msg msgs[2] = {
	     { DS1672_ADDR, IIC_M_WR, 1, &addr },
	     { DS1672_ADDR, IIC_M_RD, size, data }
	};

	return (iicbus_transfer(dev, msgs, 2));
}

static int
ds1672_write(device_t dev, uint8_t addr, uint8_t *data, uint8_t size)
{
	uint8_t buffer[MAX_IIC_DATA_SIZE + 1];
	struct iic_msg msgs[1] = {
	     { DS1672_ADDR, IIC_M_WR, size + 1, buffer },
	};
	
	if (size > MAX_IIC_DATA_SIZE)
		return (ENOMEM);
	/* NB: register pointer precedes actual data */
	buffer[0] = addr;
	memcpy(buffer + 1, data, size);
	return (iicbus_transfer(dev, msgs, 1));
}

static int
ds1672_init(device_t dev)
{
	uint8_t ctrl;
	int error;

	error = ds1672_read(dev, DS1672_CTRL, &ctrl, 1);
	if (error)
		return (error);

	/*
	 * Check if oscillator is not running.
	 */
	if (ctrl & DS1672_CTRL_EOSC) {
		device_printf(dev, "RTC oscillator was stopped. Check system"
		    " time and RTC battery.\n");
		ctrl &= ~DS1672_CTRL_EOSC;	/* Start oscillator. */
		error = ds1672_write(dev, DS1672_CTRL, &ctrl, 1);
	}
	return (error);
}

static int
ds1672_detach(device_t dev)
{

    clock_unregister(dev);
    return (0);
}

static int
ds1672_attach(device_t dev)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree_node;
	struct sysctl_oid_list *tree;
	struct ds1672_softc *sc = device_get_softc(dev);

	sc->sc_dev = dev;

	/*
	 * On some HW devices (RPI-B I'm talking to you) a kernel panic will
	 * occur with a message saying "timed sleep before time counters
	 * enabled" if any iic bus activity is initiated during this attach
	 * function. Do a lazy init instead to prevent panic().
	 */

	ctx = device_get_sysctl_ctx(dev);
	tree_node = device_get_sysctl_tree(dev);
	tree = SYSCTL_CHILDREN(tree_node);
	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "seconds",
		CTLTYPE_INT | CTLFLAG_RW, sc, sizeof(*sc),
		ds1672_sysctl_seconds, "I", "time of day");

	clock_register(dev, 1000);
	return (0);
}

static int
ds1672_gettime(device_t dev, struct timespec *ts)
{
	uint8_t secs[4];
	int error;

	error = ds1672_init(dev);
	if (error)
		return (error);

	error = ds1672_read(dev, DS1672_COUNTER, secs, 4);
	if (error == 0) {
		/* counter has seconds since epoch */
		ts->tv_sec = (secs[3] << 24) | (secs[2] << 16)
			   | (secs[1] <<  8) | (secs[0] <<  0);
		ts->tv_nsec = 0;
	}
	clock_dbgprint_ts(dev, CLOCK_DBG_READ, ts); 
	return (error);
}

static int
ds1672_settime(device_t dev, struct timespec *ts)
{
	uint8_t data[4];

	data[0] = (ts->tv_sec >> 0) & 0xff;
	data[1] = (ts->tv_sec >> 8) & 0xff;
	data[2] = (ts->tv_sec >> 16) & 0xff;
	data[3] = (ts->tv_sec >> 24) & 0xff;

	ts->tv_nsec = 0;
	clock_dbgprint_ts(dev, CLOCK_DBG_WRITE, ts);
	return (ds1672_write(dev, DS1672_COUNTER, data, 4));
}

static device_method_t ds1672_methods[] = {
	DEVMETHOD(device_probe,		ds1672_probe),
	DEVMETHOD(device_attach,	ds1672_attach),
	DEVMETHOD(device_detach,	ds1672_detach),

	DEVMETHOD(clock_gettime,	ds1672_gettime),
	DEVMETHOD(clock_settime,	ds1672_settime),

	DEVMETHOD_END
};

static driver_t ds1672_driver = {
	"ds1672_rtc",
	ds1672_methods,
	sizeof(struct ds1672_softc),
};
static devclass_t ds1672_devclass;

DRIVER_MODULE_ORDERED(ds1672, iicbus, ds1672_driver, ds1672_devclass, 0, 0, SI_ORDER_ANY);
MODULE_VERSION(ds1672, 1);
MODULE_DEPEND(ds1672, iicbus, 1, 1, 1);
