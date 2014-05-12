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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/jhash.h>
#include "svec.h"
#include "hw/xloader_regs.h"

char *svec_fw_name = "fmc/svec-golden.bin";

/* Module parameters */
static int slot[SVEC_MAX_DEVICES];
static unsigned int slot_num;
static unsigned int vme_base[SVEC_MAX_DEVICES] = SVEC_UNINITIALIZED_VME_BASE;
static unsigned int vme_base_num;
static char *fw_name[SVEC_MAX_DEVICES];
static unsigned int fw_name_num;
static int vector[SVEC_MAX_DEVICES] = SVEC_UNINITIALIZED_IRQ_VECTOR;
static unsigned int vector_num;
static int level[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IRQ_LEVEL;
static unsigned int level_num;
static int lun[SVEC_MAX_DEVICES] = SVEC_DEFAULT_IDX;
static unsigned int lun_num;
static int vme_am[SVEC_MAX_DEVICES] = SVEC_DEFAULT_VME_AM;
static unsigned int vme_am_num;
static int vme_size[SVEC_MAX_DEVICES] = SVEC_DEFAULT_VME_SIZE;
static unsigned int vme_size_num;
static int verbose = 0;

module_param_array(slot, int, &slot_num, S_IRUGO);
MODULE_PARM_DESC(slot, "Slot where SVEC card is installed");
module_param_array(lun, int, &lun_num, S_IRUGO);
MODULE_PARM_DESC(lun, "Index value for SVEC card");
module_param_array(vme_base, uint, &vme_base_num, S_IRUGO);
MODULE_PARM_DESC(vme_base, "VME Base address of the SVEC card registers");
module_param_array(vme_am, uint, &vme_am_num, S_IRUGO);
MODULE_PARM_DESC(vme_size, "VME Window size of the SVEC card registers");
module_param_array(vme_size, uint, &vme_size_num, S_IRUGO);
MODULE_PARM_DESC(vme_am, "VME Address modifier of the SVEC card registers");
module_param_array_named(fw_name, fw_name, charp, &fw_name_num, S_IRUGO);
MODULE_PARM_DESC(fw_name, "Firmware file");
module_param_array(vector, int, &vector_num, S_IRUGO);
MODULE_PARM_DESC(vector, "IRQ vector");
module_param_array(level, int, &level_num, S_IRUGO);
MODULE_PARM_DESC(level, "IRQ level");
module_param(verbose, int, S_IRUGO);
MODULE_PARM_DESC(verbose, "Output lots of debugging messages");

/* Maps given VME window using configuration provided through module parameters or sysfs.
   Two windows are supported:
   - MAP_CR_CSR: CR/CSR space for bootloading the FPGA bitstream and initializing 
     the VME64x CSR registers
   - MAP_REG: the main VME window for the FMC driver. Configured via 
     module parameters or sysfs.
*/

int svec_map_window(struct svec_dev *svec, enum svec_map_win map_type)
{
	struct device *dev = svec->dev;
	enum vme_address_modifier am;
	unsigned long base;
	unsigned int size;
	int rval;

	if (svec->map[map_type] != NULL) {
		dev_err(dev, "Window %d already mapped\n", (int)map_type);
		return -EPERM;
	}

	/* Default values are for MAP_CR_CSR */
	/* For register map, we need to set them to: */
	if (map_type == MAP_REG) {
		am = svec->cfg_cur.vme_am;
		base = svec->cfg_cur.vme_base;
		size = svec->cfg_cur.vme_size;
	} else {
		am = VME_CR_CSR;
		base = svec->slot * 0x80000;
		size = 0x80000;
	}

	svec->map[map_type] = kzalloc(sizeof(struct vme_mapping), GFP_KERNEL);
	if (!svec->map[map_type]) {
		dev_err(dev, "Cannot allocate memory for vme_mapping struct\n");
		return -ENOMEM;
	}

	/* Window mapping */
	svec->map[map_type]->am = am;
	svec->map[map_type]->data_width = VME_D32;
	svec->map[map_type]->vme_addru = 0;
	svec->map[map_type]->vme_addrl = base;
	svec->map[map_type]->sizeu = 0;
	svec->map[map_type]->sizel = size;

	if ((rval = vme_find_mapping(svec->map[map_type], 1)) != 0) {
		dev_err(dev, "Failed to map window %d: (%d)\n",
			(int)map_type, rval);
		kfree(svec->map[map_type]);
		svec->map[map_type] = NULL;
		return -EINVAL;
	}

	if(svec->verbose)
	dev_info(dev, "%s mapping successful at 0x%p\n",
		 map_type == MAP_REG ? "register" : "CR/CSR",
		 svec->map[map_type]->kernel_va);

	return 0;
}

/* Unmaps given VME window */
int svec_unmap_window(struct svec_dev *svec, enum svec_map_win map_type)
{
	struct device *dev = svec->dev;

	if (svec->map[map_type] == NULL)
		return 0;

	if (vme_release_mapping(svec->map[map_type], 1)) {
		dev_err(dev, "Unmap for window %d failed\n", (int)map_type);
		return -EINVAL;
	}
	
	if(svec->verbose)
	dev_info(dev, "Window %d unmapped\n", (int)map_type);
	
	kfree(svec->map[map_type]);
	svec->map[map_type] = NULL;
	return 0;
}

/* Checks if the card responds to a bootloader call in order to determine if
   we are talking to a SVEC or not. If it is a SVEC, its Application FPGA is erased!
*/
int svec_check_bootloader_present(struct svec_dev *svec)
{
	int rv = 0;

	if (!svec->map[MAP_CR_CSR])
		rv = svec_map_window(svec, MAP_CR_CSR);

	if (rv)
		return rv;

	if (svec_bootloader_unlock(svec)) {
		rv = -EINVAL;
		goto fail;
	}

	/* Check if bootloader is active */
	if (!svec_is_bootloader_active(svec)) {
		rv = -EINVAL;
		goto fail;
	}

      fail:
	svec_unmap_window(svec, MAP_CR_CSR);

	return rv;
}

/* Writes a "magic" unlock sequence, activating the System FPGA bootloader
   regardless of what is going on in the Application FPGA. */
int svec_bootloader_unlock(struct svec_dev *svec)
{
	struct device *dev = svec->dev;
	const uint32_t boot_seq[8] = { 0xde, 0xad, 0xbe, 0xef,
		0xca, 0xfe, 0xba, 0xbe
	};
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

	if(svec->verbose)
	dev_info(dev, "Wrote unlock sequence at %lx\n", (unsigned long)addr);

	return 0;
}

/* Checks if the SVEC is in bootloader mode. If true, it implies that the Appliocation
   FPGA has no bitstream loaded. */
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
		
		if(svec->verbose)
		    dev_info(dev, "IDCode value %x [%s].\n", idc, buf);
		/* Bootloader active. Unlocked */
		return 1;
	}

	/* Bootloader not active. Locked */
	return 0;
}

