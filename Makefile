obj-m += stealth_dco.o
stealth_dco-y := main.o mmio_audit.o ahci_engine.o apic_stealth.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean