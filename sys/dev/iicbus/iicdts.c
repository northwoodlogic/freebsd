/*-
 * Copyright (c) 2017,2019 Dave Rush.  All rights reserved.
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
 * Texas Instruments TMP100 digital thermal sensor
 *
 * This device is fairly close in register format to the LM75 and in fact
 * the Linux driver for this part is unified in that is supports many LM75
 * derivitives. The LM75 driver in the FreeBSD tree isn't set up that way
 * which is why this driver exists separately.
 */

#include "opt_platform.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#ifdef FDT
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iiconf.h>
#include "iicbus_if.h"

/*
 * Add this to the measured millidegrees read from the part
 *
 * T(C) = T(K) - 273.15
 *
 */
#define MILLIKELVIN_OFFSET 273150

/*
 * The largest register The TMP100 has is 2 bytes
 */
#define	MAX_IIC_DATA_SIZE	2

#define TMP100_ADC_REG 0x00
#define TMP100_CFG_REG 0x01

/* OR these together to configure the device */
#define TMP100_CFG_BITS_MASK 0x60
#define TMP100_CFG_BITS_12   0x60

struct iicdts_softc {
	device_t	sc_dev;
	uint16_t	sc_addr;
};

static int
iicdts_read(device_t dev, uint8_t reg, uint8_t *data, uint8_t size);

static int
iicdts_write(device_t dev, uint8_t reg, uint8_t *data, uint8_t size);

/*
 * Configure the ADC for 12 bit mode if it's not already.
 */
static int
iicdts_configure(device_t dev)
{
	int error;
	uint8_t config = 0;

	error = iicdts_read(dev, TMP100_CFG_REG, &config, sizeof(config));
	if (error != 0) {
		device_printf(dev, "unable to read device configuration\n");
		return (error);
	}

	/* Already configured for 12 bits resolution */
	if ((config & TMP100_CFG_BITS_MASK) == TMP100_CFG_BITS_12)
		return (0);

	config = TMP100_CFG_BITS_12;

	error = iicdts_write(dev, TMP100_CFG_REG, &config, sizeof(config));
	if (error != 0) {
		device_printf(dev, "unable to configure 12 bits mode\n");
		return (error);
	}

	return (0);
}

static int
iicdts_sysctl_temperature(SYSCTL_HANDLER_ARGS)
{
	int error;
	int value;
	int16_t adc;
	uint8_t bytes[2] = { 0, 0 };
	struct iicdts_softc *sc;
	sc = (struct iicdts_softc*)arg1;

	if (iicdts_configure(sc->sc_dev) != 0)
		return (EIO);

	/*
	 * Pointer register value 0 will read back temperature conversion
	 */
	if (iicdts_read(sc->sc_dev, 0x00, bytes, sizeof(bytes)) != 0) {
		device_printf(sc->sc_dev, "unable to read temperature value\n");
		return (EIO);
	}

	/*
	 * With 12-bits used in the computation, each LSB tick represents
	 * 0.0625 DegC. This is independent of the actual ADC converter
	 * configuration. The sysctl is returns temperature data in
	 * milli-Kelvin. The ADC returns data in two's compliment where
	 * zero is zero DegC.
	 *
	 * Multiplying the converter value by 625 and then dividing the
	 * result by 10 gives the correct value in milli-DegC. It's a simple
	 * offset addition to convert that to milli-Kelvin.
	 */

	/*
	 * Build a 16 bit two's compliment value, then shift right by
	 * 4 to get a 12 bit two's compliement value.
	 */
	adc = ((uint16_t)bytes[0] << 8) | ((uint16_t)bytes[1] & 0xFF);
	adc = adc >> 4; /* Gets to 12 bits */
	value  = adc;   /* Sign extended, prevent overflow */
	value *= 625;   /* computes 1/10000'th of a degree */
	value /= 10;    /* get back to milli-DegC. */

	// shift from degC to degK
	value += MILLIKELVIN_OFFSET;

	error = sysctl_handle_int(oidp, &value, sizeof(value), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	return (0);
}

/*
 * There are numerous I2C digital temperature sensor devices that could be
 * added to this driver. This driver was brought up with a TMP100 from Texas
 * Instruments. The other devices differ mainly in converter resolution or
 * contain additional thermostat control registers.
 */
static int
iicdts_probe(device_t dev)
{

#ifdef FDT
	if (!ofw_bus_is_compatible(dev, "ti,tmp100"))
		return (ENXIO);
#endif
	device_set_desc(dev, "Texas Instruments TMP100 Thermal Sensor");
	return (BUS_PROBE_GENERIC);
}

static int
iicdts_read(device_t dev, uint8_t reg, uint8_t *data, uint8_t size)
{

	struct iicdts_softc *sc = device_get_softc(dev);
	struct iic_msg msgs[2] = {
	     { sc->sc_addr, IIC_M_WR, 1, &reg },
	     { sc->sc_addr, IIC_M_RD, size, data }
	};
	return (iicbus_transfer(dev, msgs, 2));
}

static int
iicdts_write(device_t dev, uint8_t reg, uint8_t *data, uint8_t size)
{

	struct iicdts_softc *sc = device_get_softc(dev);
	uint8_t buffer[MAX_IIC_DATA_SIZE + 1];
	struct iic_msg msgs[1] = {
	     { sc->sc_addr, IIC_M_WR, size + 1, buffer },
	};

	if (size > MAX_IIC_DATA_SIZE)
		return (ENOMEM);

	buffer[0] = reg;
	memcpy(buffer + 1, data, size);
	return (iicbus_transfer(dev, msgs, 1));
}

static int
iicdts_attach(device_t dev)
{

	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree_node;
	struct sysctl_oid_list *tree;
	struct iicdts_softc *sc = device_get_softc(dev);

	sc->sc_dev = dev;
	sc->sc_addr = iicbus_get_addr(dev);

	ctx = device_get_sysctl_ctx(dev);
	tree_node = device_get_sysctl_tree(dev);
	tree = SYSCTL_CHILDREN(tree_node);
	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "temperature",
		CTLTYPE_INT | CTLFLAG_RD, sc, sizeof(*sc),
		iicdts_sysctl_temperature, "IK3", "Temperature");
	/*
	 * Note on temperature formatting:
	 * "IK" will print a value in 1/10th of a degree C.
	 * "IK0" prints in degC directly, base units
	 * "IK3" prints in millidegC, 1/1000th of a degree C.
	 */

	return (0);
}

static int
iicdts_detach(device_t dev)
{
	return (0);
}

static device_method_t iicdts_methods[] = {
	DEVMETHOD(device_probe,		iicdts_probe),
	DEVMETHOD(device_attach,	iicdts_attach),
	DEVMETHOD(device_detach,	iicdts_detach),
	DEVMETHOD_END
};

static driver_t iicdts_driver = {
	"iicdts",
	iicdts_methods,
	sizeof(struct iicdts_softc),
};
static devclass_t iicdts_devclass;

DRIVER_MODULE_ORDERED(iicdts, iicbus, iicdts_driver, iicdts_devclass, 0, 0, SI_ORDER_ANY);
MODULE_VERSION(iicdts, 1);
MODULE_DEPEND(iicdts, iicbus, 1, 1, 1);
