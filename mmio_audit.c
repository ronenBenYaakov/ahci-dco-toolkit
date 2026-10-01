#include "stealth_dco.h"

void __iomem *manual_direct_map(unsigned long phys_addr)
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

void __iomem *get_stealth_abar_mmio(void)
{
    struct pci_dev *pdev = NULL;
    resource_size_t abar_phys;
    void __iomem *mmio = NULL;

    pr_info("[STEALTH_DCO] [*] Initiating precise SATA AHCI PCI discovery & INTx masking...\n");

    while ((pdev = pci_get_class(PCI_CLASS_STORAGE_SATA << 8, pdev)) != NULL) {
        abar_phys = pci_resource_start(pdev, 5);
        if (abar_phys) {
            u16 pci_cmd;
            
            pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);
            pci_cmd |= (1 << 10);
            pci_write_config_word(pdev, PCI_COMMAND, pci_cmd);
            pr_info("[STEALTH_DCO] [+] PCI INTx disabled via PCI Command Register config space for %s (class 0x%06x)\n", 
                    pci_name(pdev), pdev->class);

            pr_info("[STEALTH_DCO] [+] Found SATA AHCI controller %s with BAR5 at 0x%llx\n",
                    pci_name(pdev), (unsigned long long)abar_phys);
            
            mmio = ioremap(abar_phys, AHCI_ABAR_SIZE);
            if (mmio) {
                pci_dev_put(pdev);
                return mmio;
            } else {
                pr_warn("[STEALTH_DCO] [!] ioremap failed for BAR5 0x%llx on %s\n", 
                        (unsigned long long)abar_phys, pci_name(pdev));
            }
        }
    }

    pr_warn("[STEALTH_DCO] [!] Dynamic SATA AHCI scan missed BAR5. Applying static fallback map: 0x%llx\n", 
            VBOX_AHCI_FALLBACK_PHYS);
    
    mmio = ioremap(VBOX_AHCI_FALLBACK_PHYS, AHCI_ABAR_SIZE);
    if (mmio) {
        pr_info("[STEALTH_DCO] [+] Successfully resolved fallback ABAR via static mapping.\n");
        return mmio;
    }

    pr_err("[STEALTH_DCO] [-] FATAL: Hardware ABAR resolution and fallback both failed.\n");
    return NULL;
}

void execute_method2_onchip_mmio_audit(void __iomem *abar_base)
{
    u32 scratch_val_orig, scratch_val_test;

    pr_info("[STEALTH_DCO] --- Method 2: On-Chip MMIO Scratchpad / Vendor Register Audit ---\n");

    scratch_val_orig = readl((char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    pr_info("[STEALTH_DCO] [+] Original Vendor Scratchpad (Offset 0x%X): 0x%08x\n",
            INTEL_AHCI_VENDOR_SCRATCH_OFFSET, scratch_val_orig);

    writel(scratch_val_orig ^ 0x5A5A5A5A, (char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();
    
    scratch_val_test = readl((char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    pr_info("[STEALTH_DCO] [+] Verified On-Chip MMIO Responsive State: 0x%08x\n", scratch_val_test);

    writel(scratch_val_orig, (char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();

    pr_info("[STEALTH_DCO] [+] Method 2 On-Chip MMIO scratchpad interaction complete.\n");
}

#include <asm/io.h>
#include <linux/io.h>

static __always_inline u32 raw_pci_read32(u8 bus, u8 dev, u8 fn, u8 reg)
{
    outl((1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC), 0xCF8);
    return inl(0xCFC);
}

static __always_inline u16 raw_pci_read16(u8 bus, u8 dev, u8 fn, u8 reg)
{
    outl((1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC), 0xCF8);
    return inw(0xCFC + (reg & 2));
}

static __always_inline void raw_pci_write16(u8 bus, u8 dev, u8 fn, u8 reg, u16 val)
{
    outl((1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC), 0xCF8);
    outw(val, 0xCFC + (reg & 2));
}

static __always_inline void raw_pci_write32(u8 bus, u8 dev, u8 fn, u8 reg, u32 val)
{
    outl((1U << 31) | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC), 0xCF8);
    outl(val, 0xCFC);
}

int suppress_interrupts_adaptive(u8 bus, u8 dev, u8 fn)
{
    u16 status, cap_ptr;
    u8 cap_id, current_ptr;
    bool intercepted = false;

    status = raw_pci_read16(bus, dev, fn, 0x06);
    if (!(status & (1 << 4))) 
        return -1;

    cap_ptr = raw_pci_read16(bus, dev, fn, 0x34) & 0xFC;
    current_ptr = (u8)cap_ptr;

    /* 1. Stealthy Hardware-Level MSI / MSI-X Vector Masking (Maintains 'Enable+' in lspci) */
    while (current_ptr) {
        u32 cap_header = raw_pci_read32(bus, dev, fn, current_ptr);
        cap_id = (u8)(cap_header & 0xFF);

        if (cap_id == 0x05) {
            u16 msi_ctrl = raw_pci_read16(bus, dev, fn, current_ptr + 2);
            if (msi_ctrl & (1 << 0)) {
                if (msi_ctrl & (1 << 8)) { // Check if Maskable+ is supported
                    u8 mask_offset = current_ptr + 12; // 32-bit MSI Vector Mask register
                    u32 msi_mask = raw_pci_read32(bus, dev, fn, mask_offset);
                    raw_pci_write32(bus, dev, fn, mask_offset, msi_mask | 0x1);
                    intercepted = true;
                }
                break;
            }
        } 
        else if (cap_id == 0x11) {
            u16 msix_ctrl = raw_pci_read16(bus, dev, fn, current_ptr + 2);
            if (msix_ctrl & (1 << 15)) {
                raw_pci_write16(bus, dev, fn, current_ptr + 2, msix_ctrl | (1 << 14)); // Function Mask Bit
                intercepted = true;
                break;
            }
        }

        current_ptr = (u8)((cap_header >> 8) & 0xFC);
    }

    /* 2. Direct Motherboard MMIO Quiescence (GHC Interrupt Enable + Port-Level Gates) */
    {
        u32 bar5 = raw_pci_read32(bus, dev, fn, 0x24); // Region 5 BAR
        if (bar5 & 0xFFFFFFFC) {
            void __iomem *hba_base = ioremap(bar5 & 0xFFFFFFFC, 0x2000);
            if (hba_base) {
                u32 pi, ghc;
                int i;

                /* Clear Global HBA Interrupt Enable (IE) at offset 0x04 */
                ghc = readl(hba_base + 0x04);
                if (ghc & (1 << 1)) {
                    writel(ghc & ~(1 << 1), hba_base + 0x04);
                }

                /* Traverse Ports Implemented (PI) register at offset 0x0C */
                pi = readl(hba_base + 0x0C);
                for (i = 0; i < 32; i++) {
                    if (pi & (1 << i)) {
                        void __iomem *port_base = hba_base + 0x100 + (i * 0x80);
                        
                        /* Clear any latched Port Interrupt Status flags */
                        u32 port_is = readl(port_base + 0x10);
                        if (port_is) {
                            writel(port_is, port_base + 0x10);
                        }

                        /* Zero out individual Port Interrupt Enable masks */
                        writel(0x0, port_base + 0x14);
                    }
                }

                iounmap(hba_base);
            }
        }
    }

    return intercepted ? 0 : -1;
}