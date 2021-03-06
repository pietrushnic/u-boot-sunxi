/*
 * (C) Copyright 2014 Freescale Semiconductor, Inc.
 * Author: Nitin Garg <nitin.garg@freescale.com>
 *             Ye Li <Ye.Li@freescale.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <config.h>
#include <common.h>
#include <div64.h>
#include <fuse.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <thermal.h>
#include <imx_thermal.h>

/* board will busyloop until this many degrees C below CPU max temperature */
#define TEMPERATURE_HOT_DELTA   5 /* CPU maxT - 5C */
#define FACTOR0			10000000
#define FACTOR1			15976
#define FACTOR2			4297157
#define MEASURE_FREQ		327

#define TEMPSENSE0_TEMP_CNT_SHIFT	8
#define TEMPSENSE0_TEMP_CNT_MASK	(0xfff << TEMPSENSE0_TEMP_CNT_SHIFT)
#define TEMPSENSE0_FINISHED		(1 << 2)
#define TEMPSENSE0_MEASURE_TEMP		(1 << 1)
#define TEMPSENSE0_POWER_DOWN		(1 << 0)
#define MISC0_REFTOP_SELBIASOFF		(1 << 3)
#define TEMPSENSE1_MEASURE_FREQ		0xffff

struct thermal_data {
	unsigned int fuse;
	int critical;
	int minc;
	int maxc;
};

static int read_cpu_temperature(struct udevice *dev)
{
	int temperature;
	unsigned int reg, n_meas;
	const struct imx_thermal_plat *pdata = dev_get_platdata(dev);
	struct anatop_regs *anatop = (struct anatop_regs *)pdata->regs;
	struct thermal_data *priv = dev_get_priv(dev);
	u32 fuse = priv->fuse;
	int t1, n1;
	u32 c1, c2;
	u64 temp64;

	/*
	 * Sensor data layout:
	 *   [31:20] - sensor value @ 25C
	 * We use universal formula now and only need sensor value @ 25C
	 * slope = 0.4297157 - (0.0015976 * 25C fuse)
	 */
	n1 = fuse >> 20;
	t1 = 25; /* t1 always 25C */

	/*
	 * Derived from linear interpolation:
	 * slope = 0.4297157 - (0.0015976 * 25C fuse)
	 * slope = (FACTOR2 - FACTOR1 * n1) / FACTOR0
	 * (Nmeas - n1) / (Tmeas - t1) = slope
	 * We want to reduce this down to the minimum computation necessary
	 * for each temperature read.  Also, we want Tmeas in millicelsius
	 * and we don't want to lose precision from integer division. So...
	 * Tmeas = (Nmeas - n1) / slope + t1
	 * milli_Tmeas = 1000 * (Nmeas - n1) / slope + 1000 * t1
	 * milli_Tmeas = -1000 * (n1 - Nmeas) / slope + 1000 * t1
	 * Let constant c1 = (-1000 / slope)
	 * milli_Tmeas = (n1 - Nmeas) * c1 + 1000 * t1
	 * Let constant c2 = n1 *c1 + 1000 * t1
	 * milli_Tmeas = c2 - Nmeas * c1
	 */
	temp64 = FACTOR0;
	temp64 *= 1000;
	do_div(temp64, FACTOR1 * n1 - FACTOR2);
	c1 = temp64;
	c2 = n1 * c1 + 1000 * t1;

	/*
	 * now we only use single measure, every time we read
	 * the temperature, we will power on/down anadig thermal
	 * module
	 */
	writel(TEMPSENSE0_POWER_DOWN, &anatop->tempsense0_clr);
	writel(MISC0_REFTOP_SELBIASOFF, &anatop->ana_misc0_set);

	/* setup measure freq */
	reg = readl(&anatop->tempsense1);
	reg &= ~TEMPSENSE1_MEASURE_FREQ;
	reg |= MEASURE_FREQ;
	writel(reg, &anatop->tempsense1);

	/* start the measurement process */
	writel(TEMPSENSE0_MEASURE_TEMP, &anatop->tempsense0_clr);
	writel(TEMPSENSE0_FINISHED, &anatop->tempsense0_clr);
	writel(TEMPSENSE0_MEASURE_TEMP, &anatop->tempsense0_set);

	/* make sure that the latest temp is valid */
	while ((readl(&anatop->tempsense0) &
		TEMPSENSE0_FINISHED) == 0)
		udelay(10000);

	/* read temperature count */
	reg = readl(&anatop->tempsense0);
	n_meas = (reg & TEMPSENSE0_TEMP_CNT_MASK)
		>> TEMPSENSE0_TEMP_CNT_SHIFT;
	writel(TEMPSENSE0_FINISHED, &anatop->tempsense0_clr);

	/* milli_Tmeas = c2 - Nmeas * c1 */
	temperature = (long)(c2 - n_meas * c1)/1000;

	/* power down anatop thermal sensor */
	writel(TEMPSENSE0_POWER_DOWN, &anatop->tempsense0_set);
	writel(MISC0_REFTOP_SELBIASOFF, &anatop->ana_misc0_clr);

	return temperature;
}

int imx_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct thermal_data *priv = dev_get_priv(dev);
	int cpu_tmp = 0;

	cpu_tmp = read_cpu_temperature(dev);
	while (cpu_tmp >= priv->critical) {
		printf("CPU Temperature (%dC) too close to max (%dC)",
		       cpu_tmp, priv->maxc);
		puts(" waiting...\n");
		udelay(5000000);
		cpu_tmp = read_cpu_temperature(dev);
	}

	*temp = cpu_tmp;

	return 0;
}

static const struct dm_thermal_ops imx_thermal_ops = {
	.get_temp	= imx_thermal_get_temp,
};

static int imx_thermal_probe(struct udevice *dev)
{
	unsigned int fuse = ~0;

	const struct imx_thermal_plat *pdata = dev_get_platdata(dev);
	struct thermal_data *priv = dev_get_priv(dev);

	/* Read Temperature calibration data fuse */
	fuse_read(pdata->fuse_bank, pdata->fuse_word, &fuse);

	/* Check for valid fuse */
	if (fuse == 0 || fuse == ~0) {
		printf("CPU:   Thermal invalid data, fuse: 0x%x\n", fuse);
		return -EPERM;
	}

	/* set critical cooling temp */
	get_cpu_temp_grade(&priv->minc, &priv->maxc);
	priv->critical = priv->maxc - TEMPERATURE_HOT_DELTA;
	priv->fuse = fuse;

	enable_thermal_clk();

	return 0;
}

U_BOOT_DRIVER(imx_thermal) = {
	.name	= "imx_thermal",
	.id	= UCLASS_THERMAL,
	.ops	= &imx_thermal_ops,
	.probe	= imx_thermal_probe,
	.priv_auto_alloc_size = sizeof(struct thermal_data),
	.flags  = DM_FLAG_PRE_RELOC,
};
