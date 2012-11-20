/**
* svec_drv.c
* Driver for SVEC carrier.
* 
* Released under GPL v2. (and only v2, not any later version)
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <vmebus.h>
#include "svec.h"
#include "xloader_regs.h"

#define DRIVER_NAME	"svec"
#define PFX		DRIVER_NAME ": "
#define BASE_LOADER 0x70000
#define MS_TICKS 1
#define TIMEOUT 1000

/* Module parameters */
char *svec_fw_name = "fmc/svec-init.bin";
/*
module_param_named(fw_name, svec_fw_name, charp, 0444);
*/
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


module_param_array(vmebase1, long, &vmebase1_num, S_IRUGO);
MODULE_PARM_DESC(vmebase1, "VME Base Adress #1 of the SVEC card");
module_param_array(vmebase2, long, &vmebase2_num, S_IRUGO);
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
static struct vme_mapping *cs_csr = NULL;
static struct class *svec_class;

static const struct file_operations svec_fops = {
	.owner = THIS_MODULE,
};

static inline unsigned short swapbe16(unsigned short val)
{
        return (((val & 0xff00) >> 8) | ((val & 0xff) << 8));
}

static inline unsigned int swapbe32(unsigned int val)
{
        return (((val & 0xff000000) >> 24) | ((val & 0xff0000) >> 8) |
                ((val & 0xff00) << 8) | ((val & 0xff) << 24));
}

int map_cs_csr(struct svec_dev *svec)
{
	int rval;

	if (cs_csr != NULL) {
		printk(KERN_ERR PFX "Error: CS/CSR already mapped?\n");
		return -EPERM;
	}
	cs_csr = kzalloc(sizeof(struct vme_mapping), GFP_KERNEL);
	if (!cs_csr) {
		printk(KERN_ERR PFX "kzalloc failed allocating memory for vme_mapping struct\n");
		return -ENOMEM;
	}

	/* CS/CSR mapping*/
	cs_csr->am = 		VME_CR_CSR; /* 0x2f */
	cs_csr->data_width = 	VME_D32;
	cs_csr->vme_addru =	0;
	cs_csr->vme_addrl =	svec->vmebase1;
	cs_csr->sizeu =		0;
	cs_csr->sizel = 	0x80000;
	cs_csr->window_num =	0;
	cs_csr->bcast_select = 0;
	cs_csr->read_prefetch_enabled = 0;

	printk(KERN_ERR PFX "Mapping CR/CSR space\n");
	if (( rval = vme_find_mapping(cs_csr, 1)) != 0) {
        	 printk(KERN_ERR PFX "Failed to map CS_CSR (%d)\n", rval);
	         return -EINVAL;
	}
	printk(KERN_ERR PFX "CR/CSR mapping successful at 0x%p\n", cs_csr->kernel_va);

	return 0;
}

int unmap_cs_csr(void) {

	if (vme_release_mapping(cs_csr, 1)) {
		printk(KERN_ERR PFX "Unmap CR/CSR window failed\n");
		return -EINVAL;
	}
	printk(KERN_ERR PFX "CR/CSR window unmape\n");
	return 0;
}

#define BIT_SETRG (0x7FFFB)
#define BIT_CLEARRG (0x7FFF7)
#define WB64 0
#define WB32 1
#define RESET_CORE 0x80
#define ENABLE 0x10

int setup_csr_fa0 (void)
{
	/* Check if CS/CSR window is mapped */
	if (cs_csr == NULL) {
		printk(KERN_ERR PFX "CS/CSR window not found\n");
		return -EINVAL;
	}

	iowrite8(RESET_CORE, cs_csr->kernel_va + BIT_SETRG);
	/*
	iowrite8(ENABLE, cs_cst->kernel_va + BIT_CLEARRG);
	iowrite
	*/
	return 0;
}

