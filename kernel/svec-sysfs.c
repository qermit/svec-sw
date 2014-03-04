/*
* Copyright (C) 2012-2013 CERN (www.cern.ch)
* Author: Juan David Gonzalez Cobas <dcobas@cern.ch>
* Author: Luis Fernando Ruiz Gago <lfruiz@cern.ch>
* Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
*/
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/stat.h>
#include "svec.h"

#define ATTR_SHOW_CALLBACK(name) \
	static ssize_t svec_show_##name(struct device *pdev, \
	struct device_attribute *attr, \
	char *buf)

#define ATTR_STORE_CALLBACK(name) \
	static ssize_t svec_store_##name(struct device *pdev, \
					struct device_attribute *attr, \
					const char *buf, \
					size_t count)

ATTR_SHOW_CALLBACK(bootloader_active)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	size_t ret;
	int active;

	if (test_bit(SVEC_FLAG_FMCS_REGISTERED, &card->flags))
		active = 0;
	else {
		active = svec_is_bootloader_active(card);
		if (active < 0)
			return active;
	}

	ret = snprintf(buf, PAGE_SIZE, "%d\n", active);

	return ret;
}

ATTR_STORE_CALLBACK(bootloader_active)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	if (test_bit(SVEC_FLAG_FMCS_REGISTERED, &card->flags))
	{
		svec_fmc_destroy(card);
		svec_irq_exit(card);
	}

	svec_bootloader_unlock(card);
	return count;
}

ATTR_SHOW_CALLBACK(firmware_name)
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

ATTR_STORE_CALLBACK(firmware_name)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	int error;

	pr_debug("storing firmware name [%s] length %zd\n", buf, count);
	error = svec_load_fpga_file(card, buf);

	if (!error)
		snprintf(card->fw_name, PAGE_SIZE, "%s", buf);

	return count;
}

ATTR_SHOW_CALLBACK(interrupt_vector)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
			card->cfg_cur.interrupt_vector);
}

ATTR_SHOW_CALLBACK(interrupt_level)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d\n", card->cfg_cur.interrupt_level);
}

ATTR_STORE_CALLBACK(interrupt_level)
{
	int lvl;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &lvl) != 1)
		return -EINVAL;
	if (lvl < 0 || lvl > 7)
		return -EINVAL;

	card->cfg_new.interrupt_level = lvl;
	return count;
}

ATTR_STORE_CALLBACK(interrupt_vector)
{
	int vec;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &vec) != 1)
		return -EINVAL;
	if (vec < 0 || vec > 255)
		return -EINVAL;

	card->cfg_new.interrupt_vector = vec;
	return count;
}

ATTR_SHOW_CALLBACK(vme_am)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "0x%x\n", card->cfg_cur.vme_am);
}

ATTR_STORE_CALLBACK(vme_am)
{
	int am;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &am) != 1)
		return -EINVAL;

	card->cfg_new.vme_am = am;
	return count;
}

ATTR_SHOW_CALLBACK(vme_base)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "0x%x\n", card->cfg_cur.vme_base);
}

ATTR_STORE_CALLBACK(vme_base)
{
	uint32_t base;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &base) != 1)
		return -EINVAL;

	card->cfg_new.vme_base = base;	/* will be verified on commit */
	return count;
}

ATTR_SHOW_CALLBACK(vme_size)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "0x%x\n", card->cfg_cur.vme_size);
}

ATTR_STORE_CALLBACK(vme_size)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	uint32_t size;

	if (sscanf(buf, "%i", &size) != 1)
		return -EINVAL;

	card->cfg_new.vme_size = size;	/* will be verified on commit */
	return count;

}

ATTR_SHOW_CALLBACK(vme_addr)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "0x%x\n", card->vme_raw_addr);
}

ATTR_STORE_CALLBACK(vme_addr)
{
	uint32_t addr;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &addr) != 1)
		return -EINVAL;

	if (addr >= card->cfg_cur.vme_base + card->cfg_cur.vme_size)
		return -EINVAL;

	if (addr & 3)
		return -EINVAL;

	card->vme_raw_addr = addr;

	return count;
}

ATTR_SHOW_CALLBACK(vme_data)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	uint32_t data;

	if (unlikely(!card->map[MAP_REG]))
		return -EAGAIN;

	if (!card->cfg_cur.configured)
		return -EAGAIN;

	data = ioread32be(card->map[MAP_REG]->kernel_va + card->vme_raw_addr);

	return snprintf(buf, PAGE_SIZE, "0x%x\n", data);
}

ATTR_STORE_CALLBACK(vme_data)
{
	uint32_t data;
	struct svec_dev *card = dev_get_drvdata(pdev);

	if (!card->cfg_cur.configured)
		return -EAGAIN;

	if (sscanf(buf, "%i", &data) != 1)
		return -EINVAL;

	iowrite32be(data, card->map[MAP_REG]->kernel_va + card->vme_raw_addr);

	return count;
}

ATTR_SHOW_CALLBACK(use_vic)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d", card->cfg_cur.use_vic);
}

ATTR_STORE_CALLBACK(use_vic)
{
	int enabled;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &enabled) != 1)
		return -EINVAL;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	card->cfg_new.use_vic = enabled;
	return count;

}

