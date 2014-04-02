GW_GOLDEN = svec-golden-v2.0-20140306.bin

GW_URL = http://www.ohwr.org/attachments/download/2785/$(GW_GOLDEN)

FIRMWARE_PATH ?= /lib/firmware/fmc

gateware_install:	bin/$(GW_GOLDEN)
	install -D bin/$(GW_GOLDEN) $(FIRMWARE_PATH)/$(GW_GOLDEN)
	ln -sf $(GW_GOLDEN) $(FIRMWARE_PATH)/svec-golden.bin

bin/$(GW_GOLDEN):
	wget $(GW_URL) -P bin
