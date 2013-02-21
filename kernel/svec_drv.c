/**
* svec_drv.c
* Driver for SVEC carrier.
*
* Released under GPL v2. (and only v2, not any later version)
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

#define DRIVER_NAME	"svec"
#define PFX		DRIVER_NAME ": "

char *svec_fw_name = "fmc/svec_golden.bin";

/* Module parameters */
static long vmebase1[SVEC_MAX_DEVICES];
static unsigned int vmebase1_num;
static long vmebase2[SVEC_MAX_DEVICES];
static unsigned int vmebase2_num;
static char *fw_name[SVEC_MAX_DEVICES];
static unsigned int fw_name_num;
static int  vector[SVEC_MAX_DEVICES];
static unsigned int vector_num;
static int  level[SVEC_MAX_DEVICES];
static unsigned int level_num;
static int lun[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IDX;
static unsigned int lun_num;


module_param_array(vmebase1, ulong, &vmebase1_num, S_IRUGO);
MODULE_PARM_DESC(vmebase1, "VME Base Adress #1 of the SVEC card");
module_param_array(vmebase2, ulong, &vmebase2_num, S_IRUGO);
MODULE_PARM_DESC(vmebase2, "VME Base Adress #2 of the SVEC card");
module_param_array_named(fw_name, fw_name , charp, &fw_name_num, S_IRUGO);
MODULE_PARM_DESC(fw_name, "firmware file");
module_param_array(vector, int, &vector_num, S_IRUGO);
MODULE_PARM_DESC(vector, "IRQ vector");
module_param_array(level, int, &level_num, S_IRUGO);
MODULE_PARM_DESC(level, "IRQ level");
module_param_array(lun, int, &lun_num, S_IRUGO);
MODULE_PARM_DESC(lun, "Index value for SVEC card");

/* For device creation. Not really sure if it's necessary... */
static dev_t svec_devno;

static const struct file_operations svec_fops = {
	.owner = THIS_MODULE,
};

int svec_map_window(struct svec_dev *svec, enum svec_map_win map_type)
{
	struct device *dev = svec->dev;
	enum vme_address_modifier am = VME_CR_CSR;
	enum vme_data_width dw = VME_D32;
	unsigned long base = svec->vmebase1;
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
		base = svec->vmebase2;
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
	for (i=0; i<8; i++)
		iowrite32(cpu_to_be32(boot_seq[i]), addr);

	dev_info(dev, "Wrote unlock sequence at %x\n", (unsigned int)addr);

	return 0;
}

int svec_bootloader_is_active(struct svec_dev *svec)
{
	struct device *dev = svec->dev;
	uint32_t idc;
	char *buf = (char *)&idc;
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
	uint32_t rval;
	int xldr_fifo_r0;  /* Bitstream data input control register */
	int xldr_fifo_r1;  /* Bitstream data input register */
	int i;
	unsigned long j;
	int err = 0;

	/* Check if we have something to do... */
	if ((data == NULL) || (size == 0)){
		dev_err(dev, "%s: data to be load is NULL\n", __func__);
		return -EINVAL;
	}

	/* Unlock (activate) bootloader */
	if (svec_bootloader_unlock(svec)) {
		dev_err(dev, "Bootloader unlock failed\n");
		return -EINVAL;
	}

	/* Check if bootloader is active */
	if (!svec_bootloader_is_active(svec)) {
		dev_err(dev, "Bootloader locked after unlock!\n");
		return -EINVAL;
	}

	/* FPGA loader virtual address */
	loader_addr = svec->map[MAP_CR_CSR]->kernel_va + SVEC_BASE_LOADER;

	iowrite32(cpu_to_be32(XLDR_CSR_SWRST), loader_addr + XLDR_REG_CSR);
	iowrite32(cpu_to_be32(XLDR_CSR_START | XLDR_CSR_MSBF),
				loader_addr + XLDR_REG_CSR);

	i = 0;
	while(i < size) {
		rval = be32_to_cpu(ioread32(loader_addr + XLDR_REG_FIFO_CSR));
		if (!(rval & XLDR_FIFO_CSR_FULL)) {
			n = (size-i>4?4:size-i);
			xldr_fifo_r0 = (n - 1) |
					((n<4) ? XLDR_FIFO_R0_XLAST : 0);
			xldr_fifo_r1 = htonl(data[i>>2]);

			iowrite32(cpu_to_be32(xldr_fifo_r0),
					loader_addr + XLDR_REG_FIFO_R0);
			iowrite32(cpu_to_be32(xldr_fifo_r1),
					loader_addr + XLDR_REG_FIFO_R1);
			i+=n;
		}
	}

	/* Two seconds later */
	j = jiffies + 2 * HZ;
	do {
		if (time_after(jiffies, j)) {
			dev_err(dev, "error: FPGA program timeout.\n");
			return -EIO;
		}
		rval = be32_to_cpu(ioread32(loader_addr + XLDR_REG_CSR));
	} while (!(rval & XLDR_CSR_DONE));

	if (rval & XLDR_CSR_ERROR) {
		dev_err(dev, "Bitstream loaded, status ERROR\n");
		return -EINVAL;
	}

	dev_info(dev, "Bitstream loaded, status: OK\n");

	/* give the VME bus control to App FPGA */
	iowrite32(cpu_to_be32(XLDR_CSR_EXIT), loader_addr + XLDR_REG_CSR);

	return err;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec = dev_get_drvdata(pdev);

	svec_fmc_destroy(svec);

	svec_unmap_window(svec, MAP_CR_CSR);
	svec_unmap_window(svec, MAP_REG);

	return 0;
}

static int __devinit svec_match(struct device *pdev, unsigned int ndev)
{
	/* TODO */

	if (ndev >= vmebase1_num)
		 return 0;
	if (ndev >= vector_num || ndev >= level_num) {
		dev_warn(pdev, "irq vector/level missing\n");
		return 0;
	}

	return 1;
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
	dev_info(dev, "Got file \"%s\", %i (0x%x) bytes\n",
			name, fw->size, fw->size);

	err = svec_load_fpga(svec, (uint32_t *)fw->data, fw->size);
	release_firmware(fw);

	return err;
}

static int __devinit svec_probe(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec;
	const char *name;
	dev_t devno;
	int error = 0;

	if (lun[ndev] >= SVEC_MAX_DEVICES) {
		dev_err(pdev, "Card lun %d out of range [0..%d]\n",
			lun[ndev], SVEC_MAX_DEVICES -1);
		return -EINVAL;
	}

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (svec == NULL) {
		dev_err(pdev, "Cannot allocate memory for svec card struct\n");
		return -ENOMEM;
	}

	/* Initialize struct fields*/
	svec->lun = lun[ndev];
	svec->vmebase1 = vmebase1[ndev];
	svec->vmebase2 = vmebase2[ndev];
	svec->vector = vector[ndev];
	svec->level = level[ndev];
	svec->fw_name = fw_name[ndev];
	svec->slot_n = 2; /* FIXME: Two mezzanines */
	svec->dev = pdev;

	/* Alloc fmc structs memory */
	svec->fmcs = kzalloc(svec->slot_n * sizeof(struct fmc_device),
				GFP_KERNEL);
	if (!svec->fmcs) return -ENOMEM;

	/* Map CR/CSR space */
	error = svec_map_window(svec, MAP_CR_CSR);
	if (error)
		goto failed;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
	name = dev_name(pdev);
#endif
	strlcpy(svec->driver, DRIVER_NAME, sizeof(svec->driver));
	snprintf(svec->description, sizeof(svec->description),
		"SVEC at VME-A32 0x%08lx - 0x%08lx irqv %d irql %d",
		svec->vmebase1, svec->vmebase2, vector[ndev], level[ndev]);

	dev_info(pdev, "%s\n", svec->description);

	/*Create cdev, just to know the carrier is there */
	devno = MKDEV(MAJOR(svec_devno), ndev);

	cdev_init(&svec->cdev, &svec_fops);
	svec->cdev.owner = THIS_MODULE;
	error = cdev_add(&svec->cdev, devno, 1);
	if (error) {
		dev_err(pdev, "Error %d adding cdev %d\n", error, ndev);
		goto failed;
	}

	dev_set_drvdata(svec->dev, svec);
	error = svec_create_sysfs_files(svec);
	if (error) {
		dev_err(pdev, "Error creating sysfs files\n");
		goto failed;
	}

	/* Load the golden FPGA binary to read the eeprom */
	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		goto failed;

	/* configure and activate function 0 */
	svec_setup_csr_fa0(svec->map[MAP_CR_CSR]->kernel_va, vmebase2[ndev],
				vector[ndev], level[ndev]);

	/* Map A32 space */
	error = svec_map_window(svec, MAP_REG);
	if (error)
		goto failed;

	error = svec_fmc_create(svec);
	if (error) {
		dev_err(pdev, "error creating fmc devices\n");
		goto failed_unmap;
	}

	return 0;

failed_unmap:
	svec_unmap_window(svec, MAP_CR_CSR);
	svec_unmap_window(svec, MAP_REG);

failed:
	kfree(svec->fmcs);
	svec->fmcs = NULL;
	kfree(svec);
	return error;
}

static struct vme_driver svec_driver = {
	.match		= svec_match,
	.probe		= svec_probe,
	.remove		= __devexit_p(svec_remove),
	.driver		= {
	.name		= DRIVER_NAME,
	},
};

static int __init svec_init(void)
{
	int error = 0;

	/* TODO: Check parameters */

	/* Device creation for the carrier, just in case... */
	error = alloc_chrdev_region(&svec_devno, 0, lun_num, "svec");
	if (error) {
		pr_err("%s: Failed to allocate chrdev region\n", __func__);
		goto out;
	}

	error = vme_register_driver(&svec_driver, lun_num);
	if (error) {
		pr_err("%s: Cannot register vme driver - lun [%d]\n", __func__,
			lun_num);
	}

out:
	return error;
}

static void __exit svec_exit(void)
{
	vme_unregister_driver(&svec_driver);
	unregister_chrdev_region(svec_devno, lun_num);
}


module_init(svec_init);
module_exit(svec_exit);

MODULE_AUTHOR("");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec driver");
