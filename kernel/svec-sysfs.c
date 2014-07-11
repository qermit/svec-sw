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
#include <linux/slab.h>

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

#define FW_CMD_RESET 0
#define FW_CMD_PROGRAM 1

static int svec_fw_cmd_reset (struct svec_dev * card)
{
	int err = 0;
	if (test_bit (SVEC_FLAG_FMCS_REGISTERED, &card->flags))
	{
		svec_fmc_destroy (card);
		svec_irq_exit (card);
	}

	if (!card->map[MAP_CR_CSR])
		err = svec_map_window (card, MAP_CR_CSR);

	if(err < 0)
		return err;

	svec_bootloader_unlock (card);

	if (!svec_is_bootloader_active (card))
		return -ENODEV;

	if (card->fw_buffer == NULL)
		card->fw_buffer = vmalloc (SVEC_MAX_GATEWARE_SIZE);

	card->fw_length = 0;
	card->fw_hash = 0xffffffff;
	return 0;
}

static int svec_fw_cmd_program (struct svec_dev * card)
{
	int err;
	if (!card->fw_buffer || !card->fw_length)
	    return -EINVAL;

	err = svec_load_fpga (card, card->fw_buffer, card->fw_length);
	
	vfree (card->fw_buffer);

	card->fw_buffer = NULL;
	card->fw_length = 0;
	if (err < 0)
		return err;

	svec_reconfigure (card);
	return 0;
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

	char *tmp_buf = strim((char *)buf);

	if (ksize(card->fw_name) < strlen(buf)+1) 
		card->fw_name = krealloc(card->fw_name, sizeof(char) * (strlen(tmp_buf)+1), GFP_KERNEL);

	if (! card->fw_name) {
		return -ENOMEM;		
	}

	strcpy(card->fw_name, buf);

	return count;
}

ATTR_STORE_CALLBACK(firmware_cmd)
{
	int cmd;
	int result = 0;

	struct svec_dev *card = dev_get_drvdata(pdev);

	if (sscanf(buf, "%i", &cmd) != 1)
		return -EINVAL;

	switch(cmd)
	{
	    case FW_CMD_RESET:
		result = svec_fw_cmd_reset (card);
		break;
	    case FW_CMD_PROGRAM:
		result = svec_fw_cmd_program (card);
		break;
	    default:
		return -EINVAL;
	}

	if (result < 0)
		return result;
	else
		return count;
}

ATTR_STORE_CALLBACK(firmware_blob)
{
	struct svec_dev *card = dev_get_drvdata(pdev);

	if (!card->fw_buffer)
	    return -EAGAIN;
	
	if (card->fw_length + count - 1 >= SVEC_MAX_GATEWARE_SIZE)
	    return -EINVAL;

	memcpy (card->fw_buffer + card->fw_length, buf, count);
	card->fw_length += count;

	return count;
    
}

ATTR_SHOW_CALLBACK(dummy_attr)
{
	return snprintf(buf, PAGE_SIZE, "0");
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

ATTR_SHOW_CALLBACK(vme_addr32)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	unsigned long addr = card->vme_raw_addr >> 2;
	return snprintf(buf, PAGE_SIZE, "0x%lx\n", addr);
}

ATTR_STORE_CALLBACK(vme_addr32)
{
	uint32_t addr;
	int error;
	char *tmp_buf;
	struct svec_dev *card = dev_get_drvdata(pdev);
	
	tmp_buf = strim((char *)buf);
	error = kstrtoul(buf, 0, (unsigned long *)  &addr);
	if (error != 0) 
		return error;

	addr = addr << 2;

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

static int __next_token(char **str, char *buf, int buf_length)
{
	char *p = *str, *tok;
	int len;

	while(isspace (*p))
		p++;

	if(*p == 0)
		return 0;
	tok = p;
	while(*p && !isspace(*p))
		p++;

	len = min(p - tok + 1, buf_length - 1);
	memcpy(buf, tok, len);
	buf[len - 1] = 0;

	*str = p;
	return 1;
}

ATTR_STORE_CALLBACK(vme_data)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	uint32_t data;
	uint32_t addr = card->vme_raw_addr;
	char *args = (char *) buf, token[16];
	
	if (!card->cfg_cur.configured)
		return -EAGAIN;

	while (__next_token (&args, token, sizeof(token)))
	{
		if (sscanf(token, "%i", &data) != 1)
			return -EINVAL;

		iowrite32be(data, card->map[MAP_REG]->kernel_va + addr);
		addr += 4;
	}

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

        char *tmp_buf =  strim((char *)buf);
        int cmd_type;

	error = kstrtoint(tmp_buf, 0, & cmd_type);
        if (error != 0) 
		return error;

	if (cmd_type == 0) {
		error = svec_fw_cmd_reset (card);
		/// @todo unconfigure
	} else if (cmd_type == 1) {

		if (!svec_validate_configuration(card->dev, &card->cfg_new))
			return -EINVAL;

		card->cfg_new.configured = 1;
		card->cfg_cur = card->cfg_new;


		error = svec_load_fpga_file(card, card->fw_name);
		if (error != 0)
			return error;


		error = svec_reconfigure(card);
	} else if (cmd_type == 1) {
		error = svec_fw_cmd_program(card);
	} else {
		return -EINVAL;
	}

	if (error)
		return error;
	return count;
}

ATTR_SHOW_CALLBACK(slot)
{
	struct svec_dev *card = dev_get_drvdata(pdev);
	return snprintf(buf, PAGE_SIZE, "%d\n", card->slot);
}

static DEVICE_ATTR(firmware_name,
		   S_IWUSR | S_IRUGO,
		   svec_show_firmware_name, svec_store_firmware_name);

static DEVICE_ATTR(firmware_cmd,
		   S_IWUSR | S_IRUGO,
		   svec_show_dummy_attr, svec_store_firmware_cmd);

static DEVICE_ATTR(firmware_blob,
		   S_IWUSR | S_IRUGO,
		   svec_show_dummy_attr, svec_store_firmware_blob);

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
  - write: 0 to reset card
  - write: 1 to commit the new VME configuration
  - write: 2 to load firmware from buffer
  */

static DEVICE_ATTR(configured,
		   S_IWUSR | S_IRUGO,
		   svec_show_configured, svec_store_configured);

/*
  Raw VME read/write access, for debugging purposes
*/
static DEVICE_ATTR(vme_addr32,
		   S_IWUSR | S_IRUGO, svec_show_vme_addr32, svec_store_vme_addr32);

static DEVICE_ATTR(vme_addr,
		   S_IWUSR | S_IRUGO, svec_show_vme_addr, svec_store_vme_addr);

static DEVICE_ATTR(vme_data,
		   S_IWUSR | S_IRUGO, svec_show_vme_data, svec_store_vme_data);

static struct attribute *svec_attrs[] = {
	&dev_attr_firmware_name.attr,
	&dev_attr_firmware_blob.attr,
	&dev_attr_firmware_cmd.attr,
	&dev_attr_interrupt_vector.attr,
	&dev_attr_interrupt_level.attr,
	&dev_attr_vme_base.attr,
	&dev_attr_vme_size.attr,
	&dev_attr_vme_am.attr,
	&dev_attr_use_vic.attr,
	&dev_attr_use_fmc.attr,
	&dev_attr_configured.attr,
	&dev_attr_vme_addr32.attr,
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

	return error;
}

void svec_remove_sysfs_files(struct svec_dev *card)
{
	sysfs_remove_group(&card->dev->kobj, &svec_attr_group);
}