int svec_load_fpga(struct svec_dev *svec, const void *blob, int size)
{

	const uint32_t boot_seq[8] = {0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe};
	const uint32_t *data = blob;
	const char svec_idr[4] = "SVEC";
	void *loader_addr; /* FPGA loader virtual address */
	uint32_t idc;
    	char id_string[5]; /* Null terminated string. Should be 'SVEC' */
	uint32_t n;
	uint32_t rval;
	int xldr_fifo_r0;  /* Bitstream data input control register */
	int xldr_fifo_r1;  /* Bitstream data input register */
	int i;
	int err = 0;

	/* Check if we have something to do... */
	if ((data == NULL) || (size == 0)){
		printk(KERN_ERR PFX "%s: data to be load is NULL\n", __func__);
		return -EINVAL;
	}

	/* Check if CS/CSR window is mapped */
	if (cs_csr == NULL) {
		printk(KERN_ERR PFX "CS/CSR window not found\n");
		return -EINVAL;
	}
	loader_addr = cs_csr->kernel_va + BASE_LOADER;

	/* Magic sequence: unlock bootloader mode, disable application FPGA */
	for (i=0; i<8; i++)
		iowrite32(swapbe32(boot_seq[i]), loader_addr + XLDR_REG_BTRIGR);	
	printk(KERN_ERR PFX "wrote unlock sequence at %x\n", 
			(unsigned int)loader_addr + XLDR_REG_BTRIGR);

	/* Check if we are really talking to a SVEC */
	/* Looking for 'SVEC' string */
	idc = swapbe32(ioread32(loader_addr + XLDR_REG_IDR));
	id_string[0] = (idc >> 24) & 0xff;
	id_string[1] = (idc >> 16) & 0xff;
	id_string[2] = (idc >> 8) & 0xff;
	id_string[3] = (idc >> 0) & 0xff;
	id_string[4] = 0;
    
	if(strncmp(id_string, svec_idr, 4))
	{
	    printk(KERN_ERR PFX "Invalid IDCode value %d [%s].\n", idc, &id_string[0]);
	    return -EINVAL;
	}
	printk(KERN_INFO PFX "IDCode value %d [%s].\n", idc, &id_string[0]);

	iowrite32(swapbe32(XLDR_CSR_SWRST), loader_addr + XLDR_REG_CSR); 
	iowrite32(swapbe32(XLDR_CSR_START | XLDR_CSR_MSBF), loader_addr + XLDR_REG_CSR); 

	while(i < size) {
		rval = swapbe32(ioread32(loader_addr + XLDR_REG_FIFO_CSR));
		if (!(rval & XLDR_FIFO_CSR_FULL)) {
			n = (size-i>4?4:size-i);
			xldr_fifo_r0 = (n - 1) | ((n<4) ? XLDR_FIFO_R0_XLAST : 0);
			xldr_fifo_r1 = htonl(data[i>>2]); 

			iowrite32(swapbe32(xldr_fifo_r0), loader_addr + XLDR_REG_FIFO_R0);
			iowrite32(swapbe32(xldr_fifo_r1), loader_addr + XLDR_REG_FIFO_R1);
    			i+=n;
		}
	}

	while(1) 
	{
		rval = swapbe32(ioread32(loader_addr + XLDR_REG_CSR));
		if(rval & XLDR_CSR_DONE) {
			err = rval & XLDR_CSR_ERROR ? -EINVAL : 0;
    	    		printk(KERN_ERR PFX "Bitstream loaded, status: %s\n", 
				(err < 0 ? "ERROR" : "OK"));

			/* give the VME bus control to App FPGA */
			iowrite32(swapbe32(XLDR_CSR_EXIT), loader_addr + XLDR_REG_CSR);
			break;
		}
	}

	return err;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	printk(KERN_ERR PFX "%s\n", __func__);
	
	/* struct svec_dev *card = dev_get_drvdata(pdev); 
	int ret;
	dev_t devno = MKDEV(MAJOR(svec_devno), 0);

	ret = vme_free_irq(vector[ndev]);
	if (ret)
		dev_warn(pdev, "Cannot free irq %d, err %d\n", vector[ndev], ret);
	*/
	/* avoid deleting unregistered devices */
	/*
	if (card) {
		if (card->cdev.owner == THIS_MODULE) {
			printk(KERN_ERR PFX "unregister_chrdev_region\n");
			unregister_chrdev_region(devno, lun_num);
		}
	}
	else
		printk(KERN_ERR PFX "card is null!\n");
	printk(KERN_ERR PFX "class_destroy\n");
	class_destroy(svec_class);
	*/
	

	return 0;
}
/*
static int svec_irq(void *arg)
{
	return 0;
}
*/
static int __devinit svec_match(struct device *pdev, unsigned int ndev)
{
	printk(KERN_ERR PFX "%s\n", __func__);
	/* TODO */

	if (ndev >= vmebase1_num)
        	 return 0;
	if (ndev >= vector_num || ndev >= level_num) {
        	 dev_warn(pdev, "irq vector/level missing\n");
	         return 0;
	}

	return 1;
}

