/*
* Copyright (C) 2012-2013 CERN (www.cern.ch)
* Author: Juan David Gonzalez Cobas <dcobas@cern.ch>
* Author: Luis Fernando Ruiz Gago <lfruiz@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
*/
#include <linux/slab.h>
#include <linux/fmc.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/fmc-sdb.h>
#include <linux/jhash.h>
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
	uint32_t fw_hash;
	int ret = 0;

	/* If no firmware filename is provided, load default */
	if (!gw)
		gw = svec_fw_name;

	if (!strlen(gw)) { /* use module parameters from the driver */
		int index;

		/*
		 * FIXME: this should come from a call to validate.
		 * if the driver uses FMC_PARAM_BUSID/FMC_PARAM_GATEWARE.
		 * Please check docs and spec-sw (wr-nic).
		 */
		index = 0;

		gw = drv->gw_val[index];
		if (!gw)
			return -ESRCH; /* the caller may accept this */
	}

	dev_info(fmc->hwdev, "reprogramming with %s\n", gw);
	ret = request_firmware(&fw, gw, dev);
	if (ret < 0) {
		dev_warn(dev, "request firmware \"%s\": error %i\n", gw, ret);
		return ret;
	}
	fmc_free_sdb_tree(fmc);
	
	/* Hash firmware bitstream */
	fw_hash = jhash(fw->data, fw->size,0);
	if (fw_hash == svec->fw_hash) {
		dev_info(dev, "card already programmed with \"%s\" [%x]\n", gw, fw_hash);
		goto out;
	}

	/* load the firmware */
	ret = svec_load_fpga(svec, fw->data, fw->size);
	if (ret <0) {
		dev_err(dev, "error %i programming firmware \"%s\"\n", ret, gw);
		goto out;
	}

	/* configure and activate function 0 */
	dev_info(fmc->hwdev, "svec-fmc: setup fa0\n");
	svec_setup_csr_fa0(svec->map[MAP_CR_CSR]->kernel_va, svec->vmebase,
				svec->vector, svec->level);

	/* Store firmware hash to avoid reprogram */
	svec->fw_hash = fw_hash;
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
	return svec_eeprom_read(fmc, pos, data, len);
}

static int svec_write_ee(struct fmc_device *fmc, int pos,
			 const void *data, int len)
{

	if (!(fmc->flags & FMC_DEVICE_HAS_GOLDEN))
		return -ENOTSUPP;
	return svec_eeprom_write(fmc, pos, data, len);
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

static int check_golden(struct fmc_device *fmc)
{
	struct svec_dev *svec = fmc->carrier_data;
	int ret;
	uint32_t magic, vendor, device;

	/* poor man's SDB */
	magic = fmc_readl(fmc, 0x00);
	if (magic != 0x5344422d) {
		dev_err(svec->dev, "Bad SDB magic: 0x%08x\n", magic);
		return -ENODEV;
	}
	if ( (ret = fmc_scan_sdb_tree(fmc, 0x0)) < 0)
		return -ENODEV;

	vendor = fmc_readl(fmc, 0x5c);
	if (vendor != 0x0000ce42) {
		dev_err(svec->dev, "unsexpected vendor in SDB\n");
		return -ENODEV;
	}
	device = fmc_readl(fmc, 0x60);
	if (device != 0x676f6c64) {
		dev_err(svec->dev, "unexpected device in SDB\n");
		return -ENODEV;
	}
	if (svec_show_sdb)
		fmc_show_sdb_tree(fmc);
	return 0;
}

int svec_fmc_prepare(struct svec_dev *svec, unsigned int fmc_slot)
{
	struct fmc_device *fmc;
	int ret = 0;

	if (fmc_slot >= SVEC_N_SLOTS)
		return -EINVAL;

	fmc = kzalloc(sizeof(*fmc), GFP_KERNEL);
	if (!fmc) {
		dev_err(svec->dev, "cannot allocate fmc slot %d\n",
			fmc_slot);
		return -ENOMEM;
	}

	fmc->version = FMC_VERSION;
	fmc->carrier_name = "SVEC";
	fmc->carrier_data = svec;
	fmc->owner = THIS_MODULE;

	fmc->fpga_base = svec->map[MAP_REG]->kernel_va;

	fmc->irq = 0; /*TO-DO*/
	fmc->op = &svec_fmc_operations;
	fmc->hwdev = svec->dev; /* for messages */

	fmc->slot_id = fmc_slot;
	fmc->device_id = (svec->slot << 6) | fmc_slot;
	fmc->eeprom_addr = 0x50 + 2 * fmc_slot;
	fmc->memlen = 0x100000;

	/* check golden integrity */
	/* FIXME: this uses fmc_scan_sdb_tree and de-allocation
	 * could be wrong at second reprogramming, as it is called
	 * n times, one per slot */
	ret = check_golden(fmc);
	if (ret) {
		dev_err(svec->dev, "Bad golden, error %d\n", ret);
		kfree(fmc);
		return ret;
	}

	ret = svec_i2c_init(fmc);
	if (ret) {
		dev_err(svec->dev, "Error %d on svec i2c init", ret);
		kfree(fmc);
		return ret;
	}

	svec->fmcs[fmc_slot] = fmc;
	dev_info(svec->dev, "ready to create fmc device_id 0x%x\n",
			fmc->device_id);

	return ret;
}

static uint8_t svec_void_eeprom[10];

int svec_fmc_create(struct svec_dev *svec)
{
	int i;
	int error = 0;

	/* fmc structures filling */
	for (i=0; i < svec->fmcs_n; i++) {
		error = svec_fmc_prepare(svec, i);
		if (error)
			goto failed;
	}

	/* fmc device creation */
	error = fmc_device_register_n(svec->fmcs, svec->fmcs_n);
	if (error) {
		dev_err(svec->dev, "Error registering fmc devices\n");
		goto failed;
	}
	/* FIXME: how do we retrieve the actual number of registered
	 * devices?
	 */
	dev_info(svec->dev, "fmc devices registered\n");

failed:
	/* FIXME: free fmc allocations. */
	return error;

}

void svec_fmc_destroy(struct svec_dev *svec)
{
	fmc_device_unregister_n(svec->fmcs, svec->fmcs_n);
	dev_info(svec->dev, "%d fmc devices unregistered\n",
		 svec->fmcs_n);
}
