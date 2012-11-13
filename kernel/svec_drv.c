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
#define US_TICKS 100
#define TIMEOUT 1000

struct svec_dev {
	int 	                lun;
	unsigned long		vmebase1;
	unsigned long		vmebase2;
	int 			vector;
	int			level;
	char			*fw_name;
        struct device           *dev;
	struct cdev		cdev;
	char 			driver[16];
	char			description[80];
};

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

int svec_load_fpga(struct svec_dev *svec, uint32_t *data, int size)
{

	const uint32_t boot_seq[8] = {0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe};
	const char svec_idr[4] = "SVEC";
	int i;
    	char idr[5];
    	struct vme_mapping mapping;
	void *vaddr;
	uint32_t idc;
	uint32_t n;
	uint32_t rval;
	void *addr;
	int timeout;

	/* CS/CSR mapping*/
	memset(&mapping, 0, sizeof(mapping));
	mapping.am = 		VME_CR_CSR; /* 0x2f */
	mapping.data_width = 	VME_D32;
	mapping.vme_addru =	0;
	mapping.vme_addrl =	svec->vmebase1;
	mapping.sizeu =		0;
	mapping.sizel = 	0x80000;
	mapping.window_num =	0;
	mapping.bcast_select = 0;
	mapping.read_prefetch_enabled = 0;

	printk(KERN_ERR PFX "Mapping CR/CSR space\n");
	if (( rval = vme_find_mapping(&mapping, 1)) != 0) {
        	 printk(KERN_ERR PFX "Failed to map CS_CSR (%d)\n", rval);
	         return -EINVAL;
	}
	vaddr = mapping.kernel_va;

	printk(KERN_ERR PFX "Mapping successful at 0x%p\n", mapping.kernel_va);
	
	/* Magic sequence: unlock bootloader mode, disable application FPGA */
	for (i=0; i<8; i++)
		iowrite32(swapbe32(boot_seq[i]), vaddr + BASE_LOADER + XLDR_REG_BTRIGR);	
	printk(KERN_ERR PFX "wrote unlock sequence at %x\n", 
			(unsigned int)vaddr + BASE_LOADER + XLDR_REG_BTRIGR);

	/* check if we are really talking to a SVEC */
	addr = vaddr + BASE_LOADER + XLDR_REG_IDR;
	/*
	printk(KERN_ERR PFX "reading at 0x%p:0x%p\n", vaddr, addr);
	printk(KERN_ERR PFX "C: 0x%08x\n", ioread32be(vaddr + 0x1f));
	printk(KERN_ERR PFX "R: 0x%08x\n", ioread32be(vaddr + 0x23));
	*/

	/* Looking for 'SVEC' string */
	idc = swapbe32(ioread32(addr));
	idr[0] = (idc >> 24) & 0xff;
	idr[1] = (idc >> 16) & 0xff;
	idr[2] = (idc >> 8) & 0xff;
	idr[3] = (idc >> 0) & 0xff;
	idr[4] = 0;
    
	if(strncmp(idr, svec_idr, 4))
	{
	    printk(KERN_ERR PFX "Invalid IDCode value %d [%s].\n", idc, &idr[0]);
	    return -EINVAL;
	}

	printk(KERN_INFO PFX "IDCode value %d [%s].\n", idc, &idr[0]);

	if (data == NULL) {
		printk(KERN_ERR PFX "firmware data is null\n");
		return 0;
	}

	iowrite32(swapbe32(XLDR_CSR_SWRST), vaddr + BASE_LOADER + XLDR_REG_CSR); 
	iowrite32(swapbe32(XLDR_CSR_START | XLDR_CSR_MSBF), vaddr + BASE_LOADER + XLDR_REG_CSR); 

	timeout = 0;
	while(i < size) {
		rval = swapbe32(ioread32(vaddr + XLDR_REG_FIFO_CSR + XLDR_FIFO_CSR_FULL));
		if (rval & XLDR_FIFO_CSR_FULL) {
			if (timeout++ > TIMEOUT) {
				printk(KERN_ERR PFX "timeout on XLDR_FIFO_CSR_FULL\n");
				return -ETIME;
			}
			usleep_range(1, US_TICKS);
			continue;
		}
		printk(KERN_ERR PFX "pass #1\n");
		n = (size -i) > 4 ? 3 : size -i - 1;
		if (n < 3) 			
			n |= XLDR_FIFO_R0_XLAST;

		iowrite32(swapbe32(n), vaddr + BASE_LOADER + XLDR_REG_FIFO_R0);
		iowrite32(swapbe32(htonl(data[i>>2])), vaddr + BASE_LOADER + XLDR_REG_FIFO_R1);
    		i+=n;
	}
	printk(KERN_ERR PFX "pass #2\n");
	
	while(1) 
	{
		rval = swapbe32(ioread32(vaddr + BASE_LOADER + XLDR_REG_CSR));
		if(rval & XLDR_CSR_DONE) {
    			printk(KERN_ERR PFX "Bitstream loaded, status: %s\n", 
						(rval & XLDR_CSR_ERROR ? "ERROR" : "OK"));
			/* give the VME bus control to App FPGA */
			iowrite32(swapbe32(XLDR_CSR_EXIT), vaddr + BASE_LOADER + XLDR_REG_CSR);
			vme_release_mapping(&mapping, 1);
			return rval & XLDR_CSR_ERROR ? -1 : 0;
		}
		if (rval & XLDR_CSR_ERROR) {
			printk(KERN_ERR PFX "Error loading bitstream\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int __devexit svec_remove(struct device *pdev, unsigned int ndev)
{
	printk("remove\n");
	
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
	printk (KERN_ERR PFX "match\n");
	/* TODO */

	if (ndev >= vmebase1_num)
        	 return 0;
	if (ndev >= vector_num || ndev >= level_num) {
        	 dev_warn(pdev, "irq vector/level missing\n");
	         return 0;
	}

	return 1;
}

int svec_load_fpga_file (struct svec_dev *svec, char *name)
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

	err = svec_load_fpga(svec, (uint32_t *)fw->data, 0);
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

	/* Load the golden FPGA binary to read the eeprom */
	error = svec_load_fpga_file(svec, svec->fw_name);
	if (error)
		goto failed;

	
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
	name = pdev->bus_id;
#else
        name = dev_name(pdev);
#endif
	/*
	error = vme_request_irq(vector[ndev], svec_irq, svec, name);
	if (error) {
		printk(KERN_ERR PFX "vme_request_irq failed %d\n", error);
		goto failed;
	}
	*/
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
        svec->dev = device_create(svec_class, pdev, devno, NULL, "svec.%i", ndev);
#else
        svec->dev = device_create(svec_class, pdev, devno, "svec.%i", ndev);
#endif /* 2.6.28 */

	if (IS_ERR(pdev)) {
		error = PTR_ERR(svec->dev);
		dev_err(pdev, "Error %d creating device %d\n", error, ndev);
		svec->dev = NULL;
		goto device_create_failed;
	}

	if (!error) {
		dev_set_drvdata(svec->dev, svec);
		goto out;
	}

device_create_failed:
	cdev_del(&svec->cdev);

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
	printk("svec_exit\n");
        vme_unregister_driver(&svec_driver);
}


module_init(svec_init);
module_exit(svec_exit);

MODULE_AUTHOR("");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("svec driver");
