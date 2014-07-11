LINUX ?= /lib/modules/$(shell uname -r)/build

FMC_DRV ?= $(shell ../check-fmc-bus)
export FMC_DRV

GIT_VERSION = $(shell git describe --dirty --long --tags)

ccflags-y += -I$(FMC_DRV)/include
ccflags-y += -DDEBUG
ccflags-y += -DGIT_VERSION=\"$(GIT_VERSION)\"

KBUILD_EXTRA_SYMBOLS += $(obj)/../fmc-bus/kernel/Module.symvers
KBUILD_EXTRA_SYMBOLS += /home/qtest/ohwr/coht/vmebridge/driver/Module.symvers

obj-m := svec.o

svec-objs := svec-drv.o
svec-objs += svec-sysfs.o
svec-objs += svec-fmc.o
svec-objs += svec-i2c.o
svec-objs += svec-irq.o
svec-objs += svec-vic.o

all: modules

.PHONY: all modules clean help install modules_install

modules help install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell pwd) GIT_VERSION=$(GIT_VERSION) $@

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
                         Module.markers modules.order
