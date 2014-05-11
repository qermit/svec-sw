/*
* Copyright (C) 2012-2013 CERN (www.cern.ch)
* Author: Juan David Gonzalez Cobas <dcobas@cern.ch>
* Author: Luis Fernando Ruiz Gago <lfruiz@cern.ch>
*
* Released according to the GNU GPL, version 2 or any later version
*
* Driver for SVEC (Simple VME FMC carrier) board.
*/
#ifndef __SVEC_H__
#define __SVEC_H__

#include <linux/firmware.h>
#include <linux/fmc.h>
#include "vmebus.h"

#define SVEC_MAX_DEVICES        32
#define SVEC_MAX_FIRMWARE_SIZE  0x400000

#define SVEC_DEFAULT_IDX { [0 ... (SVEC_MAX_DEVICES-1)] = -1 }
#define SVEC_DEFAULT_VME_AM  { [0 ... (SVEC_MAX_DEVICES-1)] = 0x39 }
#define SVEC_DEFAULT_VME_SIZE  { [0 ... (SVEC_MAX_DEVICES-1)] = 0x80000 }
#define SVEC_DEFAULT_IRQ_LEVEL { [0 ... (SVEC_MAX_DEVICES-1)] = 0x2 }

#define SVEC_UNINITIALIZED_VME_BASE  { [0 ... (SVEC_MAX_DEVICES-1)] = 0xffffffff }
#define SVEC_UNINITIALIZED_IRQ_VECTOR { [0 ... (SVEC_MAX_DEVICES-1)] = -1 }

#define SVEC_IRQ_LEVEL	2
#define SVEC_N_SLOTS	2
#define SVEC_BASE_LOADER	0x70000
#define SVEC_VENDOR_ID		0x80030

#define VME_VENDOR_ID_OFFSET	0x24

/* The eeprom is at address 0x50 */
/* FIXME ? Copied from spec.h */
#define SVEC_I2C_EEPROM_SIZE (8 * 1024)

#define SVEC_MAX_GATEWARE_SIZE 0x420000

enum svec_map_win {
	MAP_CR_CSR = 0,		/* CR/CSR */
	MAP_REG			/* A32/A24/A16 space */
};

struct svec_config {
	int configured;
	uint32_t vme_base;
	int vme_am;
	uint32_t vme_size;
	uint32_t vic_base;
	int interrupt_vector;
	int interrupt_level;
	int use_vic;
	int use_fmc;
};

#define SVEC_FLAG_FMCS_REGISTERED 	0
#define SVEC_FLAG_IRQS_REQUESTED  	1
#define SVEC_FLAG_BOOTLOADER_ACTIVE 	2
#define SVEC_FLAG_AFPGA_PROGRAMMED	3

/* Our device structure */
struct svec_dev {
	int lun;
	int slot;
	unsigned long flags;
	struct device *dev;
	char name[16];
	char driver[16];
	char description[80];
	struct vme_mapping *map[2];
	struct svec_config cfg_cur, cfg_new;

	struct fmc_device *fmcs[SVEC_N_SLOTS];
	irq_handler_t fmc_handlers[SVEC_N_SLOTS];

	/* FMC devices */
	int fmcs_n;		/* Number of FMC devices */
	unsigned long irq_count;	/* for mezzanine use too */
	unsigned int current_vector;
	spinlock_t irq_lock;

	struct vic_irq_controller *vic;
	uint32_t vme_raw_addr;	/* VME address for raw VME I/O through vme_addr/vme_data attributes */
	int verbose;

	char *fw_name;
	uint32_t fw_hash;
	void *fw_buffer;
	int fw_length;
};

/* Functions and data in svec-vme.c */
extern int svec_is_bootloader_active(struct svec_dev *svec);
extern int svec_bootloader_unlock(struct svec_dev *svec);
extern int svec_load_fpga(struct svec_dev *svec, const void *data, int size);
extern int svec_load_fpga_file(struct svec_dev *svec, const char *name);
extern void svec_setup_csr_fa0(struct svec_dev *svec);
extern int svec_unmap_window(struct svec_dev *svec, enum svec_map_win map_type);
extern int svec_map_window(struct svec_dev *svec, enum svec_map_win map_type);

extern char *svec_fw_name;

/* Functions in svec-fmc.c, used by svec-vme.c */
extern int svec_fmc_create(struct svec_dev *svec);
extern void svec_fmc_destroy(struct svec_dev *svec);

/* Functions in svec-i2c.c, used by svec-fmc.c */
extern int svec_i2c_init(struct fmc_device *fmc);
extern void svec_i2c_exit(struct fmc_device *fmc);
extern int svec_eeprom_read(struct fmc_device *fmc, uint32_t offset,
			    void *buf, size_t size);
extern int svec_eeprom_write(struct fmc_device *fmc, uint32_t offset,
			     const void *buf, size_t size);

/* SVEC CSR offsets */
#define FUN0ADER	0x7FF63
#define FUN1ADER	0x7FF73
#define INT_LEVEL	0x7ff5b
#define INTVECTOR	0x7ff5f
#define WB_32_64	0x7ff33
#define BIT_SET_REG	0x7FFFB
#define BIT_CLR_REG	0x7FFF7
#define WB32		1
#define WB64		0
#define RESET_CORE	0x80
#define ENABLE_CORE	0x10

/* Functions in svec-sysfs.c */
extern int svec_create_sysfs_files(struct svec_dev *card);
extern void svec_remove_sysfs_files(struct svec_dev *card);

int svec_dma_write(struct svec_dev *svec, uint32_t addr, int am, size_t size,
		   void *buf, int is_fifo);
int svec_dma_read(struct svec_dev *svec, uint32_t addr, int am, size_t size,
		  void *buf, int is_fifo);


int svec_reconfigure(struct svec_dev *svec);
int svec_setup_csr(struct svec_dev *svec);

int svec_validate_configuration(struct device *pdev, struct svec_config *cfg);
int svec_load_golden(struct svec_dev *svec);

/* VIC interrupt controller stuff */
irqreturn_t svec_vic_irq_dispatch(struct svec_dev *svec);
int svec_vic_irq_request(struct svec_dev *svec, struct fmc_device *fmc, unsigned long id, irq_handler_t handler);
int svec_vic_irq_free(struct svec_dev *svec, unsigned long id);
void svec_vic_irq_ack(struct svec_dev *svec, unsigned long id);
void svec_vic_cleanup(struct svec_dev *svec);

/* Generic IRQ routines */

int svec_irq_request(struct fmc_device *fmc, irq_handler_t handler, char *name,
		     int flags);
void svec_irq_ack(struct fmc_device *fmc);
int svec_irq_free(struct fmc_device *fmc);
void svec_irq_exit(struct svec_dev *svec);

#endif /* __SVEC_H__ */

