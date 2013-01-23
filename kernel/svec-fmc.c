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
#include <linux/fmc-sdb.h>
#include "svec.h"

static int svec_show_sdb;
module_param_named(show_sdb, svec_show_sdb, int, 0444);

/* The main role of this file is offering the fmc_operations for the svec */

static uint32_t svec_readl(struct fmc_device *fmc, int offset)
{
	uint32_t val = 0;

	val = ioread32be(fmc->fpga_base + offset);

	return val;
}

static void svec_writel(struct fmc_device *fmc, uint32_t val, int offset)
{
	iowrite32be(val, fmc->fpga_base + offset);
}

static int svec_reprogram(struct fmc_device *fmc, struct fmc_driver *drv,
			  char *gw)
{
	const struct firmware *fw;
	struct svec_dev *svec = fmc->carrier_data;
	struct device *dev = fmc->hwdev;
	int ret = 0;

	if (svec->already_reprogrammed) {
		dev_info(fmc->hwdev, "Already programmed\n");
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
	dev_info(fmc->hwdev, "svec-fmc: setup fa0\n");
	setup_csr_fa0(svec->map[MAP_CR_CSR]->kernel_va, svec->vmebase2,
				svec->vector, svec->level);

	/* Map A32 space */
	if (svec->map[MAP_REG] == NULL)
		map_window(svec, MAP_REG, VME_A32_USER_DATA_SCT,
				VME_D32, svec->vmebase2, 0x100000);

	svec->already_reprogrammed = 1;
out:
	release_firmware(fw);
	if (ret < 0)
		dev_err(dev, "svec reprogram failed while loading %s\n", gw);
	return ret;
}

static int svec_validate(struct fmc_device *fmc, struct fmc_driver *drv)
{
	return 0; /* everyhing is valid */
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
	return svec_eeprom_read(fmc, fmc->eeprom_addr, pos, data, len);

	return -ENOTSUPP;
}

static int svec_write_ee(struct fmc_device *fmc, int pos,
			 const void *data, int len)
{

	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_write(fmc, fmc->eeprom_addr, pos, data, len);

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
	.validate =		svec_validate,
};

int svec_fmc_create(struct svec_dev *svec, unsigned int n)
{
	struct fmc_device *fmc = svec->fmcs + n;
	int ret = 0;

	printk(KERN_ERR "%s enters\n", __func__);

	if (n<0 || n>1)
		return -EINVAL;

	fmc->version = FMC_VERSION;
	fmc->carrier_name = "SVEC";
	fmc->carrier_data = svec;
	fmc->owner = THIS_MODULE;

	fmc->fpga_base = svec->map[MAP_REG]->kernel_va;

	fmc->irq = 0; /*TO-DO*/
	fmc->op = &svec_fmc_operations;
	fmc->hwdev = svec->dev; /* for messages */

	fmc->slot_id = n;
	fmc->device_id = n + 1;
	fmc->eeprom_addr = 0x50 + 2 * n;
	fmc->memlen = 0x100000;

	ret = svec_i2c_init(fmc, n);
	if (ret) {
		dev_err(svec->dev, "Error %d on svec i2c init", ret);
		return ret;
	}

	printk(KERN_ERR "%s exits\n", __func__);

	return ret;
}
