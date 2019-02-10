/*-
 * Copyright (c) 2017 Dave Rush.  All rights reserved.
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
 * Texas Instruments INA220 Power Meter
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

#define INA220_CFG_REG    0x00
#define INA220_VSHUNT_REG 0x01
#define INA220_VBUS_REG   0x02
#define INA220_PBUS_REG   0x03
#define INA220_IBUS_REG   0x04
#define INA220_CAL_REG    0x05

#define INA220_PBUS_MULTIPLIER 20
#define INA220_VBUS_SHIFT_BITS 3
#define INA220_VBUS_LSB_MV     4

#define INA220_VSHUNT_LSB_UV   10

/*
 * The largest register the INA220 has is 2 bytes.
 */
#define	MAX_IIC_DATA_SIZE	sizeof(uint16_t)

struct ina220_softc {
	device_t	sc_dev;
	uint16_t	sc_addr;
/*
 * Configuration Register:
 * rcfg[2:0] - Mode
 *     3'b000  --> Power Down
 *     3'b001  --> Shunt Voltage, triggered
 *     3'b010  --> Bus Voltage, triggered
 *     3'b100  --> ADC Disabled
 *     3'b101  --> Shunt voltage, continuous
 *     3'b110  --> Bus voltage, continuous
 *     3'b111  --> Shunt & Bus voltage, continuous (Default)
 *
 * rcfg[10:7], bits[6:3] - Bus & Shunt ADC Register
 *     4'b0011 --> 12-bit resolution, 532uS conversion time (Default)
 *     See Table 4 - ADC settings from INA220 datasheet for more settings
 *
 * rcfg[12:11] - PGA Range
 *     2'b00 --> Gain /1, Range 40mV
 *     2'b01 --> Gain /2, Range 80mV
 *     2'b10 --> Gain /4, Range 160mV
 *     2'b11 --> Gain /8, Range 320mV (Default)
 *
 * rcfg[13] - Bus Voltage Range
 *     1'b0  --> 16V full scale
 *     1'b1  --> 32V full scale (Default)
 *
 * rcfg[14] - Unused
 *
 * rcfg[15] - Reset
 *     1'b0  --> Do nothing
 *     1'b1  --> Reset INA220 to default values, this bit self clears
 */
#define INA220_CFG_MODE_SHIFT   0
#define INA220_CFG_MODE_DEFAULT 0x7

#define INA220_CFG_SADC_SHIFT   3
#define INA220_CFG_SADC_DEFAULT 0x3

#define INA220_CFG_BADC_SHIFT   7
#define INA220_CFG_BADC_DEFAULT 0x3

#define INA220_CFG_PGA_SHIFT    11
#define INA220_CFG_PGA_DIV_1    0x0
#define INA220_CFG_PGA_DIV_2    0x1
#define INA220_CFG_PGA_DIV_4    0x2
#define INA220_CFG_PGA_DIV_8    0x3
#define INA220_CFG_PGA_DEFAULT  INA220_CFG_PGA_DIV_8

#define INA220_CFG_RANGE_SHIFT  13
#define INA220_CFG_RANGE_16V    0x0
#define INA220_CFG_RANGE_32V    0x1
#define INA220_CFG_RANGE_DEFAULT INA220_CFG_RANGE_32V
	uint16_t	rcfg;
	uint16_t	rcal;
};

static int ina220_read_reg(device_t dev, uint8_t reg, uint16_t *data);
static int ina220_write_reg(device_t dev, uint8_t reg, uint16_t data);

static int
ina220_configure(device_t dev)
{

	struct ina220_softc *sc;
	int error;
	uint16_t config = 0;

	sc = device_get_softc(dev);

	error = ina220_read_reg(dev, INA220_CFG_REG, &config);
	if (error != 0) {
		device_printf(dev, "configuration register read fail\n");
		return error;
	}

	if (sc->rcfg != config) {
		error = ina220_write_reg(dev, INA220_CFG_REG, sc->rcfg);
		if (error != 0) {
			device_printf(dev, "configuration register write fail\n");
			return error;
		}
	}

	error = ina220_read_reg(dev, INA220_CAL_REG, &config);
	if (error != 0) {
		device_printf(dev, "calibration register read fail\n");
		return error;
	}

	if (sc->rcal != config) {
		error = ina220_write_reg(dev, INA220_CAL_REG, sc->rcal);
		if (error != 0) {
			device_printf(dev, "calibration register write fail\n");
			return error;
		}
	}
	return 0;
}

