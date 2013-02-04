# Assign sensible defaults, but allow overriding with environment variables.
CPU	?= L865
KVER	?= 2.6.24.7-rt27
KVER	?= 3.2.33-rt50
LINUX ?= "/acc/sys/$(CPU)/usr/src/kernels/$(KVER)"

ccflags-y += -I $(M)/../include
ccflags-y += -I $(M)/fmc-bus/kernel/include
ccflags-y += -DDEBUG

KBUILD_EXTRA_SYMBOLS += $(M)/Module.symvers.vme
KBUILD_EXTRA_SYMBOLS += $(M)/Module.symvers.fmc

obj-m := svec.o

svec-objs := svec_drv.o
svec-objs += svec-sysfs.o
svec-objs += svec-fmc.o
svec-objs += svec-i2c.o

all: modules

.PHONY: all modules clean help install modules_install

modules clean help install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell pwd) CPU=$(CPU) KVER=$(KVER) $@
