#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>

#define AHCI_LST_PHYS 0x109B00000ULL

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("AHCI Command List & PRDT Decoder");

static const char *get_ata_cmd_name(u8 cmd)
{
    switch (cmd) {
    case 0x20: return "READ SECTORS";
    case 0x24: return "READ EXT";
    case 0x25: return "READ DMA EXT";
    case 0x30: return "WRITE SECTORS";
    case 0x34: return "WRITE EXT";
    case 0x35: return "WRITE DMA EXT";
    case 0x60: return "READ FPDMA QUEUED (NCQ Read)";
    case 0x61: return "WRITE FPDMA QUEUED (NCQ Write)";
    case 0xEC: return "IDENTIFY DEVICE";
    case 0xE7: return "FLUSH CACHE EXT";
    case 0xCA: return "DATA SET MANAGEMENT (TRIM)";
    default:   return "UNKNOWN / VENDOR SPECIFIC";
    }
}

static int __init ahci_decode_init(void)
{
    void *lst_virt;
    u32 flags, prdbc, ctba_low, ctba_high;
    u64 ctba_phys;
    int i;

    pr_info("[AHCI_DECODER] Mapping Command List at 0x%llX...\n", AHCI_LST_PHYS);

    // Map 1024 bytes for the 32 Command Headers
    lst_virt = memremap(AHCI_LST_PHYS, 1024, MEMREMAP_WB);
    if (!lst_virt) {
        lst_virt = (void *)ioremap(AHCI_LST_PHYS, 1024);
        if (!lst_virt) {
            pr_err("[AHCI_DECODER] Failed to map physical address 0x%llX\n", AHCI_LST_PHYS);
            return -ENOMEM;
        }
    }

    for (i = 0; i < 32; i++) {
        void *slot_hdr = lst_virt + (i * 32);

        flags     = readl(slot_hdr + 0x00);
        prdbc     = readl(slot_hdr + 0x04);
        ctba_low  = readl(slot_hdr + 0x08);
        ctba_high = readl(slot_hdr + 0x0C);

        ctba_phys = ((u64)ctba_high << 32) | ctba_low;

        if (ctba_phys == 0)
            continue;

        u32 prdtl    = (flags >> 16) & 0xFFFF;
        u32 is_write = (flags >> 6) & 0x1;

        // Map the Command Table (CTBA) region (256 bytes covers FIS + ACDB + 1st PRDT entry)
        void *tbl_virt = memremap(ctba_phys, 256, MEMREMAP_WB);
        if (!tbl_virt)
            tbl_virt = (void *)ioremap(ctba_phys, 256);

        if (tbl_virt) {
            u8 fis_type = readb(tbl_virt + 0x00);

            if (fis_type == 0x27) { // Register FIS - Host to Device
                u8 ata_cmd   = readb(tbl_virt + 0x02);
                u8 lba0      = readb(tbl_virt + 0x04);
                u8 lba1      = readb(tbl_virt + 0x05);
                u8 lba2      = readb(tbl_virt + 0x06);
                u8 lba3      = readb(tbl_virt + 0x08);
                u8 lba4      = readb(tbl_virt + 0x09);
                u8 lba5      = readb(tbl_virt + 0x0A);
                u8 count_l   = readb(tbl_virt + 0x0C);
                u8 count_h   = readb(tbl_virt + 0x0D);

                u64 lba48 = ((u64)lba5 << 40) | ((u64)lba4 << 32) |
                            ((u64)lba3 << 24) | ((u64)lba2 << 16) |
                            ((u64)lba1 << 8)  | (u64)lba0;

                u16 sector_count = ((u16)count_h << 8) | count_l;

                pr_info("[AHCI_DECODER] Slot %02d | %s | CTBA: 0x%llX | CMD: 0x%02X (%s) | LBA: %llu | Sectors: %u | PRDTL: %u | PRDBC: %u\n",
                        i, is_write ? "WRITE" : "READ ", ctba_phys, ata_cmd,
                        get_ata_cmd_name(ata_cmd), lba48, sector_count, prdtl, prdbc);

                // Decode PRDT Entry 0 (located at CTBA + 0x80)
                if (prdtl > 0) {
                    void *prdt_virt = tbl_virt + 0x80;

                    u32 dba_low  = readl(prdt_virt + 0x00);
                    u32 dba_high = readl(prdt_virt + 0x04);
                    u32 dbc_raw  = readl(prdt_virt + 0x0C);

                    u64 dba_phys = ((u64)dba_high << 32) | dba_low;
                    u32 byte_count = (dbc_raw & 0x003FFFFF) + 1; // Bits 21:0 store (Length - 1)

                    pr_info("[AHCI_DECODER]   -> Data Buffer Phys Addr (DBA) : 0x%llX\n", dba_phys);
                    pr_info("[AHCI_DECODER]   -> Described Buffer Size       : %u bytes (%u KB)\n",
                            byte_count, byte_count / 1024);
                }
            } else {
                pr_info("[AHCI_DECODER] Slot %02d | CTBA: 0x%llX | Non-Reg FIS: 0x%02X\n",
                        i, ctba_phys, fis_type);
            }

            memunmap(tbl_virt);
        } else {
            pr_err("[AHCI_DECODER] Failed to map CTBA at 0x%llX\n", ctba_phys);
        }
    }

    memunmap(lst_virt);
    return 0;
}

static void __exit ahci_decode_exit(void)
{
    pr_info("[AHCI_DECODER] Module unloaded\n");
}

module_init(ahci_decode_init);
module_exit(ahci_decode_exit);