static void svec_csr_write(u8 value, void *base, u32 offset)
{
	offset -= offset % 4;
	iowrite32be(value, base + offset);
}

/* Loads the Application FPGA bitstream through the System FPGA bootloader. 
   Does all necessary VME mappings & checks if the bitstream has not been already
   loaded to save time. */
int svec_load_fpga(struct svec_dev *svec, const void *blob, int size)
{
	struct device *dev = svec->dev;
	const uint32_t *data = blob;
	void *loader_addr;	/* FPGA loader virtual address */
	uint32_t n;
	uint32_t rval = 0;
	uint32_t fw_hash;
	int xldr_fifo_r0;	/* Bitstream data input control register */
	int xldr_fifo_r1;	/* Bitstream data input register */
	int i;
	u64 timeout;
	int rv = 0;

	clear_bit(SVEC_FLAG_AFPGA_PROGRAMMED, &svec->flags);

	/* Hash firmware bitstream */
	fw_hash = jhash(blob, size, 0);
	if (fw_hash == svec->fw_hash) {
		if(svec->verbose)
		    dev_info(svec->dev,
			 "card already programmed with bitstream with hash 0x%x\n",
			 fw_hash);
    
    		set_bit(SVEC_FLAG_AFPGA_PROGRAMMED, &svec->flags);
		return 0;
	}

	/* Check if we have something to do... */
	if ((data == NULL) || (size == 0)) {
		dev_err(dev, "%s: data to be load is NULL\n", __func__);
		return -EINVAL;
	}
	if (!svec->map[MAP_CR_CSR])
		rv = svec_map_window(svec, MAP_CR_CSR);

	if (rv)
		return rv;

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
			    ((n < 4) ? XLDR_FIFO_R0_XLAST : 0);
			xldr_fifo_r1 = htonl(data[i >> 2]);

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

	if(svec->verbose)
	dev_info(dev, "Bitstream loaded, status: OK\n");

	/* give the VME bus control to App FPGA */
	iowrite32(cpu_to_be32(XLDR_CSR_EXIT), loader_addr + XLDR_REG_CSR);

	/* give the VME core a little while to settle up */
	msleep(10);

	set_bit(SVEC_FLAG_AFPGA_PROGRAMMED, &svec->flags);

	/* after a successful reprogram, save the hash so that the future call can
	   return earlier if requested to load the same bitstream */
	svec->fw_hash = fw_hash;

	return 0;
}

