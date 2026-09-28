obj-m += ahci_cmd_builder.o
obj-m += ahci_decoder.o
obj-m += ahci_dump.o

PWD := $(shell pwd)
KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean