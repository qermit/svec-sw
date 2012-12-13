CPU	:= L865
# KVER	:= 2.6.24.7-rt27
KVER	:= 3.2.33-rt50
KERN_DIR:= "/acc/sys/$(CPU)/usr/src/kernels/$(KVER)"
VME_DIR := "../../repo/vmebridge/driver"
#VME_OBJDIR := "../../repo/vmebridge/driver/$(CPU)/$(KVER)"
VME_OBJDIR := "../../repo/vmebridge/driver/"

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
	$(MAKE) -C $(KERN_DIR) M=$(shell /bin/pwd)
.PHONY: all clean help

modules:
	$(MAKE) -C $(KERN_DIR) M=$(shell pwd) CPU=$(CPU) KVER=$(KVER) 

clean:
	$(MAKE) -C $(KERN_DIR) M=`pwd` clean

help:
	$(MAKE) -C $(KERN_DIR) M=`pwd` help
