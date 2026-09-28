sudo dmesg -C && \
sudo insmod ahci_dump.ko && \
sudo dmesg | grep AHCI_DUMP; \
sudo rmmod ahci_dump