#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>

// BAR5 Base Address from lspci (0xf0806000) + Port 0 Offset (0x100)
#define AHCI_PORT0_MMIO 0xf0806100

MODULE_LICENSE("GPL");

static int __init ahci_dump_init(void)
{
    void __iomem *mmio_reg;
    void __iomem *lst_virt;
    u32 lst_phys;
    u32 flags, ctba;
    int i;

    // 1. Map the AHCI Hardware Register
    mmio_reg = ioremap(AHCI_PORT0_MMIO, 4);
    if (!mmio_reg) {
        pr_err("AHCI_DUMP: Failed to map MMIO register!\n");
        return -ENOMEM;
    }

    // 2. Read live PORT_LST_ADDR from hardware
    lst_phys = readl(mmio_reg);
    iounmap(mmio_reg);

    pr_info("AHCI_DUMP: Hardware reports PORT_LST_ADDR = 0x%08X\n", lst_phys);

    if (lst_phys == 0 || lst_phys == 0xFFFFFFFF) {
        pr_err("AHCI_DUMP: Invalid physical address read from register!\n");
        return -EINVAL;
    }

    // 3. Map the true physical RAM allocated by the AHCI driver
    lst_virt = ioremap(lst_phys, 1024);
    if (!lst_virt) {
        pr_err("AHCI_DUMP: Failed to map memory at 0x%08X\n", lst_phys);
        return -ENOMEM;
    }

    // 4. Dump the real 32 slots
    for (i = 0; i < 32; i++) {
        void __iomem *slot_hdr = lst_virt + (i * 32);
        flags = readl(slot_hdr);
        ctba = readl(slot_hdr + 8);

        if (flags != 0 || ctba != 0) {
            pr_info("AHCI_DUMP: Slot %02d | Flags: 0x%08X | CTBA Pointer: 0x%08X\n", i, flags, ctba);
        }
    }

    iounmap(lst_virt);
    return 0;
}

static void __exit ahci_dump_exit(void)
{
    pr_info("AHCI_DUMP: Unloaded.\n");
}

module_init(ahci_dump_init);
module_exit(ahci_dump_exit);