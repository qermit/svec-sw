/*
 * Copyright (C) 2010-2012 CERN (www.cern.ch)
 * Author: Juan David Gonzalez Cobas
 * Author: Luis Fernando Ruiz Gago
 *
 * Released according to the GNU GPL, version 2 or any later version.
 *
 * This work is part of the White Rabbit project, a research effort led
 * by CERN, the European Institute for Nuclear Research.
 */
#include <linux/device.h>
#include <linux/ctype.h>
#include "svec.h"

static ssize_t
svec_show_bootloader_status(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	size_t ret;
	int locked;

	if (!card->ops.bootloader_check)
		return -EINVAL;
	
	locked = card->ops.bootloader_check(card);
	if (locked < 0) 
		return -EINVAL;
	
	if (locked == 0)
		ret = snprintf(buf, PAGE_SIZE, "svec.%d: bootloader active (unlocked)\n", card->lun);
	else
		ret = snprintf(buf, PAGE_SIZE, "svec.%d: bootloader not active (locked)\n", card->lun);

	return ret;
}

static ssize_t
svec_unlock_bootloader(struct device *pdev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	if (!card->ops.bootloader_unlock)
		return -EINVAL;

	if (card->ops.bootloader_unlock(card) < 0)
		return -EINVAL;

	return count;
}

static DEVICE_ATTR(unlock_bootloader, S_IWUSR | S_IRUGO, svec_show_bootloader_status, svec_unlock_bootloader);

static struct attribute *svec_attrs[] = {
	&dev_attr_unlock_bootloader.attr,
	NULL,
};

static struct attribute_group svec_attr_group = {
	.attrs = svec_attrs,
};

int svec_create_sysfs_files (struct svec_dev *card)
{
	int error = 0;

	printk(KERN_ERR "svec: %s\n", __func__);

	error = sysfs_create_group(&card->dev->kobj, &svec_attr_group);

	return error;
}

void svec_remove_sysfs_files (struct svec_dev *card)
{
	sysfs_remove_group(&card->dev->kobj, &svec_attr_group);
}