static int
ina220_sysctl(SYSCTL_HANDLER_ARGS)
{

	int error;
	int value;
	uint16_t reg_value = 0;
	struct ina220_softc *sc;
	uint8_t reg_addr = (uint8_t)arg2;
	sc = (struct ina220_softc*)arg1;

	error = ina220_configure(sc->sc_dev);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: Configuration failed\n", __func__);
	}

	error = ina220_read_reg(sc->sc_dev, reg_addr, &reg_value);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: Register read failure\n", __func__);
	}

	switch (reg_addr) {

	case INA220_VBUS_REG:
		reg_value = reg_value >> INA220_VBUS_SHIFT_BITS;
		value = (uint32_t)reg_value * INA220_VBUS_LSB_MV;
		break;
	case INA220_PBUS_REG:
		value = (uint32_t)reg_value * INA220_PBUS_MULTIPLIER;
		break;
	case INA220_VSHUNT_REG:
		/* Sign bits are already be in the right spot */
		value = (int16_t)reg_value;
		/* 10uV LSB is independent of the front end PGA setting */
		value *= INA220_VSHUNT_LSB_UV;
		break;
	default:
		value = reg_value;
	}

	error = sysctl_handle_int(oidp, &value, sizeof(value), req);
	if (error != 0 || req->newptr == NULL) {
		return error;
	}

	/*
	 * The only writable registers are CONFIG and CALIBRATION. One of
	 * those two are being written if req->newptr is not null
	 */

	if (reg_addr == INA220_CFG_REG)
		sc->rcfg = (uint16_t)value;
	else if (reg_addr == INA220_CAL_REG)
		sc->rcal = (uint16_t)value;

	return ina220_configure(sc->sc_dev);
}

static int
ina220_probe(device_t dev)
{

	int rc;
#ifdef FDT
	if (!ofw_bus_is_compatible(dev, "ti,ina220"))
		return (ENXIO);
#endif
	rc = BUS_PROBE_GENERIC;
	device_set_desc(dev, "Texas Instruments Power Monitor");
	return rc;
}

static int
ina220_read_reg(device_t dev, uint8_t reg, uint16_t *data)
{

	int rc;
	uint8_t bytes[2];
	struct ina220_softc *sc;
	sc = device_get_softc(dev);
	struct iic_msg msgs[2] = {
	     { sc->sc_addr, IIC_M_WR, sizeof(reg), &reg },
	     { sc->sc_addr, IIC_M_RD, sizeof(bytes), bytes }
	};
	rc = iicbus_transfer(dev, msgs, 2);
	if (rc != 0)
		return rc;

	/* INA220 sends bytes MSB first */
	*data = ((uint16_t)bytes[0]) << 8 | bytes[1];
	return 0;
}

static int
ina220_write_reg(device_t dev, uint8_t reg, uint16_t data)
{

	struct ina220_softc *sc;
	uint8_t buffer[MAX_IIC_DATA_SIZE + 1];

	sc = device_get_softc(dev);
	struct iic_msg msgs[1] = {
	     { sc->sc_addr, IIC_M_WR, sizeof(buffer), buffer },
	};

	buffer[0] = reg;
	buffer[1] = (uint8_t)(data >> 8);
	buffer[2] = (uint8_t)(data);
	return (iicbus_transfer(dev, msgs, 1));
}

