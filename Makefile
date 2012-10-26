LINUX ?= /acc/sys/L865/usr/src/kernels/2.6.24.7-rt27/

ccflags-y += -I /acc/src/dsc/drivers/coht/vmebridge/include/ -DDEBUG
obj-m += svec.o

all: modules

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)

modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) $@

clean:
	rm -rf *.o *.ko  *.mod.c
	rm -rf .*.o.cmd .*.ko.cmd  *.mod.c
	rm -rf .tmp_versions modules.order
