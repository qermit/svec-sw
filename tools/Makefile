ENV ?= lab
CPU ?= L865

include /acc/src/dsc/co/Make.auto

VME_INC ?= /acc/src/dsc/drivers/coht/vmebridge/include/

CFLAGS += -g -Wall
CFLAGS += -I $(VME_INC)
LDFLAGS += -L /acc/local/$(CPU)/lib
LDLIBS += -lvmebus

all: vme.$(CPU)
vme.$(CPU): vme.$(CPU).o

install-$(ENV):
	dsc_install vme.$(CPU) /acc/dsc/$(ENV)/$(CPU)/bin

clean:
	rm -f *.o *.$(CPU)

# These targets are called from ../Makefile, just do nothing
modules install modules_install:

.PHONY: all modules clean install modules_install
