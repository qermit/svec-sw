# Assign sensible defaults, but allow overriding with environment variables.
CPU	?= L865
KVER	?= 2.6.24.7-rt27
KVER	?= 3.2.33-rt50
LINUX ?= "/acc/sys/$(CPU)/usr/src/kernels/$(KVER)"

FMC_DRV ?= $(shell ../check-fmc-bus)
export FMC_DRV

ccflags-y += -I$(FMC_DRV)/include
ccflags-y += -I/acc/dsc/src/drivers/coht/vmebridge/include
ccflags-y += -DDEBUG

KBUILD_EXTRA_SYMBOLS += $(M)/Module.symvers.vme
KBUILD_EXTRA_SYMBOLS += $(M)/Module.symvers.fmc

obj-m := svec.o

svec-objs := svec-drv.o
svec-objs += svec-sysfs.o
svec-objs += svec-fmc.o
svec-objs += svec-i2c.o

all: modules

.PHONY: all modules clean help install modules_install

modules help install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell pwd) CPU=$(CPU) KVER=$(KVER) $@

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
                         Module.markers modules.order