static int svec_remove(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec = dev_get_drvdata(pdev);

	if (test_bit(SVEC_FLAG_FMCS_REGISTERED, &svec->flags)) {
		svec_fmc_destroy(svec);
		clear_bit(SVEC_FLAG_FMCS_REGISTERED, &svec->flags);
	}

	svec_irq_exit(svec);

	svec_unmap_window(svec, MAP_CR_CSR);
	svec_unmap_window(svec, MAP_REG);
	svec_remove_sysfs_files(svec);

	if(svec->verbose)
	    dev_info(pdev, "removed\n");

	kfree(svec);

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
		dev_err(dev, "Request firmware \"%s\": error %i\n", name, err);
		return err;
	}

	if(svec->verbose)
	dev_info(dev, "Got file \"%s\", %zi (0x%zx) bytes\n",
		 name, fw->size, fw->size);

	err = svec_load_fpga(svec, (uint32_t *) fw->data, fw->size);
	release_firmware(fw);

	return err;
}

/* Checks if a SVEC with a valid Application FPGA gateware is present at a given slot. */
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
	   In such case, a CR/CSR with a valid manufacturer ID should exist */

	addr = svec->map[MAP_CR_CSR]->kernel_va + VME_VENDOR_ID_OFFSET;

	idc = be32_to_cpu(ioread32(addr)) << 16;
	idc += be32_to_cpu(ioread32(addr + 4)) << 8;
	idc += be32_to_cpu(ioread32(addr + 8));

	if (idc == SVEC_VENDOR_ID) {
		if(svec->verbose)
		dev_info(dev, "vendor ID is 0x%08x\n", idc);
		return 1;
	}

	dev_err(dev, "wrong vendor ID. 0x%08x found, 0x%08x expected\n",
		idc, SVEC_VENDOR_ID);
	dev_err(dev, "SVEC not present at slot %d\n", svec->slot);

	return 0;
}

/* Sets up the VME64x core to respond to a address range and issue interrupts to given vector. */

