
VME_INC ?= /acc/src/dsc/drivers/coht/vmebridge/include/ 

CFLAGS += -g -Wall
CFLAGS += -I $(VME_INC)
LDFLAGS += -L /acc/local/L865/lib 
LDLIBS += -lvmebus

all: vme
vme: vme.o

clean:
	rm -f *.o vme

# These targets are called from ../Makefile, just do nothing
modules install modules_install:

.PHONY: all modules clean install modules_install
