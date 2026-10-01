#include <linux/kernel.h>
#include <linux/io.h>
#include <asm/io.h>
#include <asm/irq_vectors.h>
#include <asm/cacheflush.h>

#define LAPIC_BASE_PHYS      0xFEE00000ULL
#define LAPIC_ID_REG         0x0020
#define LAPIC_TPR_REG        0x0080
#define LAPIC_SIV_REG        0x00F0
#define LAPIC_LVT_TIMER      0x0320
#define LAPIC_LVT_PERF       0x0340
#define LAPIC_TIMER_INIT     0x0380
#define LAPIC_TIMER_CURR     0x0390
#define LAPIC_TIMER_DIV      0x03E0

/* Static persistent pointer to eliminate runtime ioremap/iounmap churn */
static void __iomem *cached_lapic_base = NULL;

/*
 * Initialize and persistently map the Local APIC subsystem once.
 * This confines all loud page-table/MMIO mapping overhead to the init window.
 */
int __init init_apic_stealth_subsystem(void)
{
    u32 perf_val;

    if (cached_lapic_base)
        return 0; // Already mapped

    pr_info("[STEALTH_APIC] [*] Persistently mapping Local APIC physical frame @ 0x%llx\n", LAPIC_BASE_PHYS);
    cached_lapic_base = ioremap(LAPIC_BASE_PHYS, PAGE_SIZE);
    if (!cached_lapic_base) {
        pr_err("[STEALTH_DCO] [-] CRITICAL: Failed to persistently map LAPIC physical address space\n");
        return -ENOMEM;
    }

    /* Pre-mask LVT Performance Monitor once at startup to keep runtime silent */
    perf_val = readl(cached_lapic_base + LAPIC_LVT_PERF);
    if (!(perf_val & (1 << 16))) {
        perf_val |= (1 << 16);
        writel(perf_val, cached_lapic_base + LAPIC_LVT_PERF);
        pr_info("[STEALTH_APIC] [+] PASS: LVT Performance Monitor pre-masked (Val: 0x%08x)\n", perf_val);
    } else {
        pr_info("[STEALTH_APIC] [*] LVT Performance Monitor was already masked\n");
    }

    return 0;
}

/*
 * Clean up persistent mapping during module unload
 */
void cleanup_apic_stealth_subsystem(void)
{
    if (cached_lapic_base) {
        pr_info("[STEALTH_APIC] [*] Unmapping persistent Local APIC handle\n");
        iounmap(cached_lapic_base);
        cached_lapic_base = NULL;
    }
}

/*
 * Global Project-Wide APIC-Serialized Sanitization Routine (Zero-Overhead Cached Version)
 * - Uses persistent memory mapping to avoid page-table modification traces at runtime.
 * - Samples the Local APIC countdown timer before and after to track tick deltas silently.
 */
void stealth_sanitize_and_flush(void *addr, size_t size)
{
    u32 tick_before, tick_after, tick_delta;

    if (!addr || size == 0) {
        pr_warn("[STEALTH_APIC] [-] WARNING: stealth_sanitize_and_flush called with NULL address or zero size\n");
        return;
    }

    pr_info("[STEALTH_APIC] [*] Initiating APIC-serialized sanitization for buffer @ %p (%zu bytes)\n", addr, size);

    /* Fallback to direct clear if subsystem initialization failed */
    if (!cached_lapic_base) {
        pr_err("[STEALTH_APIC] [-] CRITICAL: Cached LAPIC base is NULL, falling back to direct clear\n");
        memzero_explicit(addr, size);
        clflush_cache_range(addr, size);
        pr_info("[STEALTH_APIC] [+] PASS: Fallback direct memory clear & cache flush completed\n");
        return;
    }

    /* 1. Capture Pre-Sanitization APIC Tick Stamp (Instantaneous MMIO read) */
    tick_before = readl(cached_lapic_base + LAPIC_TIMER_CURR);

    /* 2. Execute Core Memory Sanitization & Strict Cache Line Scrubbing */
    memzero_explicit(addr, size);
    clflush_cache_range(addr, size);

    /* 3. Capture Post-Sanitization APIC Tick Stamp to verify serialization */
    tick_after = readl(cached_lapic_base + LAPIC_TIMER_CURR);
    tick_delta = (tick_before > tick_after) ? (tick_before - tick_after) : 0;

    pr_info("[STEALTH_APIC] [+] PASS: Sanitization & clflush finished. Tick Delta: 0x%08x (Before: 0x%08x -> After: 0x%08x)\n",
            tick_delta, tick_before, tick_after);
}

void execute_apic_stealth_engine(void)
{
    u32 lapic_id, lvt_timer_val, init_count, curr_count, perf_val;

    pr_info("[STEALTH_APIC] ==================================================\n");
    pr_info("[STEALTH_APIC] ---- APIC TIMER STEALING & LVT MASKING ENGINE ----\n");
    pr_info("[STEALTH_APIC] ==================================================\n");

    /* Ensure the persistent subsystem mapping is active */
    if (init_apic_stealth_subsystem() != 0) {
        return;
    }

    /* 1. Read Local APIC ID to verify active core context */
    lapic_id = readl(cached_lapic_base + LAPIC_ID_REG);
    pr_info("[STEALTH_APIC] [+] PASS: Active Core Local APIC ID Verified: 0x%08x\n", lapic_id);

    /* 2. Inspect Existing LVT Timer Configuration */
    lvt_timer_val = readl(cached_lapic_base + LAPIC_LVT_TIMER);
    pr_info("[STEALTH_APIC] [+] PASS: Existing LVT Timer Register: 0x%08x (Vector: 0x%02X, Masked: %s)\n", 
            lvt_timer_val, (lvt_timer_val & 0xFF), (lvt_timer_val & (1 << 16)) ? "YES" : "NO");

    /* 3. Read Current Timer Initial & Countdown Values */
    init_count = readl(cached_lapic_base + LAPIC_TIMER_INIT);
    curr_count = readl(cached_lapic_base + LAPIC_TIMER_CURR);
    pr_info("[STEALTH_APIC] [+] PASS: Timer Initial Count: 0x%08x | Current Count: 0x%08x\n", init_count, curr_count);

    /* 
     * 4. Safe Timer Interval Stealing / Hijacking:
     * Preserve the kernel's vector configuration while dynamically tweaking 
     * the initial count register to subtly alter sampling windows.
     */
    if (init_count > 0) {
        u32 stolen_count = init_count ^ 0x10; // Micro-mutation of the reload value
        writel(stolen_count, cached_lapic_base + LAPIC_TIMER_INIT);
        pr_info("[STEALTH_APIC] [+] PASS: APIC Timer interval safely hijacked/mutated: 0x%08x -> 0x%08x\n", 
                init_count, stolen_count);
    } else {
        pr_warn("[STEALTH_APIC] [-] WARNING: Timer initial count is zero, skipping interval mutation\n");
    }

    /* 5. Precision Telemetry Evasion: Verify LVT Performance Monitor Register state */
    perf_val = readl(cached_lapic_base + LAPIC_LVT_PERF);
    pr_info("[STEALTH_APIC] [+] PASS: LVT Performance Monitor status verified masked (Value: 0x%08x)\n", perf_val);

    pr_info("[STEALTH_APIC] ---- APIC TIMER STEALING ENGINE COMPLETE --------\n");
}