/*
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Juan David Gonzalez Cobas
 * Author: Luis Fernando Ruiz Gago
 *
 * Released according to the GNU GPL, version 2 or any later version.
 *
 * This work is part of the White Rabbit project, a research effort led
 * by CERN, the European Institute for Nuclear Research.
 */
#include <linux/slab.h>
#include <linux/fmc.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/gpio.h>
#include <linux/fmc-sdb.h>
#include "svec.h"

static int svec_show_sdb;
module_param_named(show_sdb, svec_show_sdb, int, 0444);

/* The main role of this file is offering the fmc_operations for the svec */

static uint32_t svec_readl(struct fmc_device *fmc, int offset)
{
	uint32_t val = 0;

	val = ioread32be(fmc->base + offset);

	/*printk(KERN_ERR "%s: [0x%p]: 0x%x\n", __func__, fmc->base + offset, val);*/

	return val;
}

static void svec_writel(struct fmc_device *fmc, uint32_t val, int offset)
{
	iowrite32be(val, fmc->base + offset);
}


static int svec_reprogram(struct fmc_device *fmc, struct fmc_driver *drv,
			  char *gw)
{
	const struct firmware *fw;
	struct svec_dev *svec = fmc->carrier_data;
	struct device *dev = fmc->hwdev;
	int ret = 0;

	if (svec->already_reprogrammed) {
		printk(KERN_ERR "already programmed\n");
		return ret;
	}

	if (!gw)
		gw = svec_fw_name;

	if (!strlen(gw)) { /* use module parameters from the driver */
		int index;

		index = 0; /* FIXME: check what this is */

		gw = drv->gw_val[index];
		if (!gw)
			return -ESRCH; /* the caller may accept this */
	}

	dev_info(fmc->hwdev, "reprogramming with %s\n", gw);
	ret = request_firmware(&fw, gw, dev);
	if (ret < 0) {
		dev_warn(dev, "request firmware \"%s\": error %i\n", gw, ret);
		goto out;
	}
	fmc_free_sdb_tree(fmc);
	fmc->flags &= ~(FMC_DEVICE_HAS_GOLDEN | FMC_DEVICE_HAS_CUSTOM);
	ret = svec_load_fpga(svec, fw->data, fw->size);
	if (ret <0) {
		dev_err(dev, "write firmware \"%s\": error %i\n", gw, ret);
		goto out;
	}
	if (gw == svec_fw_name)
		fmc->flags |= FMC_DEVICE_HAS_GOLDEN;
	else
		fmc->flags |= FMC_DEVICE_HAS_CUSTOM;

	/* configure and activate function 0 */
	printk(KERN_ERR "svec-fmc: %s setup_csr_fa0\n", __func__);
	setup_csr_fa0(svec->map[MAP_CR_CSR]->kernel_va, svec->vmebase2,
				svec->vector, svec->level);

	/* Map A32 space */
	if (svec->map[MAP_REG] == NULL)
		map_window(svec, MAP_REG, VME_A32_USER_DATA_SCT,
				VME_D32, svec->vmebase2, 0x100000);

	svec->already_reprogrammed = 1;
out:
	release_firmware(fw);
	printk(KERN_ERR "svec-fmc: %s ends\n", __func__);
	return ret;
}

static int svec_irq_request(struct fmc_device *fmc, irq_handler_t handler,
			    char *name, int flags)
{
	return 0;
}

static void svec_irq_ack(struct fmc_device *fmc)
{
}

static int svec_irq_free(struct fmc_device *fmc)
{
	return 0;
}

static int svec_gpio_config(struct fmc_device *fmc, struct fmc_gpio *gpio,
			    int ngpio)
{
	return 0;
}


static int svec_read_ee(struct fmc_device *fmc, int pos, void *data, int len)
{

	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_read(fmc, SVEC_I2C_EEPROM_ADDR, pos, data, len);

	return -ENOTSUPP;
}

static int svec_write_ee(struct fmc_device *fmc, int pos,
			 const void *data, int len)
{

	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_write(fmc, SVEC_I2C_EEPROM_ADDR, pos, data, len);

	return -ENOTSUPP;
}

static struct fmc_operations svec_fmc_operations = {
	.readl =		svec_readl,
	.writel =		svec_writel,
	.reprogram =		svec_reprogram,
	.irq_request =		svec_irq_request,
	.irq_ack =		svec_irq_ack,
	.irq_free =		svec_irq_free,
	.gpio_config =		svec_gpio_config,
	.read_ee =		svec_read_ee,
	.write_ee =		svec_write_ee,
};

/*
 * Finally, the real init and exit
 */

static int check_sdb(struct fmc_device *fmc)
{
	struct svec_dev *svec = fmc->carrier_data;
	int ret;
	uint32_t val;

	/* poor man's SDB */
	val = fmc_readl(fmc, 0x0);
	if (val != 0x5344422d) {
		dev_err(svec->dev, "Can't find SDB magic\n");
		return -ENODEV;
	}
	else
		dev_info(svec->dev, "SDB magic found\n");

	if ((ret = fmc_scan_sdb_tree(fmc, 0x0)) < 0)
		return -ENODEV;

	if (svec_show_sdb)
		fmc_show_sdb_tree(fmc);

	/* FIXME: For now, hardcoded mezzanine offsets for fd*/
	/* It should be read from SBD */
	svec->mezzanine_offset[0] = 0x10000;
	svec->mezzanine_offset[1] = 0x20000;

	return 0;
}


int svec_fmc_create(struct svec_dev *svec, unsigned int n)
{
	struct fmc_device *fmc;
	int ret;

	if (n<0 || n>1)
		return -EINVAL;

	fmc = kzalloc(sizeof(*fmc), GFP_KERNEL);
	if (!fmc)
		return -ENOMEM;

	fmc->version = FMC_VERSION;
	fmc->carrier_name = "SVEC";
	fmc->carrier_data = svec;
	fmc->irq = 0; /*TO-DO*/
	fmc->op = &svec_fmc_operations;
	fmc->base = svec->map[MAP_REG]->kernel_va;
	fmc->hwdev = svec->dev; /* for messages */
	svec->fmc[n] = fmc;

	/* Check that the golden binary is actually correct */
	if (!svec->already_reprogrammed) {
		ret = check_sdb(fmc);
		if (ret)
			goto out_free;
	}
	else
		printk(KERN_ERR "SDB already checked\n");

	fmc->base += svec->mezzanine_offset[n];

	ret = fmc_device_register(fmc);
	if (ret)
		goto out_irq;

	return ret;

out_irq:
out_free:
	svec->fmc[n] = NULL;
	kfree(fmc);
	return ret;
}

void svec_fmc_destroy(struct svec_dev *svec, unsigned int n)
{
	fmc_device_unregister(svec->fmc[n]);
	svec->fmc[n] = NULL;
}
