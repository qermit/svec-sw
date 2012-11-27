CFLAGS += -g -Wall
CFLAGS += -I /acc/src/dsc/drivers/coht/vmebridge/include/ 
LDFLAGS += -L /acc/local/L865/lib 
LDLIBS += -lvmebus

all: vme
vme: vme.o

clean:
	rm -f *.o vme
