/*
 * I2C access (on-board EEPROM)
 *
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2 as published by the Free Software Foundation or, at your
 * option, any later version.
 */

#include <linux/io.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fmc.h>
#include <linux/random.h>	/* FIXME: is this needed? */
#include "svec.h"
#include "golden_regs.h"

/* re-definitions for fields in golden core */
#define GLD_I2C_CORE_BASE	0x10000
#define GLD_I2CR_SCL_OUT	GLD_I2CR0_SCL_OUT	
#define GLD_I2CR_SDA_OUT	GLD_I2CR0_SDA_OUT	
#define GLD_I2CR_SCL_IN		GLD_I2CR0_SCL_IN	
#define GLD_I2CR_SDA_IN		GLD_I2CR0_SDA_IN	

static int svec_i2c_dump;
module_param_named(i2c_dump, svec_i2c_dump, int, 0444);

static uint32_t core_offset[] = {
	GLD_I2C_CORE_BASE + GLD_REG_I2CR0,
	GLD_I2C_CORE_BASE + GLD_REG_I2CR1
};

/* shifted versions of fmc_readl/writel to the i2c core */
static void golden_writel(struct fmc_device *fmc, uint32_t val, int offset)
{
	fmc_writel(fmc, val, core_offset[fmc->slot_id] + offset);
}

static uint32_t golden_readl(struct fmc_device *fmc, int offset)
{
	return fmc_readl(fmc, core_offset[fmc->slot_id] + offset);
}

static inline int mezzanine_present(struct fmc_device *fmc)
{
	uint32_t presence;

	presence = fmc_readl(fmc, GLD_I2C_CORE_BASE + GLD_REG_CSR);
	printk("presence: 0x%08x read from 0x%08x\n", presence, GLD_I2C_CORE_BASE + GLD_REG_CSR);
	presence = GLD_CSR_FMC_PRESENT_R(presence) & (1<<fmc->slot_id);
	printk("presence: 0x%08x\n", presence);
	return presence;
}

/* Stupid dumping tool */
static void dumpstruct(char *name, void *ptr, int size)
{
	int i;
	unsigned char *p = ptr;

	printk("%s: (size 0x%x)\n", name, size);
	for (i = 0; i < size; ) {
		printk("%02x", p[i]);
		i++;
		printk(i & 3 ? " " : i & 0xf ? "  " : "\n");
	}
	if (i & 0xf)
		printk("\n");
}

static void set_sda(struct fmc_device *fmc, int val)
{
	uint32_t reg;

	reg = golden_readl(fmc, 0) & ~GLD_I2CR_SDA_OUT;
	if (val)
		reg |= GLD_I2CR_SDA_OUT;
	golden_writel(fmc, reg, 0);
	udelay(30); /* FIXME: is this enough? */
}

static void set_scl(struct fmc_device *fmc, int val)
{
	uint32_t reg;

	reg = golden_readl(fmc, 0) & ~GLD_I2CR_SCL_OUT;
	if (val)
		reg |= GLD_I2CR_SCL_OUT;
	golden_writel(fmc, reg, 0);
	udelay(30);	/* FIXME: is this enough? */
}

static int get_sda(struct fmc_device *fmc)
{
	return golden_readl(fmc, 0) & GLD_I2CR_SDA_IN ? 1 : 0;
};

static void mi2c_start(struct fmc_device *fmc)
{
	set_sda(fmc, 0);
	set_scl(fmc, 0);
}

static void mi2c_stop(struct fmc_device *fmc)
{
	set_sda(fmc, 0);
	set_scl(fmc, 1);
	set_sda(fmc, 1);
}

int mi2c_put_byte(struct fmc_device *fmc, int data)
{
	int i;
	int ack;

	for (i = 0; i < 8; i++, data<<=1) {
		set_sda(fmc, data & 0x80);
		set_scl(fmc, 1);
		set_scl(fmc, 0);
	}

	set_sda(fmc, 1);
	set_scl(fmc, 1);

	ack = get_sda(fmc);

	set_scl(fmc, 0);
	set_sda(fmc, 0);

	return ack ? -EIO : 0; /* ack low == success */
}

