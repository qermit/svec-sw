/*
* Copyright (C) 2013 CERN (www.cern.ch)
* Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
* VIC (Vectored Interrupt Controller) support code. 
*/

#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/fmc.h>
#include <linux/fmc-sdb.h>

#include "svec.h"

#include "hw/vic_regs.h"

#define VIC_MAX_VECTORS 32

#define VIC_SDB_VENDOR 0xce42
#define VIC_SDB_DEVICE 0x0013

/* A Vectored Interrupt Controller object */
struct vic_irq_controller {
	/* already-initialized flag */
	int initialized;
	/* Base address (FPGA-relative) */
	uint32_t base;
	/* Mapped base address of the VIC */
	void *kernel_va;

	/* Vector table */
	struct vector {
		/* Saved ID of the vector (for autodetection purposes) */
		int saved_id;
		/* Pointer to the assigned handler */
		irq_handler_t handler;
		/* FMC device that owns the interrupt */
		struct fmc_device *requestor;
	} vectors[VIC_MAX_VECTORS];
};

static inline void vic_writel(struct vic_irq_controller *vic, uint32_t value,
			      uint32_t offset)
{
	iowrite32be(value, vic->kernel_va + offset);
}

static inline uint32_t vic_readl(struct vic_irq_controller *vic,
				 uint32_t offset)
{
	return ioread32be(vic->kernel_va + offset);
}

static int svec_vic_init(struct svec_dev *svec, struct fmc_device *fmc)
{
	int i;
	signed long vic_base;
	struct vic_irq_controller *vic;

	/* Try to look up the VIC in the SDB tree - note that IRQs shall be requested after the
	   FMC driver has scanned the SDB tree */
	vic_base =
	    fmc_find_sdb_device(fmc->sdb, VIC_SDB_VENDOR, VIC_SDB_DEVICE, NULL);

	if (vic_base < 0) {
		dev_err(svec->dev,
			"VIC controller not found, but a VIC interrupt requested. Wrong gateware?\n");
		return -ENODEV;
	}

	if(svec->verbose)
	dev_info(svec->dev, "Found VIC @ 0x%lx\n", vic_base);

	vic = kzalloc(sizeof(struct vic_irq_controller), GFP_KERNEL);
	if (!vic)
		return -ENOMEM;

	vic->kernel_va = svec->map[MAP_REG]->kernel_va + vic_base;
	vic->base = (uint32_t) vic_base;

	/* disable all IRQs, copy the vector table with pre-defined IRQ ids */
	vic_writel(vic, 0xffffffff, VIC_REG_IDR);
	for (i = 0; i < VIC_MAX_VECTORS; i++)
		vic->vectors[i].saved_id =
		    vic_readl(vic, VIC_IVT_RAM_BASE + 4 * i);

	/* configure the VIC output: active high, edge sensitive, pulse width = 1 tick (16 ns) */
	vic_writel(vic,
		   VIC_CTL_ENABLE | VIC_CTL_POL | VIC_CTL_EMU_EDGE |
		   VIC_CTL_EMU_LEN_W(40000), VIC_REG_CTL); /* 160 us IRQ retry timer */

	vic->initialized = 1;
	svec->vic = vic;

	return 0;
}

void svec_vic_cleanup(struct svec_dev *svec)
{
	if (!svec->vic)
		return;

	/* Disable all irq lines and the VIC in general */
	vic_writel(svec->vic, 0xffffffff, VIC_REG_IDR);
	vic_writel(svec->vic, 0, VIC_REG_CTL);
	kfree(svec->vic);
	svec->vic = NULL;
}

irqreturn_t svec_vic_irq_dispatch(struct svec_dev * svec)
{
	struct vic_irq_controller *vic = svec->vic;
	int index, rv;
	struct vector *vec;

	do {
		/* Our parent IRQ handler: read the index value from the Vector Address Register,
		   and find matching handler */
		index = vic_readl(vic, VIC_REG_VAR) & 0xff;

		if (index >= VIC_MAX_VECTORS)
			goto fail;

		vec = &vic->vectors[index];
		if (!vec->handler)
			goto fail;

		rv = vec->handler(vec->saved_id, vec->requestor);
		    
		vic_writel(vic, 0, VIC_REG_EOIR);	/* ack the irq */

		if(rv < 0)
		    break;

	/* check if any IRQ is still pending */
	} while (vic_readl(vic, VIC_REG_RISR));
	
	return rv;

      fail:
	return 0;
}

int svec_vic_irq_request(struct svec_dev *svec, struct fmc_device *fmc,
			 unsigned long id, irq_handler_t handler)
{
	struct vic_irq_controller *vic;
	int rv = 0, i;

	/* First interrupt to be requested? Look up and init the VIC */
	if (!svec->vic) {
		rv = svec_vic_init(svec, fmc);
		if (rv)
			return rv;
	}

	vic = svec->vic;

	for (i = 0; i < VIC_MAX_VECTORS; i++) {
		/* find the vector in the stored table, assign handler and enable the line if exists */
		if (vic->vectors[i].saved_id == id) {
			spin_lock(&svec->irq_lock);

			vic_writel(vic, i, VIC_IVT_RAM_BASE + 4 * i);
			vic->vectors[i].requestor = fmc;
			vic->vectors[i].handler = handler;
			vic_writel(vic, (1 << i), VIC_REG_IER);

			spin_unlock(&svec->irq_lock);

			return 0;

		}
	}

	return -EINVAL;

}

int svec_vic_irq_free(struct svec_dev *svec, unsigned long id)
{
	int i;

	for (i = 0; i < VIC_MAX_VECTORS; i++) {
		uint32_t vec = svec->vic->vectors[i].saved_id;
		if (vec == id) {
			spin_lock(&svec->irq_lock);

			vic_writel(svec->vic, 1 << i, VIC_REG_IDR);
			vic_writel(svec->vic, vec, VIC_IVT_RAM_BASE + 4 * i);
			svec->vic->vectors[i].handler = NULL;

			spin_unlock(&svec->irq_lock);
		}
	}

	return 0;
}

void svec_vic_irq_ack(struct svec_dev *svec, unsigned long id)
{
	vic_writel(svec->vic, 0, VIC_REG_EOIR);
}