int svec_load_fpga_file(struct svec_dev *svec, char *name)
{
	struct device *dev = svec->dev;
	const struct firmware *fw;
	int err = 0;

	dev_info(dev, "%s: file %s\n", __func__, name);

	if (name == NULL) {
		printk (KERN_ERR PFX "%s: file name is NULL\n", __func__);
	}

	err = request_firmware(&fw, name, dev);

	if (err < 0) {
        	dev_err(dev, "request firmware \"%s\": error %i\n", name, err);
	        return err;
	}
	printk (KERN_ERR PFX "got file \"%s\", %i (0x%x) bytes\n",
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
		printk(KERN_ERR PFX "Card lun %d out of range [0..%d]\n",
			lun[ndev], SVEC_MAX_DEVICES -1);
		return -EINVAL;
	}

	printk (KERN_ERR PFX "probe for device %02d\n", ndev);

	svec = kzalloc(sizeof(*svec), GFP_KERNEL);
	if (svec == NULL) {
		printk(KERN_ERR PFX "Cannot allocate memory for svec card struct\n");
		return -ENOMEM;
	}
	
	/* Initialize struct fields*/
	svec->lun = lun[ndev];
	svec->vmebase1 = vmebase1[ndev];
	svec->vmebase2 = vmebase2[ndev];
	svec->vector = vector[ndev];
	svec->level = level[ndev];
	svec->fw_name = fw_name[ndev];
	svec->dev = pdev;

	/* Map CS/CSR space */
	error = map_cs_csr(svec);
	if (error)
		goto failed;

	/* Load the golden FPGA binary to read the eeprom */
	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		goto failed;

	error = setup_csr_fa0();

	
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
        name = dev_name(pdev);
#endif
	strlcpy(svec->driver, DRIVER_NAME, sizeof(svec->driver));
	snprintf(svec->description, sizeof(svec->description),
		"SVEC at VME-A32 0x%08lx - 0x%08lx irqv %d irql %d",
		svec->vmebase1, svec->vmebase2, vector[ndev], level[ndev]);

	printk (KERN_ERR PFX "%s\n", svec->description);

	/*Create cdev, just to know the carrier is there */
	devno = MKDEV(MAJOR(svec_devno), ndev);

	cdev_init(&svec->cdev, &svec_fops);
	svec->cdev.owner = THIS_MODULE;
	error = cdev_add(&svec->cdev, devno, 1);
	if (error) {
		dev_err(pdev, "Error %d adding cdev %d\n", error, ndev);
		goto failed;
	}
/*
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        svec->dev = device_create(svec_class, pdev, devno, NULL, "svec.%i", ndev);
#else
        svec->dev = device_create(svec_class, pdev, devno, "svec.%i", ndev);
#endif 

	if (IS_ERR(pdev)) {
		error = PTR_ERR(svec->dev);
		dev_err(pdev, "Error %d creating device %d\n", error, ndev);
		svec->dev = NULL;
		goto device_create_failed;
	}
*/
	if (!error) {
		dev_set_drvdata(svec->dev, svec);
		goto out;
	}
	
	unmap_cs_csr();

/*
device_create_failed:
	cdev_del(&svec->cdev);
 */

failed:
	kfree(svec);
	return -EINVAL;
out:
	return error;
}

static struct vme_driver svec_driver = {
        .match          = svec_match,
        .probe          = svec_probe,
        .remove         = __devexit_p(svec_remove),
        .driver         = {
       		.name   = DRIVER_NAME,
        },
};

static int __init svec_init(void)
{
	int error = 0;

	/* TODO: Check parameters */

	/* Device creation for the carrier, just in case... */
	error = alloc_chrdev_region(&svec_devno, 0, lun_num, "svec");
	if (error) {
		printk(KERN_ERR PFX "Failed to allocate chrdev region\n");
		goto out;
	}

	/*	
	svec_class = class_create(THIS_MODULE, "svec");
	if (IS_ERR(svec_class)) {
		printk(KERN_ERR PFX "Failed to create svec class\n");
		error = PTR_ERR(svec_class);
		goto out;
	}
        */
	error = vme_register_driver(&svec_driver, lun_num);
	if (error) {
		printk(KERN_ERR PFX "Cannot register vme driver - lun [%d]\n", lun_num);
		class_destroy(svec_class);
	}

out:
	return error;
}

static void __exit svec_exit(void)
{
	printk(KERN_ERR PFX "%s\n", __func__);
        vme_unregister_driver(&svec_driver);
}


module_init(svec_init);
module_exit(svec_exit);

MODULE_AUTHOR("");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec driver");