int mi2c_get_byte(struct fmc_device *fmc, unsigned char *data, int sendack)
{
	int i;
	int indata = 0;

	/* assert: scl is low */
	set_scl(fmc, 0);
	set_sda(fmc, 1);
	for (i = 0; i < 8; i++) {
		set_scl(fmc, 1);
		indata <<= 1;
		if (get_sda(fmc))
			indata |= 0x01;
		set_scl(fmc, 0);
	}

	set_sda(fmc, (sendack ? 0 : 1));
	set_scl(fmc, 1);
	set_scl(fmc, 0);
	set_sda(fmc, 0);

	*data= indata;
	return 0;
}

void mi2c_init(struct fmc_device *fmc)
{
	set_scl(fmc, 1);
	set_sda(fmc, 1);
}

void mi2c_scan(struct fmc_device *fmc)
{
	int i;
	for(i = 0; i < 256; i += 2) {
		mi2c_start(fmc);
		if(!mi2c_put_byte(fmc, i))
			pr_info("%s: Found i2c device at 0x%x\n",
			       KBUILD_MODNAME, i >> 1);
		mi2c_stop(fmc);
	}
}

/* FIXME: this is very inefficient: read several bytes in a row instead */
int svec_eeprom_read(struct fmc_device *fmc, uint32_t offset,
		void *buf, size_t size)
{
	int i;
	uint8_t *buf8 = buf;
	unsigned char c;
	int i2c_addr = fmc->eeprom_addr;
	
	for(i = 0; i < size; i++) {
		mi2c_start(fmc);
		if(mi2c_put_byte(fmc, i2c_addr << 1) < 0) {
			mi2c_stop(fmc);
			return -EIO;
		}

		mi2c_put_byte(fmc, (offset >> 8) & 0xff);
		mi2c_put_byte(fmc, offset & 0xff);
		offset++;
		mi2c_stop(fmc);
		mi2c_start(fmc);
		mi2c_put_byte(fmc, (i2c_addr << 1) | 1);
		mi2c_get_byte(fmc, &c, 0);
		*buf8++ = c;
		mi2c_stop(fmc);
	}
	return size;
}

int svec_eeprom_write(struct fmc_device *fmc, uint32_t offset,
		 const void *buf, size_t size)
{
	int i, busy;
	const uint8_t *buf8 = buf;
	int i2c_addr = fmc->eeprom_addr;

	for(i = 0; i < size; i++) {
		mi2c_start(fmc);

		if (mi2c_put_byte(fmc, i2c_addr << 1) < 0) {
			mi2c_stop(fmc);
			return -1;
		}
		mi2c_put_byte(fmc, (offset >> 8) & 0xff);
		mi2c_put_byte(fmc, offset & 0xff);
		mi2c_put_byte(fmc, *buf8++);
		offset++;
		mi2c_stop(fmc);

		do { /* wait until the chip becomes ready */
			mi2c_start(fmc);
			busy = mi2c_put_byte(fmc, i2c_addr << 1);
			mi2c_stop(fmc);
		} while(busy);
	}
	return size;
}

int svec_i2c_init(struct fmc_device *fmc)
{
	void *buf;
	int i;

	mi2c_scan(fmc);
	if (!mezzanine_present(fmc)) {
		fmc->flags |= FMC_DEVICE_NO_MEZZANINE;
		return 0;
	}

	buf = kmalloc(SVEC_I2C_EEPROM_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	i = svec_eeprom_read(fmc, 0, buf, SVEC_I2C_EEPROM_SIZE);
	if (i != SVEC_I2C_EEPROM_SIZE) {
		dev_err(fmc->hwdev, "EEPROM read error: %i\n", i);
		kfree(buf);
		fmc->eeprom = NULL;
		fmc->eeprom_len = 0;
		return -EIO;
	} else {
		dev_info(fmc->hwdev, "Mezzanine %d, i2c 0x%x: EEPROM read ok\n",
			fmc->slot_id + 1, fmc->eeprom_addr);
	}
	fmc->eeprom = buf;
	fmc->eeprom_len = SVEC_I2C_EEPROM_SIZE;

	if (svec_i2c_dump)
		dumpstruct("eeprom", buf, SVEC_I2C_EEPROM_SIZE);

	return 0;
}

void svec_i2c_exit(struct fmc_device *fmc)
{
	kfree(fmc->eeprom);
	fmc->eeprom = NULL;
	fmc->eeprom_len = 0;
}

