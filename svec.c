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

static struct class *svec_class;
static dev_t svec_devno;


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

static int __devinit svec_probe(struct device *pdev, unsigned int ndev)
{
	dev_dbg(pdev, "probing device %d\n", ndev);
	return -ENODEV;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	dev_dbg(pdev, "removing device %d\n", ndev);
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

static int __init svec_sysfs_device_create(void)
{
	int error = 0;

	svec_class = class_create(THIS_MODULE, "svec");

	if (IS_ERR(svec_class)) {
		printk(KERN_ERR PFX "Failed to create svec class\n");
		error = PTR_ERR(svec_class);
		goto out;
	}

	error = alloc_chrdev_region(&svec_devno, 0, SVEC_MAX_DEVICES, "svec");
	if (error) {
		printk(KERN_ERR PFX "Failed to allocate chrdev region\n");
		goto alloc_chrdev_region_failed;
	}

	if (!error)
		goto out;

alloc_chrdev_region_failed:
	class_destroy(svec_class);
out:
	return error;
}

static void svec_sysfs_device_remove(void)
{
	dev_t devno = MKDEV(MAJOR(svec_devno), 0);

	class_destroy(svec_class);
	unregister_chrdev_region(devno, SVEC_MAX_DEVICES);
}

static int __init svec_init(void)
{
	int error;

	pr_debug(PFX "initializing driver\n");
	error = vme_register_driver(&svec_driver, SVEC_MAX_DEVICES);
	if (error) {
		pr_err(PFX "could not register driver\n");
		goto out;
	}
	error = svec_sysfs_device_create();
	if (error) {
		pr_err(PFX "could not create sysfs device\n");
		goto sysfs_dev_create_failed;
	}
	return 0;
sysfs_dev_create_failed:
	vme_unregister_driver(&svec_driver);
out:
	return error;
}

static void __exit svec_exit(void)
{
	pr_debug(PFX "removing driver\n");
	svec_sysfs_device_remove();
	vme_unregister_driver(&svec_driver);
}

module_init(svec_init)
module_exit(svec_exit)

MODULE_AUTHOR("Juan David Gonzalez Cobas");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec carrier driver");
MODULE_VERSION(DRV_MODULE_VERSION);
