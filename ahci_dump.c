#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>

#define AHCI_LST_PHYS 0x109B00000ULL

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("Parse AHCI Command List and CTBA FIS contents");

static int __init ahci_dump_init(void)
{
    void __iomem *lst_virt;
    u32 flags, prdbc, ctba_low, ctba_high;
    u64 ctba_phys;
    int i;

    pr_info("AHCI_DUMP: Mapping physical memory 0x%llX\n", AHCI_LST_PHYS);

    // Map 1 KB (32 slots * 32 bytes)
    lst_virt = ioremap(AHCI_LST_PHYS, 1024);
    if (!lst_virt) {
        pr_err("AHCI_DUMP: Failed to ioremap 0x%llX\n", AHCI_LST_PHYS);
        return -ENOMEM;
    }

    for (i = 0; i < 32; i++) {
        void __iomem *slot_hdr = lst_virt + (i * 32);

        flags     = readl(slot_hdr + 0x00);
        prdbc     = readl(slot_hdr + 0x04);
        ctba_low  = readl(slot_hdr + 0x08);
        ctba_high = readl(slot_hdr + 0x0C);

        ctba_phys = ((u64)ctba_high << 32) | ctba_low;

        if (flags != 0 || ctba_phys != 0) {
            u32 cfl   = flags & 0x1F;
            u32 prdtl = (flags >> 16) & 0xFFFF;
            u8 ata_cmd = 0;
            u8 fis_type = 0;

            // Follow CTBA pointer to map the Command Table if non-zero
            if (ctba_phys != 0) {
                void __iomem *tbl_virt = ioremap(ctba_phys, 128);
                if (tbl_virt) {
                    fis_type = readb(tbl_virt + 0x00); // FIS Type (0x27 = Reg H2D)
                    ata_cmd  = readb(tbl_virt + 0x02); // ATA Command Opcode
                    iounmap(tbl_virt);
                }
            }

            pr_info("AHCI_DUMP: Slot %02d | Flags: 0x%08X (CFL=%u, PRDTL=%u) | PRDBC: %u | CTBA: 0x%llX | FIS: 0x%02X (CMD: 0x%02X)\n",
                    i, flags, cfl, prdtl, prdbc, ctba_phys, fis_type, ata_cmd);
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