#include "stealth_dco.h"

int execute_ahci_slot_stealth(void *port_base, int slot, bool is_dco_set)
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
        udelay(50);
    }
    
    pr_err("[STEALTH_DCO] [-] ERROR: Slot %d timed out! PxTFD: 0x%08x\n", slot, readl((char *)port_base + AHCI_PxTFD));
    
    writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
    writel(ie_orig, (char *)port_base + AHCI_PxIE);
    wmb();

    return -ETIMEDOUT;
}
