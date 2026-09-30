obj-m += stealth_dco.o
stealth_dco-objs := main.o mmio_audit.o ahci_engine.o

PWD := $(shell pwd)

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean