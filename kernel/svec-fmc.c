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

static int svec_validate(struct fmc_device *fmc, struct fmc_driver *drv)
{

	return 0; /* everyhing is valid */
}

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
	int ret;

	if (!gw)
		gw = svec_fw_name;

	if (!strlen(gw)) { /* use module parameters from the driver */
		int index;

		index = svec_validate(fmc, drv);

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

out:
	release_firmware(fw);
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
	/*
	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_read(fmc, SVEC_I2C_EEPROM_ADDR, pos, data, len);
	*/
	return -ENOTSUPP;
}

static int svec_write_ee(struct fmc_device *fmc, int pos,
			 const void *data, int len)
{
	/*
	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_write(fmc, SVEC_I2C_EEPROM_ADDR, pos, data, len);
	*/
	return -ENOTSUPP;
}

static struct fmc_operations svec_fmc_operations = {
	/* no readl/writel because we have the base pointer */
	.readl =		svec_readl,
	.writel =		svec_writel,
	.validate =		svec_validate,
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

static int check_golden(struct fmc_device *fmc)
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

	return 0;
}


int svec_fmc_create(struct svec_dev *svec)
{
	struct fmc_device *fmc;
	int ret;

	fmc = kzalloc(sizeof(*fmc), GFP_KERNEL);
	if (!fmc)
		return -ENOMEM;

	fmc->version = FMC_VERSION;
	fmc->carrier_name = "SVEC";
	fmc->carrier_data = svec;
	fmc->base = svec->map[MAP_REG]->kernel_va;
	fmc->irq = 0; /*TO-DO*/
	fmc->op = &svec_fmc_operations;
	fmc->hwdev = svec->dev; /* for messages */
	svec->fmc = fmc;

	/* Check that the golden binary is actually correct */
	ret = check_golden(fmc);
	if (ret)
		goto out_free;

	ret = fmc_device_register(fmc);
	if (ret)
		goto out_irq;
	return ret;

out_irq:
out_free:
	svec->fmc = NULL;
	kfree(fmc);
	return ret;
}

void svec_fmc_destroy(struct svec_dev *svec)
{
	fmc_device_unregister(svec->fmc);
	svec->fmc = NULL;
}
