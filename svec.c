/*
 * svec.c
 * Simple VmE Carrier driver
 *
 * Copyright (c) 2012 CERN
 * author: Luis Fernando Ruiz Gago
 * author: Juan David Gonzalez Cobas
 * Released under the GPL v2. (and only v2, not any later version)
 */

#include <linux/version.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <asm/uaccess.h>

#include <vmebus.h>

#define DRV_NAME		"svec"
#define PFX			DRV_NAME ": "
#define DRV_MODULE_VERSION	"0.1"

#define SVEC_MAX_DEVICES	32
#define SVEC_DEFAULT_IDX	{ [0 ... (SVEC_MAX_DEVICES-1)] = -1 }

char *svec_fw_name = "fmc/svec-init.bin";
module_param_named(fw_name, svec_fw_name, charp, 0444);

static long base[SVEC_MAX_DEVICES];
static unsigned int base_num;
static int vector[SVEC_MAX_DEVICES];
static unsigned int vector_num;
static int level[SVEC_MAX_DEVICES];
static unsigned int level_num;
static int index[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IDX;

module_param_array(base, long, &base_num, 0444);
MODULE_PARM_DESC(base, "Base address of the svec board");
module_param_array(vector, int, &vector_num, 0444);
MODULE_PARM_DESC(vector, "IRQ vector");
module_param_array(level, int, &level_num, 0444);
MODULE_PARM_DESC(level, "IRQ level");
module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for svec board");

struct svec_dev {
	struct device		*dev;
	struct resource		*area[3];	/* bar 0, 2, 4 */
	void __iomem		*remap[3];	/* ioremap of bar 0, 2, 4 */
	char			*submod_name;
	/* struct work_struct	work; */
	const struct firmware	*fw;
	struct list_head	list;
	unsigned long		irqcount;
	void			*sub_priv;
	struct fmc_device	*fmc;
	int			irq_count;	/* for mezzanine use too */
	struct completion	compl;
	struct gpio_chip	*gpio;
};

static int __devinit svec_match(struct device *devp, unsigned int ndev)
{
	if (ndev >= base_num)
		return 0;
	if (ndev >= vector_num || ndev >= level_num) {
		dev_warn(devp, "irq vector/level missing\n");
		return 0;
	}
	return 1;
}

int svec_load_fpga_file(struct svec_dev *svec, char *name)
{
	struct device *dev = svec->dev;
#if 0
	const struct firmware *fw;
	int err = 0;

	err = request_firmware(&fw, name, dev);
	if (err < 0) {
		dev_err(dev, "request firmware \"%s\": error %i\n", name, err);
		return err;
	}
	dev_info(dev, "got file \"%s\", %i (0x%x) bytes\n",
		 svec_fw_name, fw->size, fw->size);

	err = svec_load_fpga(svec, fw->data, fw->size);
	release_firmware(fw);
        return err;
#endif
	dev_info(dev, "%s: file %s\n", __func__, name);
	return 0;
}

static int __devinit svec_probe(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec;
	int ret = 0;

	dev_info(pdev, "probe for device %02d\n", ndev);

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (!svec)
		return -ENOMEM;
	svec->dev = pdev;

	/* Remap our 3 bars */
	dev_info(pdev, "mapping device %02d:%02d at address 0x%08lx\n",
		ndev, index[ndev], base[ndev]);

	if (ret)
		goto out_unmap;

	/* Load the golden FPGA binary to read the eeprom */
	ret = svec_load_fpga_file(svec, svec_fw_name);
	if (ret)
		goto out_unmap;

#if 0
	ret = svec_fmc_create(svec);
	if (ret)
		goto out_unmap;
#endif

	/* Done */
	dev_set_drvdata(pdev, svec);
	return 0;

out_unmap:
	return ret;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	dev_info(pdev, "removing device %d\n", ndev);
	return 0;
}

static struct vme_driver svec_driver = {
	.match		= svec_match,
	.probe		= svec_probe,
	.remove		= __devexit_p(svec_remove),
	.driver		= {
		.name	= DRV_NAME,
	},
};

static int __init svec_init(void)
{
	pr_debug(PFX "initializing driver\n");
	return vme_register_driver(&svec_driver, SVEC_MAX_DEVICES);
}

static void __exit svec_exit(void)
{
	pr_debug(PFX "removing driver\n");
	vme_unregister_driver(&svec_driver);
}

module_init(svec_init)
module_exit(svec_exit)

MODULE_AUTHOR("Juan David Gonzalez Cobas");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec carrier driver");
MODULE_VERSION(DRV_MODULE_VERSION);
