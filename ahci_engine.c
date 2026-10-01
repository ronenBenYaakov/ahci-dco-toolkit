#include "stealth_dco.h"

int execute_ahci_slot_stealth(void *port_base, int slot, bool is_dco_set)
{
    u32 ci_val, tfd_val, serr_val;
    int i, max_polls = is_dco_set ? 300 : 100;

    pr_info("[STEALTH_DCO] [+] Preparing slot %d execution (DCO Set: %s) [Zero-Footprint Polled Mode]\n", slot, is_dco_set ? "YES" : "NO");

    /* 
     * Elite Stealth Note: 
     * We no longer disable or restore PxIE. Leaving the interrupt mask 
     * completely pristine ensures zero configuration divergence for forensic tools.
     */

    /* Issue command to the target slot via Command Issue (PxCI) */
    ci_val = readl((char *)port_base + AHCI_PxCI);
    ci_val |= (1U << slot);
    writel(ci_val, (char *)port_base + AHCI_PxCI);
    wmb();

    /* Synchronous polling loop (Decoupled from OS interrupt trees) */
    for (i = 0; i < max_polls; i++) {
        u32 current_ci = readl((char *)port_base + AHCI_PxCI);
        tfd_val = readl((char *)port_base + AHCI_PxTFD);

        /* Check if slot cleared (execution complete) and controller is free of busy/data request flags */
        if (!(current_ci & (1U << slot)) || !(tfd_val & 0x88)) {
            pr_info("[STEALTH_DCO] [+] Slot %d completed at poll iteration %d. PxTFD: 0x%08x\n", slot, i, tfd_val);

            /* Atomic Status & Error Scrubbing */
            writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
            
            serr_val = readl((char *)port_base + AHCI_PxSERR);
            writel(serr_val, (char *)port_base + AHCI_PxSERR);
            wmb();

            return 0;
        }
        udelay(50);
    }
    
    pr_err("[STEALTH_DCO] [-] ERROR: Slot %d timed out! PxTFD: 0x%08x\n", slot, readl((char *)port_base + AHCI_PxTFD));
    
    /* Clean up status even on failure paths to prevent forensic leakage */
    writel(readl((char *)port_base + AHCI_PxIS), (char *)port_base + AHCI_PxIS);
    wmb();

    return -ETIMEDOUT;
}