static int
ina220_attach(device_t dev)
{

	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree_node;
	struct sysctl_oid_list *tree;
	struct ina220_softc *sc;
#ifdef FDT
	phandle_t ofnode;
	pcell_t ofcell;
	
	if ((ofnode = ofw_bus_get_node(dev)) == -1)
		return (ENXIO);
#endif

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_addr = iicbus_get_addr(dev);
	device_printf(dev, "Attaching INA220 @ addr 0x%02X\n", sc->sc_addr);

	/*
	 * This register configuration is the power up default except the
	 * PGA setting. The PGA setting on the shunt voltage monitor is
	 * set to /1 which configures a range of +- 40mV. Battery operated
	 * or other low voltage drop out systems will very likely not be
	 * designed with a 320mV current shunt circuit for power efficiency
	 * reasons. This can always be overridden using the "cfg" sysctl or
	 * device tree cell value.
	 */
	sc->rcfg =
		(INA220_CFG_MODE_DEFAULT  << INA220_CFG_MODE_SHIFT) |
		(INA220_CFG_SADC_DEFAULT  << INA220_CFG_SADC_SHIFT) |
		(INA220_CFG_BADC_DEFAULT  << INA220_CFG_BADC_SHIFT) |
		(INA220_CFG_PGA_DIV_1     << INA220_CFG_PGA_SHIFT)  |
		(INA220_CFG_RANGE_DEFAULT << INA220_CFG_RANGE_SHIFT);

	/*
	 * The calibration register value is calculated with the following
	 * formula:
	 *
	 *   cal = truncate( 0.04096 / (current_lsb * r_shunt))
	 *
	 * where:
	 *
	 *   'r_shunt'
	 *
	 *   	A hardware component value given in Ohms. Use the value given
	 *   	by the board designers.
	 *
	 *   'current_lsb'
	 *
	 *   	A programmed value representing the least significant bit in
	 *   	the computed power register. This is almost an arbitrary
	 *   	assignment. The only constraint is that the value chosen most
	 *   	be >= (max_expected_current / 32768).
	 *
	 *   	It's common, but not required, use "1" with an assumed SI
	 *   	prefix such as deci, centi, milli, or micro as the current_lsb
	 *   	because it makes interpreting the current and power registers
	 *   	easier. For example, with a 1mA current LSB, the current
	 *   	register will read out directly in mA.
	 *
	 *   	The units chosen have an influence on how the power register
	 *   	is to be interpreted. Power is always indicated in the same
	 *   	base units as current. If the current register indicates mA,
	 *   	then the power register will indicate mW.
	 *
	 * Assuming the following:
	 *
	 * 	Shunt Resister      = 20mOhm
	 * 	Desired Current LSB = 1mA
	 *
	 * 	cal = 0.04906 / (0.001 * 0.020) = 2048
	 *
	 * Of course that example calibration value will make no sense unless
	 * the board in question just happens to have been designed with a
	 * 40mV @ 2A full scale shunt resistor. For that reason, the default
	 * calibration value of 0 is used. This value should really be set in
	 * device tree "cal" cell.
	 */

	sc->rcal = 0;

#ifdef FDT
	if (OF_getencprop(ofnode, "cfg", &ofcell, sizeof(ofcell)) > 0)
		sc->rcfg = (uint16_t)ofcell;
	if (OF_getencprop(ofnode, "cal", &ofcell, sizeof(ofcell)) > 0)
		sc->rcal = (uint16_t)ofcell;
#endif

	ctx = device_get_sysctl_ctx(dev);
	tree_node = device_get_sysctl_tree(dev);
	tree = SYSCTL_CHILDREN(tree_node);

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "vbus",
			CTLTYPE_INT | CTLFLAG_RD, sc, INA220_VBUS_REG,
			ina220_sysctl, "I", "Bus Voltage (mV)");

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "vshunt",
			CTLTYPE_INT | CTLFLAG_RD, sc, INA220_VSHUNT_REG,
			ina220_sysctl, "I", "Shunt Voltage (uV)");

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "pbus",
			CTLTYPE_INT | CTLFLAG_RD, sc, INA220_PBUS_REG,
			ina220_sysctl, "I", "Input Power");

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "ibus",
			CTLTYPE_INT | CTLFLAG_RD, sc, INA220_IBUS_REG,
			ina220_sysctl, "I", "Input Current");

	/*
	 * Configuration and calibration registers are settable via sysctl.
	 * However, when using FDT it's easier to rely on the values given
	 * in the device tree since these are very hardware specific
	 * settings.
	 */

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "cfg",
			CTLTYPE_INT | CTLFLAG_RW, sc, INA220_CFG_REG,
			ina220_sysctl, "I", "Config");

	SYSCTL_ADD_PROC(ctx, tree, OID_AUTO, "cal",
			CTLTYPE_INT | CTLFLAG_RW, sc, INA220_CAL_REG,
			ina220_sysctl, "I", "Calibration");

	return (0);
}

static int
ina220_detach(device_t dev)
{

	return (0);
}

static device_method_t ina220_methods[] = {
	DEVMETHOD(device_probe,		ina220_probe),
	DEVMETHOD(device_attach,	ina220_attach),
	DEVMETHOD(device_detach,	ina220_detach),
	DEVMETHOD_END
};

static driver_t ina220_driver = {
	"ina220",
	ina220_methods,
	sizeof(struct ina220_softc),
};
static devclass_t ina220_devclass;

DRIVER_MODULE_ORDERED(ina220, iicbus, ina220_driver, ina220_devclass, 0, 0, SI_ORDER_ANY);
MODULE_VERSION(ina220, 1);
MODULE_DEPEND(ina220, iicbus, 1, 1, 1);