ATTR_SHOW_CALLBACK(use_fmc)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d\n", card->cfg_cur.use_fmc);
}

ATTR_STORE_CALLBACK(use_fmc)
{
	int enabled;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &enabled) != 1)
		return -EINVAL;

	if (enabled != 0 && enabled != 1)
		return -EINVAL;

	card->cfg_new.use_fmc = enabled;
	return count;
}

ATTR_SHOW_CALLBACK(configured)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d\n", card->cfg_cur.configured);
}

ATTR_STORE_CALLBACK(configured)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	int error;

	if (!svec_validate_configuration(card->dev, &card->cfg_new))
		return -EINVAL;

	card->cfg_new.configured = 1;
	card->cfg_cur = card->cfg_new;

	error = svec_reconfigure(card);

	if (error)
		return error;
	return count;
}

ATTR_SHOW_CALLBACK(slot)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d\n", card->slot);
}

/********************** SVEC board attributes ***********************/

/* bootloader activation:
  read: 1 if bootloader mode is active, 0 if not
  write: 1 to enable bootloader (and deregister attached fmcs)
         0 to disable bootloader (and register attached fmcs if the card is configured) 
*/

static DEVICE_ATTR(bootloader_active,
		   S_IWUSR | S_IRUGO,
		   svec_show_bootloader_active, svec_store_bootloader_active);

static DEVICE_ATTR(firmware_name,
		   S_IWUSR | S_IRUGO,
		   svec_show_firmware_name, svec_store_firmware_name);

/* Helper attribute to find the physical slot for a given VME LUN. Used by
  the userspace tools. */
static DEVICE_ATTR(slot, S_IRUGO, svec_show_slot, NULL);

/* Standard VME configuration attributes. Committed in atomic way by writing 1 to
  1 to 'configured' attribute. */
static DEVICE_ATTR(interrupt_vector,
		   S_IWUSR | S_IRUGO,
		   svec_show_interrupt_vector, svec_store_interrupt_vector);

static DEVICE_ATTR(interrupt_level,
		   S_IWUSR | S_IRUGO,
		   svec_show_interrupt_level, svec_store_interrupt_level);

static DEVICE_ATTR(vme_base,
		   S_IWUSR | S_IRUGO, svec_show_vme_base, svec_store_vme_base);

static DEVICE_ATTR(vme_size,
		   S_IWUSR | S_IRUGO, svec_show_vme_size, svec_store_vme_size);

static DEVICE_ATTR(vme_am,
		   S_IWUSR | S_IRUGO, svec_show_vme_am, svec_store_vme_am);

/*
  Enables support for the Vectored Interrupt Controller (VIC). 
  By default, the VIC is enabled and enumerated through SDB the first
  time the FMC driver requests an interrupt. If no VIC is found, the driver
  falls back to shared IRQ mode. This attribute is only to be used in exceptional
  situations, where even attempting to look for the VIC may screw the card. */

static DEVICE_ATTR(use_vic,
		   S_IWUSR | S_IRUGO, svec_show_use_vic, svec_store_use_vic);

/*
  Enables registration of the FMCs after checking their FRU information.
  Enabled by default, you may want to disable it for debugging purposes (programming
  EEPROMs and/or raw VME access)
  */

static DEVICE_ATTR(use_fmc,
		   S_IWUSR | S_IRUGO, svec_show_use_fmc, svec_store_use_fmc);

/*
  Configuration status/commit attribute:
  - read: 1 if the VME interface is correctly configured, 0 otherwise
  - write: 1 to commit the new VME configuration
  */

static DEVICE_ATTR(configured,
		   S_IWUSR | S_IRUGO,
		   svec_show_configured, svec_store_configured);

/*
  Raw VME read/write access, for debugging purposes
*/
static DEVICE_ATTR(vme_addr,
		   S_IWUSR | S_IRUGO, svec_show_vme_addr, svec_store_vme_addr);

static DEVICE_ATTR(vme_data,
		   S_IWUSR | S_IRUGO, svec_show_vme_data, svec_store_vme_data);

static struct attribute *svec_attrs[] = {
	&dev_attr_bootloader_active.attr,
	&dev_attr_firmware_name.attr,
	&dev_attr_interrupt_vector.attr,
	&dev_attr_interrupt_level.attr,
	&dev_attr_vme_base.attr,
	&dev_attr_vme_size.attr,
	&dev_attr_vme_am.attr,
	&dev_attr_use_vic.attr,
	&dev_attr_use_fmc.attr,
	&dev_attr_configured.attr,
	&dev_attr_vme_addr.attr,
	&dev_attr_vme_data.attr,
	&dev_attr_slot.attr,
	NULL,
};

static struct attribute_group svec_attr_group = {
	.attrs = svec_attrs,
};

/******************** sysfs file management ***************************/
int svec_create_sysfs_files(struct svec_dev *card)
{
	int error = 0;

	error = sysfs_create_group(&card->dev->kobj, &svec_attr_group);

	if (error)
		return error;

//      sysfs_create_bin_file(&card->dev->kobj, &svec_firmware_attr);

	return error;
}

void svec_remove_sysfs_files(struct svec_dev *card)
{
	sysfs_remove_group(&card->dev->kobj, &svec_attr_group);
}
