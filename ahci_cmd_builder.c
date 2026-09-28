#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>

/* ------------------------------------------------------------------ *
 *  AHCI MMIO base (BAR5 / ABAR)
 * ------------------------------------------------------------------ */
#define AHCI_ABAR_PHYS    0xe1900000ULL
/* Port selection */
#define AHCI_PORT_NUM     0
#define AHCI_PORT_BASE    (0x100 + (AHCI_PORT_NUM) * 0x80)

/* Port register offsets */
#define AHCI_PxCLB        0x00
#define AHCI_PxCLBU       0x04
#define AHCI_PxIS         0x10
#define AHCI_PxCMD        0x18
#define AHCI_PxSERR       0x30
#define AHCI_PxCI         0x38

/* PxIS bits */
#define PxIS_DHRS         (1U << 0)
#define PxIS_PSS          (1U << 1)
#define PxIS_DPS          (1U << 2)
#define PxIS_SDBS         (1U << 3)
#define PxIS_TFES         (1U << 30)

/* 
 * Stealth DCO Parameters:
 * 1. Micro-Trim: Instead of dropping to 256 sectors, we trim a tiny tail 
 *    (e.g., hiding 1000 sectors off a 2M sector drive, leaving partitions safe).
 * 2. Feature Masking: Setting specific configuration option bits in count_low 
 *    to control feature exposure and telemetry reporting.
 */
#define NATIVE_MAX_LBA    2097152ULL
#define TAIL_TRIM_SECTORS 1000ULL
#define STEALTH_MAX_LBA   (NATIVE_MAX_LBA - TAIL_TRIM_SECTORS)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Researcher");
MODULE_DESCRIPTION("Detailed Telemetry Stealth AHCI DCO Micro-Trim and Feature Masking Module");

