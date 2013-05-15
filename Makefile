FMC_DRV ?= $(shell ./check-fmc-bus)
export FMC_DRV

RUNME := $(shell test -d $(FMC_DRV) || git submodule update --init)

DIRS = $(FMC_DRV) kernel tools

all clean modules install modules_install:
	for d in $(DIRS); do $(MAKE) -C $$d $@ || exit 1; done
