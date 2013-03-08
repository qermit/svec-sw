/*
* Copyright (C) 2012-2013 CERN (www.cern.ch)
* Author: Juan David Gonzalez Cobas <dcobas@cern.ch>
* Author: Luis Fernando Ruiz Gago <lfruiz@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
*/
#include <linux/device.h>
#include <linux/ctype.h>
#include "svec.h"

static ssize_t svec_show_bootloader_active(struct device *pdev,
						struct device_attribute *attr,
						char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	size_t ret;
	int active;

	active = svec_is_bootloader_active(card);
	if (active < 0)
		return active;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", active);

	return ret;
}

static ssize_t svec_store_bootloader_active(struct device *pdev,
						struct device_attribute *attr,
						const char *buf,
						size_t count)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	svec_bootloader_unlock(card);

	return count;
}

static ssize_t svec_show_firmware_name(struct device *pdev,
					struct device_attribute *attr,
					char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	size_t ret;
	int bl_active;

	bl_active = svec_is_bootloader_active(card);
	if (bl_active < 0)
		return bl_active;

	if (bl_active)
		ret = snprintf(buf, PAGE_SIZE, "%s (not active)\n",
				card->fw_name);
	else
		ret = snprintf(buf, PAGE_SIZE, "%s\n", card->fw_name);

	return ret;
}

static ssize_t svec_store_firmware_name(struct device *pdev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	int error;

	struct svec_dev *card = dev_get_drvdata(pdev);

	pr_debug("storing firmware name [%s] length %d\n", buf, count);
	error = svec_load_fpga_file(card, buf);

	if (!error)
		snprintf(card->fw_name, PAGE_SIZE, "%s", buf);

	return count;
}


static ssize_t svec_show_interrupt_vector(struct device *pdev,
						struct device_attribute *attr,
						char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", card->vector);
}

static ssize_t svec_show_interrupt_level(struct device *pdev,
						struct device_attribute *attr,
						char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "%d\n", card->level);
}


/********************** SVEC board attributes ***********************/
static DEVICE_ATTR(bootloader_active,
			S_IWUSR | S_IRUGO,
			svec_show_bootloader_active,
			svec_store_bootloader_active);

static DEVICE_ATTR(firmware_name,
			S_IWUSR | S_IRUGO,
			svec_show_firmware_name,
			svec_store_firmware_name);
static DEVICE_ATTR(interrupt_vector,
			S_IRUGO,
			svec_show_interrupt_vector,
			NULL);
static DEVICE_ATTR(interrupt_level,
			S_IRUGO,
			svec_show_interrupt_level,
			NULL);

static struct attribute *svec_attrs[] = {
	&dev_attr_bootloader_active.attr,
	&dev_attr_firmware_name.attr,
	&dev_attr_interrupt_vector.attr,
	&dev_attr_interrupt_level.attr,
	NULL,
};

static struct attribute_group svec_attr_group = {
	.attrs = svec_attrs,
};

/******************** sysfs file management ***************************/
int svec_create_sysfs_files (struct svec_dev *card)
{
	int error = 0;

	error = sysfs_create_group(&card->dev->kobj, &svec_attr_group);

	return error;
}

void svec_remove_sysfs_files (struct svec_dev *card)
{
	sysfs_remove_group(&card->dev->kobj, &svec_attr_group);
}