/* ------------------------------------------------------------------ *
 *  Structures
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */
static int __init ahci_stealth_dco_init(void)
{
    void __iomem *abar_base = NULL;
    void *port_base = NULL;
    void *ctba_virt = NULL;
    void *lst_slot_virt = NULL;

    struct sata_fis_h2d    *fis;
    struct ahci_cmd_header *hdr;

    u64 ctba_phys = 0, lst_phys = 0;
    u32 clb_lo, clb_hi;
    u32 ci_val, pxci_after, pxis_after, pxserr_after, cmd_val;
    u32 pxcmd_before, pxis_before;
    unsigned long ctba_page = 0;
    int i, ret = 0;

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] ---- initializing stealth sequence (DETAILED) ----\n");
    pr_info("[STEALTH_DCO] ==================================================\n");

    /* -------------------------------------------------------------- *
     * 1. Map ABAR with ioremap
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 1: Mapping ABAR physical address 0x%llX...\n", AHCI_ABAR_PHYS);
    abar_base = ioremap(AHCI_ABAR_PHYS, 0x1100);
    if (!abar_base) {
        pr_err("[STEALTH_DCO] FATAL: ioremap failed for ABAR 0x%llX\n", AHCI_ABAR_PHYS);
        return -ENOMEM;
    }
    port_base = (void *)((char *)abar_base + AHCI_PORT_BASE);
    pr_info("[STEALTH_DCO] ABAR successfully mapped at virtual address %px\n", abar_base);
    pr_info("[STEALTH_DCO] Target Port Index: %u | Calculated Port Base Offset: 0x%X\n", AHCI_PORT_NUM, AHCI_PORT_BASE);

    /* -------------------------------------------------------------- *
     * 2. Read Command List base from PxCLB / PxCLBU
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 2: Reading Command List Base registers (PxCLB / PxCLBU)...\n");
    clb_lo = readl((char *)port_base + AHCI_PxCLB);
    clb_hi = readl((char *)port_base + AHCI_PxCLBU);
    lst_phys = ((u64)clb_hi << 32) | clb_lo;

    pr_info("[STEALTH_DCO] PxCLB low register value  = 0x%08X\n", clb_lo);
    pr_info("[STEALTH_DCO] PxCLBU high register value = 0x%08X\n", clb_hi);
    pr_info("[STEALTH_DCO] Resolved Command List Physical Address (lst_phys) = 0x%016llX\n", lst_phys);

    if (!lst_phys) {
        pr_err("[STEALTH_DCO] FATAL: PxCLB=0 — AHCI driver not bound to port %u\n", AHCI_PORT_NUM);
        ret = -ENODEV;
        goto out;
    }

    /* -------------------------------------------------------------- *
     * 3. Allocate low page for the Command Table
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 3: Allocating DMA-safe memory page for Command Table (CTBA)...\n");
    ctba_page = __get_free_page(GFP_KERNEL | GFP_DMA32);
    if (!ctba_page) {
        pr_err("[STEALTH_DCO] FATAL: __get_free_page(CTBA) failed\n");
        ret = -ENOMEM;
        goto out;
    }
    ctba_virt = (void *)ctba_page;
    ctba_phys = (u64)virt_to_phys(ctba_virt);
    memset(ctba_virt, 0, 256);

    pr_info("[STEALTH_DCO] CTBA allocated successfully:\n");
    pr_info("[STEALTH_DCO]   -> Virtual Address : %px\n", ctba_virt);
    pr_info("[STEALTH_DCO]   -> Physical Address: 0x%016llX\n", ctba_phys);

    /* -------------------------------------------------------------- *
     * 4. Populate FIS for DEVICE CONFIGURATION SET with Micro-Trim
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 4: Constructing Host-to-Device FIS (DCO B1h / Set C2h)...\n");
    fis = (struct sata_fis_h2d *)ctba_virt;
    fis->fis_type    = 0x27;
    fis->pm_port_c   = 0x80;
    fis->command     = 0xB1;  /* DEVICE CONFIGURATION command */
    fis->feature_low = 0xC2;  /* SET subcommand */
    
    /* Apply Micro-Trim Max LBA (hiding only the tail sectors) */
    fis->lba0        = (u8)(STEALTH_MAX_LBA & 0xFF);
    fis->lba1        = (u8)((STEALTH_MAX_LBA >> 8) & 0xFF);
    fis->lba2        = (u8)((STEALTH_MAX_LBA >> 16) & 0xFF);
    fis->device      = 0x40;  /* LBA mode */
    fis->lba3        = (u8)((STEALTH_MAX_LBA >> 24) & 0xFF);
    fis->lba4        = 0;
    fis->lba5        = 0;
    
    fis->count_low   = 0x01;  /* Configuration flags */
    fis->count_high  = 0x00;

    pr_info("[STEALTH_DCO] FIS Structure Populated:\n");
    pr_info("[STEALTH_DCO]   -> Command Code     : 0x%02X (DEVICE CONFIGURATION)\n", fis->command);
    pr_info("[STEALTH_DCO]   -> Feature Low      : 0x%02X (SET Subcommand)\n", fis->feature_low);
    pr_info("[STEALTH_DCO]   -> Native Max LBA   : %llu sectors\n", (unsigned long long)NATIVE_MAX_LBA);
    pr_info("[STEALTH_DCO]   -> Tail Trimmed     : %llu sectors\n", (unsigned long long)TAIL_TRIM_SECTORS);
    pr_info("[STEALTH_DCO]   -> Stealth Target   : 0x%llX (%llu active sectors)\n",
            (unsigned long long)STEALTH_MAX_LBA, (unsigned long long)STEALTH_MAX_LBA);

    /* -------------------------------------------------------------- *
     * 5. Fill Slot 31 Command Header (Non-data command, PRDTL = 0)
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 5: Mapping and populating command list slot 31 header...\n");
    lst_slot_virt = memremap(lst_phys + (31 * 32), 32, MEMREMAP_WB);
    if (!lst_slot_virt) {
        pr_err("[STEALTH_DCO] FATAL: memremap command header failed for slot 31\n");
        ret = -ENOMEM;
        goto out;
    }

    hdr = (struct ahci_cmd_header *)lst_slot_virt;
    hdr->dw0   = (5 & 0x1F) | (0 << 16); /* CFL = 5 dwords, PRDTL = 0 */
    hdr->dw1   = 0;
    hdr->ctba  = (u32)(ctba_phys & 0xFFFFFFFF);
    hdr->ctbau = (u32)(ctba_phys >> 32);

    wmb();
    pr_info("[STEALTH_DCO] Slot 31 Header Written Successfully:\n");
    pr_info("[STEALTH_DCO]   -> DW0  = 0x%08X (CFL=5, PRDTL=0)\n", hdr->dw0);
    pr_info("[STEALTH_DCO]   -> DW1  = 0x%08X\n", hdr->dw1);
    pr_info("[STEALTH_DCO]   -> CTBA = 0x%08X%08X\n", hdr->ctbau, hdr->ctba);

    /* -------------------------------------------------------------- *
     * 6. Snapshot Pre-Execution Register State
     * -------------------------------------------------------------- */
    pxcmd_before = readl((char *)port_base + AHCI_PxCMD);
    pxis_before  = readl((char *)port_base + AHCI_PxIS);
    pr_info("[STEALTH_DCO] Pre-Execution Register Snapshot:\n");
    pr_info("[STEALTH_DCO]   -> PxCMD before = 0x%08X\n", pxcmd_before);
    pr_info("[STEALTH_DCO]   -> PxIS before  = 0x%08X\n", pxis_before);

    /* -------------------------------------------------------------- *
     * 7. Force Enable Port Command Engine
     * -------------------------------------------------------------- */
    pr_info("[STEALTH_DCO] Step 6: Ensuring Port Command Engine (ST and FRE) is active...\n");
    cmd_val = pxcmd_before | (1U << 0) | (1U << 4);
    writel(cmd_val, (char *)port_base + AHCI_PxCMD);
    wmb();
    pr_info("[STEALTH_DCO] PxCMD updated to: 0x%08X\n", readl((char *)port_base + AHCI_PxCMD));

    /* -------------------------------------------------------------- *
     * 8. Issue the command via PxCI slot 31
     * -------------------------------------------------------------- */
    ci_val  = readl((char *)port_base + AHCI_PxCI);
    pr_info("[STEALTH_DCO] Step 7: Triggering execution. Current PxCI = 0x%08X\n", ci_val);
    
    ci_val |= (1U << 31);
    writel(ci_val, (char *)port_base + AHCI_PxCI);
    pr_info("[STEALTH_DCO] PxCI written with Slot 31 set (0x80000000). Awaiting controller response...\n");

    /* -------------------------------------------------------------- *
     * 9. Bounded wait for HBA completion with iteration telemetry
     * -------------------------------------------------------------- */
    for (i = 0; i < 50; i++) {
        pxci_after = readl((char *)port_base + AHCI_PxCI);
        if (!(pxci_after & (1U << 31))) {
            pr_info("[STEALTH_DCO] Slot 31 cleared by HBA at iteration %d (%d ms elapsed)\n", i, i * 10);
            break;
        }
        msleep(10);
    }

    pxis_after   = readl((char *)port_base + AHCI_PxIS);
    pxserr_after = readl((char *)port_base + AHCI_PxSERR);

    pr_info("[STEALTH_DCO] ==================================================\n");
    pr_info("[STEALTH_DCO] POST-EXECUTION TELEMETRY SUMMARY:\n");
    pr_info("[STEALTH_DCO]   -> Final PxCI   = 0x%08X\n", pxci_after);
    pr_info("[STEALTH_DCO]   -> Final PxIS   = 0x%08X\n", pxis_after);
    pr_info("[STEALTH_DCO]   -> Final PxSERR = 0x%08X\n", pxserr_after);
    pr_info("[STEALTH_DCO] ==================================================\n");

    if (i == 50) {
        pr_err("[STEALTH_DCO] ERROR: Timeout waiting for HBA slot completion (PxCI stuck at 0x%08X)\n", pxci_after);
        ret = -EIO;
        goto out;
    }

    if (pxis_after & PxIS_TFES) {
        pr_err("[STEALTH_DCO] ERROR: Task File Error Set (TFES) bit asserted — DCO configuration rejected by hardware!\n");
        ret = -EIO;
    } else {
        pr_info("[STEALTH_DCO] SUCCESS: Stealth DCO micro-trim and feature mask applied successfully.\n");
    }

out:
    if (lst_slot_virt) {
        memunmap(lst_slot_virt);
        pr_info("[STEALTH_DCO] Cleanup: Unmapped slot 31 header memory.\n");
    }
    if (ctba_page) {
        free_page(ctba_page);
        pr_info("[STEALTH_DCO] Cleanup: Freed CTBA page.\n");
    }
    if (abar_base) {
        iounmap(abar_base);
        pr_info("[STEALTH_DCO] Cleanup: Unmapped ABAR region.\n");
    }
    
    pr_info("[STEALTH_DCO] ---- initialization sequence complete ----\n");
    return ret;
}

static void __exit ahci_stealth_dco_exit(void)
{
    pr_info("[STEALTH_DCO] ---- module unloaded cleanly ----\n");
}

module_init(ahci_stealth_dco_init);
module_exit(ahci_stealth_dco_exit);