obj-m += stealth_framework.o
stealth_framework-objs := main.o mmio_audit.o ahci_engine.o apic_stealth.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean