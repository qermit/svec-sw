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
#ifndef __SVEC_H__
#define __SVEC_H__

#include <linux/cdev.h>
#include <linux/firmware.h>
#include <linux/fmc.h>
#include <vmebus.h>

#define SVEC_MAX_DEVICES        32
#define SVEC_DEFAULT_IDX { [0 ... (SVEC_MAX_DEVICES-1)] = -1 }
#define SVEC_IRQ_LEVEL	2
#define SVEC_BASE_LOADER	0x70000

/* The eeprom is at address 0x50 */
/* FIXME ? Copied from spec.h */
#define SVEC_I2C_EEPROM_SIZE (8 * 1024)

enum svec_map_win {
	MAP_CR_CSR = 0,	/* CR/CSR */
	MAP_REG		/* A32 space */
};

/* Our device structure */
struct svec_dev {
	int 	                lun;
	unsigned long		vmebase1;
	unsigned long		vmebase2;
	int 			vector;
	int			level;

	char			*submod_name;
	char			*fw_name;
	struct device           *dev;
	struct cdev		cdev;
	char 			driver[16];
	char			description[80];

	struct vme_mapping 	*map[2];
	/* FIXME: Workaround to avoid reprogram on second fd */
	int			already_reprogrammed;

	/* struct work_struct	work; */
	const struct firmware	*fw;
	struct list_head	list;
	unsigned long		irqcount;
	void			*sub_priv;
	struct fmc_device	*fmcs;
	int			slot_n;
	int			irq_count;	/* for mezzanine use too */
	struct completion	compl;
	struct gpio_chip	*gpio;
};

/* Functions and data in svec-vme.c */
extern int svec_bootloader_is_active(struct svec_dev *svec);
extern int svec_bootloader_unlock (struct svec_dev *svec);
extern int svec_load_fpga(struct svec_dev *svec, const void *data, int size);
extern int svec_load_fpga_file(struct svec_dev *svec, const char *name);
extern void svec_setup_csr_fa0(void *base, u32 vme, unsigned vector, unsigned level);
extern int svec_unmap_window(struct svec_dev *svec, enum svec_map_win map_type);
extern int svec_map_window( struct svec_dev *svec, enum svec_map_win map_type);

extern char *svec_fw_name;
extern int spec_use_msi;

/* Functions in svec-fmc.c, used by svec-vme.c */
extern int svec_fmc_create(struct svec_dev *svec);
extern void svec_fmc_destroy(struct svec_dev *svec);
extern void svec_fmc_check(struct svec_dev *svec, unsigned int n);

/* Functions in svec-i2c.c, used by svec-fmc.c */
extern int svec_i2c_init(struct fmc_device *fmc, unsigned int n);
extern void svec_i2c_exit(struct fmc_device *fmc);
extern int svec_eeprom_read(struct fmc_device *fmc, int i2c_addr,
			    uint32_t offset, void *buf, size_t size);
extern int svec_eeprom_write(struct fmc_device *fmc, int i2c_addr,
			     uint32_t offset, const void *buf, size_t size);

/* SVEC CSR offsets */
#define FUN0ADER 	0x7FF63
#define INT_LEVEL	0x7ff5b
#define INTVECTOR	0x7ff5f
#define WB_32_64	0x7ff33
#define BIT_SET_REG	0x7FFFB
#define BIT_CLR_REG	0x7FFF7
#define WB32		1
#define WB64		0
#define RESET_CORE	0x80
#define ENABLE_CORE	0x10

/* Functions in svec-gpio.c */
extern int svec_gpio_init(struct fmc_device *fmc);
extern void svec_gpio_exit(struct fmc_device *fmc);

/* Functions in svec-sysfs.c */
extern int svec_create_sysfs_files (struct svec_dev *card);
extern void svec_remove_sysfs_files (struct svec_dev *card);

#endif /* __SVEC_H__ */