int svec_setup_csr(struct svec_dev *svec)
{
	int rv = 0;
	int func;
	void *base;
	u8 ader[2][4];		/* FUN0/1 ADER contents */

	/* don't try to set up CSRs of an empty AFPGA */
	if (!test_bit(SVEC_FLAG_AFPGA_PROGRAMMED, &svec->flags))
	    return 0;

	if (!svec->map[MAP_CR_CSR])
		rv = svec_map_window(svec, MAP_CR_CSR);

	if (rv < 0)
		return rv;

	if (!svec_is_present(svec)) {
		rv = -ENODEV;
		goto exit_reconf;
	}

	base = svec->map[MAP_CR_CSR]->kernel_va;

	/* reset the core */
	svec_csr_write(RESET_CORE, base, BIT_SET_REG);
	msleep(10);

	/* disable the core */
	svec_csr_write(ENABLE_CORE, base, BIT_CLR_REG);

	/* default to 32bit WB interface */
	svec_csr_write(WB32, base, WB_32_64);

	/* set interrupt vector and level */
	svec_csr_write(svec->cfg_cur.interrupt_vector, base, INTVECTOR);
	svec_csr_write(svec->cfg_cur.interrupt_level, base, INT_LEVEL);

	switch (svec->cfg_cur.vme_am) {
		/* choose the function to use: A32 is 0, A24 is 1. The rest is purposedly disabled. */
	case VME_A32_USER_DATA_SCT:
		func = 0;
		break;
	case VME_A24_USER_DATA_SCT:
		func = 1;
		break;
	default:
		return 0;
	}

	memset(ader, 0, sizeof(ader));

	/* Below is a hack to keep the VME core function disabling work on bitstreams
	   containing a buggy VME core (commit b2fc3ce7): set bit 0 (XAM_MODE) to 1
	   to disable given function (because neither function 0 nor 1 have anything 
	   in their extended capability sets, setting XAM_MODE = 1 effectively disables 
	   the function. */

	ader[0][3] = 1; 
	ader[1][3] = 1;

	/* do address relocation for FUN0/1 */
	ader[func][0] = (svec->cfg_cur.vme_base >> 24) & 0xFF;
	ader[func][1] = (svec->cfg_cur.vme_base >> 16) & 0xFF;
	ader[func][2] = (svec->cfg_cur.vme_base >> 8) & 0xFF;
	ader[func][3] = (svec->cfg_cur.vme_am & 0x3F) << 2;

	/* DFSR and XAM are zero. Program both functions, but only one will be enabled. */
	svec_csr_write(ader[0][0], base, FUN0ADER);
	svec_csr_write(ader[0][1], base, FUN0ADER + 4);
	svec_csr_write(ader[0][2], base, FUN0ADER + 8);
	svec_csr_write(ader[0][3], base, FUN0ADER + 12);

	svec_csr_write(ader[1][0], base, FUN1ADER);
	svec_csr_write(ader[1][1], base, FUN1ADER + 4);
	svec_csr_write(ader[1][2], base, FUN1ADER + 8);
	svec_csr_write(ader[1][3], base, FUN1ADER + 12);

	/* enable module, hence make FUN0/1 available */
	svec_csr_write(ENABLE_CORE, base, BIT_SET_REG);

      exit_reconf:

	/* unmap the CSR window after configuring the card, it's no longer necessary */
	svec_unmap_window(svec, MAP_CR_CSR);
	return rv;
}

/* Performs some checks on VME window address/size/am and interrupts configuration.
   Note that the checks are not rock solid (no checking for overlapping VME windows
   for instance), so it's still possible for a determinate user to screw something up. */
