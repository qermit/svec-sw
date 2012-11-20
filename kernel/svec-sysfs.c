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
svec_show_bootloader_active(struct device *pdev, struct device_attribute *attr, char *buf)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	size_t ret;
	int active;

	active = svec_bootloader_check(card);
	if (active < 0) 
		return -EINVAL;
	
	ret = snprintf(buf, PAGE_SIZE, "%d\n", active);

	return ret;
}

static ssize_t
svec_store_bootloader_active(struct device *pdev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	svec_bootloader_unlock(card);

	return count;
}

static DEVICE_ATTR(bootloader_active, 
			S_IWUSR | S_IRUGO, 
			svec_show_bootloader_active, 
			svec_store_bootloader_active);

static struct attribute *svec_attrs[] = {
	&dev_attr_bootloader_active.attr,
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
