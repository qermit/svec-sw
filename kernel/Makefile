CPU	:= L865
KVER	:= 3.2.33-rt50
KERN_DIR:= "/acc/sys/$(CPU)/usr/src/kernels/$(KVER)"
VME_DIR := "../../repo/vmebridge/driver"
#VME_OBJDIR := "../../repo/vmebridge/driver/$(CPU)/$(KVER)"
VME_OBJDIR := "../../repo/vmebridge/driver/"

ccflags-y += -DDEBUG

all: modules
	$(MAKE) -C $(KERN_DIR) M=$(shell /bin/pwd)
.PHONY: all clean help

modules:
	$(MAKE) -C $(KERN_DIR) M=$(shell pwd) CPU=$(CPU) KVER=$(KVER) 

clean:
	$(MAKE) -C $(KERN_DIR) M=`pwd` clean

help:
	$(MAKE) -C $(KERN_DIR) M=`pwd` help