int svec_validate_configuration(struct device *pdev, struct svec_config *cfg)
{
	uint32_t addr_mask, start_masked, end_masked;
	uint32_t max_size;

	/* no base address assigned? silently return. */
	if (cfg->vme_base == (uint32_t) - 1)
		return 0;

	if (cfg->interrupt_vector == (uint32_t) - 1) 
		return 0;
	
	switch (cfg->vme_am) {
	case VME_A32_USER_DATA_SCT:
		addr_mask = 0xff000000;
		max_size = 0x10000000;
		break;
	case VME_A24_USER_DATA_SCT:
		addr_mask = 0x00f80000;
		max_size  = 0x00100000;
		break;
	default:
		dev_err(pdev, "Unsupported VME address modifier 0x%x\n",
			cfg->vme_am);
		return 0;
	}

	if (cfg->vme_am == VME_A24_USER_DATA_SCT &&
	    cfg->vme_base >= 0xf00000)
	{
		dev_err(pdev,
			"VME base address for A24 mode must not be >= 0xf00000 due to "
			"addressing conflict with the Tsi148 VME bridge. Please change "
			"your card's configuration.\n");
		return 0;
	}

	start_masked = cfg->vme_base & ~(cfg->vme_size - 1);
	end_masked = (cfg->vme_base + cfg->vme_size - 1) & ~(cfg->vme_size - 1);

	if (cfg->vme_base & ~addr_mask) {
		dev_err(pdev,
			"VME base address incorrectly aligned (mask = 0x%x)\n",
			addr_mask);
		return 0;
	}

	if (start_masked != end_masked) {
		dev_err(pdev,
			"VME base address incorrectly aligned (start_masked = 0x%x, end_masked = 0x%x)\n",
			start_masked, end_masked);
		return 0;
	}

	if (cfg->vme_size > max_size) {
		dev_err(pdev,
			"VME window size too big (requested = 0x%x, maximum = 0x%x)\n",
			cfg->vme_size, max_size);
		return 0;
	}

	if (cfg->interrupt_vector < 0 || cfg->interrupt_vector > 0xff) {
		dev_err(pdev,
			"VME interrupt vector out of range (requested = 0x%x, allowed: 0x00 - 0xff)\n",
			cfg->interrupt_vector);
		return 0;
	}

	return 1;
}

static void svec_prepare_description(struct svec_dev *svec)
{
	if (svec->cfg_cur.configured) {
		snprintf(svec->description, sizeof(svec->description),
			 "SVEC.%d [slot: %d, am: 0x%02x, range: 0x%08x - 0x%08x irqv 0x%02x/%d]",
			 svec->lun, svec->slot,
			 svec->cfg_cur.vme_am,
			 svec->cfg_cur.vme_base,
			 svec->cfg_cur.vme_base + svec->cfg_cur.vme_size - 1,
			 svec->cfg_cur.interrupt_vector,
			 svec->cfg_cur.interrupt_level);

	} else {
		snprintf(svec->description, sizeof(svec->description),
			 "SVEC.%d [slot %d, VME to be configured via sysfs]",
			 svec->lun, svec->slot);
	}

	dev_info(svec->dev, "%s\n", svec->description);
}

/* Reconfigures everything after the VME configuration has been changed. Called during 
   probing of the card (if sufficient VME config is given via module parameters) or when the
   configuration is assigned through sysfs. Reconfiguration implies re-loading the FMCs. */
int svec_reconfigure(struct svec_dev *svec)
{
	int error;

	/* no valid VME configuration? Silently return (it has to be done at some point via sysfs) */
	if (!svec->cfg_cur.configured)
		return 0;

	/* FMCs loaded: remove before reconfiguring VME */
	if (test_bit(SVEC_FLAG_FMCS_REGISTERED, &svec->flags)) {
		if(svec->verbose)
		dev_info(svec->dev,
			 "re-registering FMCs due to sysfs-triggered card reconfiguration\n");
		svec_fmc_destroy(svec);
		clear_bit(SVEC_FLAG_FMCS_REGISTERED, &svec->flags);
	}

	/* Unmap, config the VME core and remap the new window. */
	if (svec->map[MAP_REG])
		svec_unmap_window(svec, MAP_REG);

	svec_irq_exit(svec);

	error = svec_setup_csr(svec);
	if (error)
		return error;

	error = svec_map_window(svec, MAP_REG);
	if (error)
		return error;

	/* Update the card description */
	svec_prepare_description(svec);

	/* FMC initialization enabled? Start up the FMC drivers. */
	if (svec->cfg_cur.use_fmc) {
		error = svec_fmc_create(svec);
		if (error) {
			dev_err(svec->dev, "error creating fmc devices\n");
			goto failed_unmap;
		}
		set_bit(SVEC_FLAG_FMCS_REGISTERED, &svec->flags);
	}

	return 0;
      failed_unmap:
	svec_unmap_window(svec, MAP_REG);
	return error;
}

/* Loads the golden bitstream. */
int svec_load_golden(struct svec_dev *svec)
{
	int error;

	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		return error;

	set_bit(SVEC_FLAG_AFPGA_PROGRAMMED, &svec->flags);

	error = svec_setup_csr(svec);
	if (error)
		return error;

	return 0;
}

