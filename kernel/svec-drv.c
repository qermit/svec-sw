/*
* Copyright (C) 2012-2013 CERN (www.cern.ch)
* Author: Juan David Gonzalez Cobas <dcobas@cern.ch>
* Author: Luis Fernando Ruiz Gago <lfruiz@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include "svec.h"
#include "xloader_regs.h"

char *svec_fw_name = "fmc/svec_golden.bin";

/* Module parameters */
static int  slot[SVEC_MAX_DEVICES];
static unsigned int slot_num;
static unsigned int vmebase[SVEC_MAX_DEVICES];
static unsigned int vmebase_num;
static char *fw_name[SVEC_MAX_DEVICES];
static unsigned int fw_name_num;
static int  vector[SVEC_MAX_DEVICES];
static unsigned int vector_num;
static int lun[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IDX;
static unsigned int lun_num;


module_param_array(slot, int, &slot_num, S_IRUGO);
MODULE_PARM_DESC(slot, "Slot where SVEC card is installed");
module_param_array(vmebase, uint, &vmebase_num, S_IRUGO);
MODULE_PARM_DESC(vmebase, "VME Base address of the SVEC card registers");
module_param_array_named(fw_name, fw_name , charp, &fw_name_num, S_IRUGO);
MODULE_PARM_DESC(fw_name, "firmware file");
module_param_array(vector, int, &vector_num, S_IRUGO);
MODULE_PARM_DESC(vector, "IRQ vector");
module_param_array(lun, int, &lun_num, S_IRUGO);
MODULE_PARM_DESC(lun, "Index value for SVEC card");

int svec_map_window(struct svec_dev *svec, enum svec_map_win map_type)
{
	struct device *dev = svec->dev;
	enum vme_address_modifier am = VME_CR_CSR;
	enum vme_data_width dw = VME_D32;
	unsigned long base = svec->slot * 0x80000;
	unsigned int size = 0x80000;
	int rval;

	if (svec->map[map_type] != NULL) {
		dev_err(dev, "Window %d already mapped\n", (int)map_type);
		return -EPERM;
	}

	/* Default values are for MAP_CR_CSR */
	/* For register map, we need to set them to: */
	if (map_type == MAP_REG) {
		am = VME_A32_USER_DATA_SCT;
		dw = VME_D32;
		base = svec->vmebase;
		size = 0x100000;
	}

	svec->map[map_type] = kzalloc(sizeof(struct vme_mapping), GFP_KERNEL);
	if (!svec->map[map_type]) {
		dev_err(dev, "Cannot allocate memory for vme_mapping struct\n");
		return -ENOMEM;
	}

	/* Window mapping*/
	svec->map[map_type]->am =		am; /* 0x2f */
	svec->map[map_type]->data_width =	dw;
	svec->map[map_type]->vme_addru =	0;
	svec->map[map_type]->vme_addrl =	base;
	svec->map[map_type]->sizeu =		0;
	svec->map[map_type]->sizel =		size;

	if (( rval = vme_find_mapping(svec->map[map_type], 1)) != 0) {
		dev_err(dev, "Failed to map window %d: (%d)\n",
				(int)map_type, rval);
		kfree(svec->map[map_type]);
		svec->map[map_type] = NULL;
		return -EINVAL;
	}

	dev_info(dev, "%s mapping successful at 0x%p\n",
			map_type == MAP_REG ? "register" : "CR/CSR",
			svec->map[map_type]->kernel_va);

	return 0;
}

int svec_unmap_window(struct svec_dev *svec, enum svec_map_win map_type)
{
	struct device *dev = svec->dev;

	if (svec->map[map_type] == NULL) {
		dev_err(dev, "Window %d not mapped. Cannot unmap\n",
				(int)map_type);
		return -EINVAL;
	}
	if (vme_release_mapping(svec->map[map_type], 1)) {
		dev_err(dev, "Unmap for window %d failed\n", (int)map_type);
		return -EINVAL;
	}
	dev_info(dev, "Window %d unmaped\n", (int)map_type);
	kfree(svec->map[map_type]);
	svec->map[map_type] = NULL;
	return 0;
}

int svec_bootloader_unlock(struct svec_dev *svec)
{
	struct device *dev = svec->dev;
	const uint32_t boot_seq[8] = {	0xde, 0xad, 0xbe, 0xef,
					0xca, 0xfe, 0xba, 0xbe};
	void *addr;
	int i;

	/* Check if CS/CSR window is mapped */
	if (svec->map[MAP_CR_CSR] == NULL) {
		dev_err(dev, "CS/CSR window not found\n");
		return -EINVAL;
	}

	addr = svec->map[MAP_CR_CSR]->kernel_va +
				SVEC_BASE_LOADER + XLDR_REG_BTRIGR;

	/* Magic sequence: unlock bootloader mode, disable application FPGA */
	for (i = 0; i < 8; i++)
		iowrite32(cpu_to_be32(boot_seq[i]), addr);

	dev_info(dev, "Wrote unlock sequence at %lx\n", (unsigned long)addr);

	return 0;
}

int svec_is_bootloader_active(struct svec_dev *svec)
{
	struct device *dev = svec->dev;
	uint32_t idc;
	char buf[5];
	void *addr;

	/* Check if CS/CSR window is mapped */
	if (svec->map[MAP_CR_CSR] == NULL) {
		dev_err(dev, "CS/CSR window not found\n");
		return -EINVAL;
	}

	addr = svec->map[MAP_CR_CSR]->kernel_va +
					SVEC_BASE_LOADER + XLDR_REG_IDR;

	idc = be32_to_cpu(ioread32(addr));
	idc = htonl(idc);

	strncpy(buf, (char *)&idc, 4);
	buf[4] = 0;
	if (strncmp(buf, "SVEC", 4) == 0) {
		dev_info(dev, "IDCode value %x [%s].\n", idc, buf);
		/* Bootloader active. Unlocked */
		return 1;
	} else
		dev_info(dev, "IDCode value %x.\n", idc);

	/* Bootloader not active. Locked */
	return 0;
}

static void svec_csr_write(u8 value, void *base, u32 offset)
{
	offset -= offset % 4;
	iowrite32be(value, base + offset);
}

void svec_setup_csr_fa0(void *base, u32 vme, unsigned vector, unsigned level)
{
	u8 fa[4];		/* FUN0 ADER contents */

	/* reset the core */
	svec_csr_write(RESET_CORE, base, BIT_SET_REG);
	msleep(10);

	/* disable the core */
	svec_csr_write(ENABLE_CORE, base, BIT_CLR_REG);

	/* default to 32bit WB interface */
	svec_csr_write(WB32, base, WB_32_64);

	/* set interrupt vector and level */
	svec_csr_write(vector, base, INTVECTOR);
	svec_csr_write(level, base, INT_LEVEL);

	/* do address relocation for FUN0 */
	fa[0] = (vme >> 24) & 0xFF;
	fa[1] = (vme >> 16) & 0xFF;
	fa[2] = (vme >> 8 ) & 0xFF;
	fa[3] = (VME_A32_USER_DATA_SCT & 0x3F) << 2;
			/* DFSR and XAM are zero */

	svec_csr_write(fa[0], base, FUN0ADER);
	svec_csr_write(fa[1], base, FUN0ADER + 4);
	svec_csr_write(fa[2], base, FUN0ADER + 8);
	svec_csr_write(fa[3], base, FUN0ADER + 12);

	/* enable module, hence make FUN0 available */
	svec_csr_write(ENABLE_CORE, base, BIT_SET_REG);
}

int svec_load_fpga(struct svec_dev *svec, const void *blob, int size)
{
	struct device *dev = svec->dev;
	const uint32_t *data = blob;
	void *loader_addr; /* FPGA loader virtual address */
	uint32_t n;
	uint32_t rval = 0;
	int xldr_fifo_r0;  /* Bitstream data input control register */
	int xldr_fifo_r1;  /* Bitstream data input register */
	int i;
	u64 timeout;

	/* Check if we have something to do... */
	if ((data == NULL) || (size == 0)) {
		dev_err(dev, "%s: data to be load is NULL\n", __func__);
		return -EINVAL;
	}

	/* Unlock (activate) bootloader */
	if (svec_bootloader_unlock(svec)) {
		dev_err(dev, "Bootloader unlock failed\n");
		return -EINVAL;
	}

	/* Check if bootloader is active */
	if (!svec_is_bootloader_active(svec)) {
		dev_err(dev, "Bootloader locked after unlock!\n");
		return -EINVAL;
	}

	/* FPGA loader virtual address */
	loader_addr = svec->map[MAP_CR_CSR]->kernel_va + SVEC_BASE_LOADER;

	iowrite32(cpu_to_be32(XLDR_CSR_SWRST), loader_addr + XLDR_REG_CSR);
	iowrite32(cpu_to_be32(XLDR_CSR_START | XLDR_CSR_MSBF),
				loader_addr + XLDR_REG_CSR);

	i = 0;
	while (i < size) {
		rval = be32_to_cpu(ioread32(loader_addr + XLDR_REG_FIFO_CSR));
		if (!(rval & XLDR_FIFO_CSR_FULL)) {
			n = (size - i > 4 ? 4 : size - i);
			xldr_fifo_r0 = (n - 1) |
					((n<4) ? XLDR_FIFO_R0_XLAST : 0);
			xldr_fifo_r1 = htonl(data[i>>2]);

			iowrite32(cpu_to_be32(xldr_fifo_r0),
					loader_addr + XLDR_REG_FIFO_R0);
			iowrite32(cpu_to_be32(xldr_fifo_r1),
					loader_addr + XLDR_REG_FIFO_R1);
			i += n;
		}
	}

	/* Two seconds later */
	timeout = get_jiffies_64() + 2 * HZ;
	while (time_before64(get_jiffies_64(), timeout)) {
		rval = be32_to_cpu(ioread32(loader_addr + XLDR_REG_CSR));
		if (rval & XLDR_CSR_DONE)
			break;
		msleep(1);
	}

	if (!(rval & XLDR_CSR_DONE)) {
		dev_err(dev, "error: FPGA program timeout.\n");
		return -EIO;
	}

	if (rval & XLDR_CSR_ERROR) {
		dev_err(dev, "Bitstream loaded, status ERROR\n");
		return -EINVAL;
	}

	dev_info(dev, "Bitstream loaded, status: OK\n");

	/* give the VME bus control to App FPGA */
	iowrite32(cpu_to_be32(XLDR_CSR_EXIT), loader_addr + XLDR_REG_CSR);

	return 0;
}

static int svec_remove(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec = dev_get_drvdata(pdev);

	svec_fmc_destroy(svec);

	svec_unmap_window(svec, MAP_CR_CSR);
	svec_unmap_window(svec, MAP_REG);
	svec_remove_sysfs_files(svec);
	kfree(svec);

	dev_info(pdev, "removed\n");

	return 0;
}

int svec_load_fpga_file(struct svec_dev *svec, const char *name)
{
	struct device *dev = svec->dev;
	const struct firmware *fw;
	int err = 0;

	if (name == NULL) {
		dev_err(dev, "%s. File name is NULL\n", __func__);
		return -EINVAL;
	}

	err = request_firmware(&fw, name, dev);

	if (err < 0) {
		dev_err(dev, "Request firmware \"%s\": error %i\n",
			name, err);
		return err;
	}
	dev_info(dev, "Got file \"%s\", %zi (0x%zx) bytes\n",
			name, fw->size, fw->size);

	err = svec_load_fpga(svec, (uint32_t *)fw->data, fw->size);
	release_firmware(fw);

	return err;
}

int svec_is_present(struct svec_dev *svec)
{
	struct device *dev = svec->dev;
	uint32_t idc;
	void *addr;

	/* Check for bootloader */
	if (svec_is_bootloader_active(svec)) {
		return 1;
	}

	/* Ok, maybe there is a svec, but bootloader is not active.
	In such case, a CR/CSR with a valid manufacturer ID should exist*/

	addr = svec->map[MAP_CR_CSR]->kernel_va + VME_VENDOR_ID_OFFSET;

	idc = be32_to_cpu(ioread32(addr)) << 16;
	idc += be32_to_cpu(ioread32(addr + 4))  << 8;
	idc += be32_to_cpu(ioread32(addr + 8));

	if (idc == SVEC_VENDOR_ID) {
		dev_info(dev, "vendor ID is 0x%08x\n", idc);
		return 1;
	}

	dev_err(dev, "wrong vendor ID. 0x%08x found, 0x%08x expected\n",
			idc, SVEC_VENDOR_ID);
	dev_err(dev, "SVEC not present at slot %d\n", svec->slot);

	return 0;

}

static int svec_probe(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec;
	const char *name;
	int error = 0;

	if (lun[ndev] >= SVEC_MAX_DEVICES) {
		dev_err(pdev, "Card lun %d out of range [0..%d]\n",
			lun[ndev], SVEC_MAX_DEVICES - 1);
		return -EINVAL;
	}

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (svec == NULL) {
		dev_err(pdev, "Cannot allocate memory for svec card struct\n");
		return -ENOMEM;
	}

	/* Initialize struct fields*/
	svec->lun = lun[ndev];
	svec->slot = slot[ndev];
	svec->vmebase = vmebase[ndev];
	svec->vector = vector[ndev];
	svec->level = SVEC_IRQ_LEVEL; /* Default value */
	svec->fmcs_n = SVEC_N_SLOTS; /* FIXME: Two mezzanines */
	svec->dev = pdev;

	/* Get firmware name */
	if (ndev < fw_name_num) {
		svec->fw_name = fw_name[ndev];
	} else {
		svec->fw_name = svec_fw_name; /* Default value */
		dev_warn(pdev, "'fw_name' parameter not provided,"\
				" using %s as default\n", svec->fw_name);
	}

	/* Map CR/CSR space */
	error = svec_map_window(svec, MAP_CR_CSR);
	if (error)
		goto failed;

	if (!svec_is_present(svec)) {
		error = -EINVAL;
		goto failed_unmap_crcsr;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
	name = dev_name(pdev);
#endif
	strlcpy(svec->driver, KBUILD_MODNAME, sizeof(svec->driver));
	snprintf(svec->description, sizeof(svec->description),
		"SVEC at VME-A32 slot %d 0x%08x - 0x%08x irqv %d irql %d",
		svec->slot, svec->slot << 19, svec->vmebase,
		vector[ndev], svec->level);

	dev_info(pdev, "%s\n", svec->description);

	dev_set_drvdata(svec->dev, svec);
	error = svec_create_sysfs_files(svec);
	if (error) {
		dev_err(pdev, "Error creating sysfs files\n");
		goto failed_unmap_crcsr;
	}

	/* Load the golden FPGA binary to read the eeprom */
	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		goto failed_sysfs;

	/* configure and activate function 0 */
	svec_setup_csr_fa0(svec->map[MAP_CR_CSR]->kernel_va, vmebase[ndev],
				vector[ndev], svec->level);

	/* Map A32 space */
	error = svec_map_window(svec, MAP_REG);
	if (error)
		goto failed_sysfs;

	error = svec_fmc_create(svec);
	if (error) {
		dev_err(pdev, "error creating fmc devices\n");
		goto failed_unmap;
	}

	return 0;

failed_unmap:
	svec_unmap_window(svec, MAP_REG);
failed_sysfs:
	svec_remove_sysfs_files(svec);
failed_unmap_crcsr:
	svec_unmap_window(svec, MAP_CR_CSR);

failed:
	kfree(svec);

	return error;
}

static struct vme_driver svec_driver = {
	.probe		= svec_probe,
	.remove		= svec_remove,
	.driver		= {
	.name		= KBUILD_MODNAME,
	},
};

static int __init svec_init(void)
{
	int error = 0;

	/* Check that all insmod argument vectors are the same length */
	if (lun_num != slot_num || lun_num != vmebase_num ||
		lun_num != vector_num) {
		pr_err("%s: The number of parameters doesn't match\n",
		       __func__);
		return -EINVAL;
	}

	error = vme_register_driver(&svec_driver, lun_num);
	if (error) {
		pr_err("%s: Cannot register vme driver - lun [%d]\n", __func__,
			lun_num);
	}

	return error;
}

static void __exit svec_exit(void)
{
	vme_unregister_driver(&svec_driver);
}


module_init(svec_init);
module_exit(svec_exit);

MODULE_AUTHOR("Juan David Gonzalez Cobas");
MODULE_LICENSE("GPL");
MODULE_VERSION(GIT_VERSION);
MODULE_DESCRIPTION("svec driver");
