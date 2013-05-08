/*
 * I2C access (on-board EEPROM)
 *
 */
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/time.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fmc.h>
#include "svec.h"
#include "hw/wrc_syscon_regs.h"

#include "oc_i2c_regs.h"

static int svec_i2c_dump;
module_param_named(i2c_dump, svec_i2c_dump, int, 0444);

static uint32_t core_offset[] = { 0x10000, 0x11000 };

/* shifted versions of fmc_readl/writel to the i2c core */
static void i2c_writel(struct fmc_device *fmc, uint32_t val, int offset)
{
	fmc_writel(fmc, val, core_offset[fmc->slot_id] + offset);
}

static uint32_t i2c_readl(struct fmc_device *fmc, int offset)
{
	return fmc_readl(fmc, core_offset[fmc->slot_id] + offset);
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

static void oc_i2c_init(struct fmc_device *fmc)
{
	const int prescaler = 200;

	i2c_writel(fmc, (prescaler >> 8) & 0xff, I2C_REG_PRER_HI);
	i2c_writel(fmc, prescaler & 0xff, I2C_REG_PRER_LO);
	i2c_writel(fmc, I2C_CTR_EN, I2C_REG_CTR);
}

static uint32_t oc_i2c_wait_busy(struct fmc_device *fmc)
{
	uint32_t sr;

	do {
		  sr = i2c_readl(fmc, I2C_REG_SR);
	} while(sr & I2C_SR_TIP);

	return sr;
}

static int oc_i2c_scan_bus(struct fmc_device *fmc)
{
	int i, ack;
	uint32_t sr;

	for (i = 0; i < 256; i += 2) {
		i2c_writel(fmc, i | 1, I2C_REG_TXR);
		i2c_writel(fmc, I2C_CR_STA | I2C_CR_WR, I2C_REG_CR);

		sr = oc_i2c_wait_busy(fmc);
		ack = !(sr & I2C_SR_RXACK);

		if (ack) {
			pr_info("Device found at address 0x%x\n", i >> 1);

			i2c_writel(fmc, 0,I2C_REG_TXR);
			i2c_writel(fmc,  I2C_CR_STO | I2C_CR_WR, I2C_REG_CR);
			oc_i2c_wait_busy(fmc);
			return 1;
		}
	}
	return 0;
}


static int oc_i2c_write(struct fmc_device *fmc, int i2c_addr, const uint8_t *buf, size_t size)
{
	uint32_t sr;

	pr_debug("%s: entering [ addr %x ]\n", __func__, i2c_addr);
	i2c_writel(fmc, i2c_addr << 1, I2C_REG_TXR);
	i2c_writel(fmc, I2C_CR_STA | I2C_CR_WR, I2C_REG_CR);

	sr = oc_i2c_wait_busy(fmc);
	if (sr & I2C_SR_RXACK)
		return -1;

	while (size--) {
		i2c_writel(fmc, *buf++, I2C_REG_TXR);
		i2c_writel(fmc, I2C_CR_WR | (size == 0 ? I2C_CR_STO : 0), I2C_REG_CR);
		sr = oc_i2c_wait_busy(fmc);
		if (sr & I2C_SR_RXACK)
			return -1;
	}

	return 0;
}

static int oc_i2c_read(struct fmc_device *fmc, int i2c_addr, uint8_t *buf, size_t size)
{
	uint32_t sr;

	pr_debug("%s: entering size %zd\n", __func__, size);
	i2c_writel(fmc, (i2c_addr << 1) | 1, I2C_REG_TXR);
	i2c_writel(fmc, I2C_CR_STA | I2C_CR_WR, I2C_REG_CR);

	sr = oc_i2c_wait_busy(fmc);
	if (sr & I2C_SR_RXACK)
		return -1;

	while (size--) {
		uint8_t r;

		i2c_writel(fmc, I2C_CR_RD, I2C_REG_CR);
		sr = oc_i2c_wait_busy(fmc);
		if (sr & I2C_SR_RXACK)
			return -1;

		r = i2c_readl(fmc, I2C_REG_RXR) & 0xff;
		*buf++ =  r;
	}

	i2c_writel(fmc, I2C_CR_STO | I2C_CR_RD, I2C_REG_CR);
	sr = oc_i2c_wait_busy(fmc);

	return 0;
}

int svec_eeprom_read(struct fmc_device *fmc, uint32_t offset,
		void *buf, size_t size)
{
	uint8_t txbuf[2];

	txbuf[0] = (offset >> 8) & 0xff;
	txbuf[1] = offset & 0xff;

	oc_i2c_write(fmc, fmc->eeprom_addr, txbuf, 2);
	oc_i2c_read(fmc, fmc->eeprom_addr, buf, size);

	return size;
}

int svec_eeprom_write(struct fmc_device *fmc, uint32_t offset,
		const void *buf, size_t size)
{
	uint8_t txbuf[2];

	txbuf[0] = (offset >> 8) & 0xff;
	txbuf[1] = offset & 0xff;

	oc_i2c_write(fmc, fmc->eeprom_addr, txbuf, 2);
	oc_i2c_write(fmc, fmc->eeprom_addr, buf, size);

	return size;
}


int svec_i2c_init(struct fmc_device *fmc, unsigned int slot)
{
	void *buf;
	int i, found;

	oc_i2c_init(fmc);
	found = oc_i2c_scan_bus(fmc);
	if (!found) {
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
			slot + 1, fmc->eeprom_addr);
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