static int svec_probe(struct device *pdev, unsigned int ndev)
{
	struct svec_dev *svec;
	const char *name;
	int error = 0;

	if (lun[ndev] < 0 || lun[ndev] >= SVEC_MAX_DEVICES) {
		dev_err(pdev, "Card lun %d out of range [0..%d]\n",
			lun[ndev], SVEC_MAX_DEVICES - 1);
		return -EINVAL;
	}

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (svec == NULL) {
		dev_err(pdev, "Cannot allocate memory for svec card struct\n");
		return -ENOMEM;
	}

	/* Initialize struct fields */
	svec->verbose = verbose;
	svec->lun = lun[ndev];
	svec->slot = slot[ndev];
	svec->fmcs_n = SVEC_N_SLOTS;	/* FIXME: Two mezzanines */
	svec->dev = pdev;

	svec->cfg_cur.use_vic = 1;
	svec->cfg_cur.use_fmc = 1;
	svec->cfg_cur.vme_base = vme_base[ndev];
	svec->cfg_cur.vme_am = vme_am[ndev];
	svec->cfg_cur.vme_size = vme_size[ndev];
	svec->cfg_cur.interrupt_vector = vector[ndev];
	svec->cfg_cur.interrupt_level = level[ndev];
	svec->cfg_cur.configured = 1;
	svec->cfg_cur.configured =
	    svec_validate_configuration(pdev, &svec->cfg_cur);
	svec->cfg_new = svec->cfg_cur;

	/* see if we are really talking to a SVEC */
	if (svec_check_bootloader_present(svec) < 0) {
		dev_err(pdev,
			"ERROR: The SVEC expected in slot %d is not responding, "
			"the mezzanines installed on it will not be visible in the" 
			"system. Please check if the card is correctly installed.\n",
			svec->slot);

		error = -ENODEV;
		goto failed;
	}

	/* Get firmware name */
	if (ndev < fw_name_num) {
		svec->fw_name = fw_name[ndev];
	} else {
		svec->fw_name = svec_fw_name;	/* Default value */
	}

	if(svec->verbose)
	    dev_info(pdev, "using '%s' golden bitstream.", svec->fw_name);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
	name = dev_name(pdev);
#endif
	strlcpy(svec->driver, KBUILD_MODNAME, sizeof(svec->driver));
	snprintf(svec->name, sizeof(svec->name), "svec.%d", svec->lun);

	dev_set_drvdata(svec->dev, svec);

	svec_prepare_description(svec);

	error = svec_create_sysfs_files(svec);
	if (error) {
		dev_err(pdev, "Error creating sysfs files\n");
		goto failed;
	}

	/* Map user address space & give control to the FMCs */
	svec_reconfigure(svec);

	return 0;

	svec_remove_sysfs_files(svec);
      failed:
	kfree(svec);

	return error;
}

static struct vme_driver svec_driver = {
	.probe = svec_probe,
	.remove = svec_remove,
	.driver = {
		   .name = KBUILD_MODNAME,
		   },
};

static int __init svec_init(void)
{
	int error = 0;

	if (lun_num == 0) {
		pr_err("%s: Need at least one slot/LUN pair.\n", __func__);
		return -EINVAL;
	}

	/* Check that all insmod argument vectors are the same length */
	if (lun_num != slot_num) {
		pr_err("%s: The number of parameters doesn't match\n",
		       __func__);
		return -EINVAL;
	}

	error |= (vme_base_num && vme_base_num != slot_num);
	error |= (vme_am_num && vme_am_num != slot_num);
	error |= (vme_size_num && vme_size_num != slot_num);
	error |= (level_num && level_num != slot_num);
	error |= (vector_num && vector_num != slot_num);
	error |= (fw_name_num && fw_name_num != slot_num);

	if (error) {
		pr_err
		    ("%s: The number of vme_base/vme_am/vme_size/level/vector/fw_name/use_vic/use_fmc parameters must be zero or equal to the number of cards.\n",
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
