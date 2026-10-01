#ifndef STEALTH_DCO_H
#define STEALTH_DCO_H

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

/* AHCI Port Configuration & Constants */
#define VBOX_AHCI_FALLBACK_PHYS 0xe1900000ULL
#define AHCI_PORT_NUM     0
#define AHCI_PORT_BASE    (0x100 + (AHCI_PORT_NUM) * 0x80)
#define AHCI_ABAR_SIZE    0x1000

/* Global Host Control Register Offsets & Flags */
#define AHCI_GHC          0x04
#define GHC_IE            (1U << 1)

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

/* Method 2: Intel ICH Vendor-Specific / Scratchpad MMIO Offset Window */
#define INTEL_AHCI_VENDOR_SCRATCH_OFFSET 0xA0

#define PxIS_TFES         (1U << 30)

#define NATIVE_MAX_LBA    2097152ULL
#define TAIL_TRIM_SECTORS 1000ULL
#define STEALTH_MAX_LBA   (NATIVE_MAX_LBA - TAIL_TRIM_SECTORS)
#define TARGET_INSPECT_LBA (NATIVE_MAX_LBA - 1ULL)

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

/* Function Prototypes */
void __iomem *manual_direct_map(unsigned long phys_addr);
void __iomem *get_stealth_abar_mmio(void);
void execute_method2_onchip_mmio_audit(void __iomem *abar_base);
u32 execute_method4_intx_suppression(void __iomem *abar_base);
int execute_ahci_slot_stealth(void *port_base, int slot, bool is_dco_set);
void execute_apic_stealth_engine(void);
void stealth_sanitize_and_flush(void *addr, size_t size);

#endif /* STEALTH_DCO_H */
