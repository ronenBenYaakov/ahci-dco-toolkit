# AHCI-DCO-EXPLOITATION

> **Advanced Bare-Metal AHCI Out-of-Band Device Configuration Overlay (DCO) Manipulation & Stealth Framework**

## Overview

**AHCI-DCO-EXPLOITATION** is a low-level systems research toolkit designed to perform hardware-level capacity truncation and tail sector manipulation on SATA storage drives entirely **under the radar**. By bypassing the Linux kernel's high-level `libata` block layer and interacting directly with the AHCI Host Controller's Memory-Mapped I/O (MMIO) registers—combined with hardware-level CPU silicon manipulation via the Local APIC and asynchronous NMI trapping—this toolkit achieves complete invisibility across software, kernel module registries, physical memory, CPU cache lines, and hardware performance monitoring profilers.

---

## Core Architecture & Algorithmic Breakdown

The toolkit relies on ten distinct architectural pillars to ensure absolute operational stealth, universal compatibility, and execution integrity:

### 1. Universal PCI ABAR Resolution & Fallback Engine

To interface with the AHCI controller's memory space without relying on internal `libata` driver state structures that vary across kernel versions:

* **Dynamic PCI Scanner:** Iterates through active PCI storage controllers (`PCI_CLASS_STORAGE_SATA`), resolving Base Address Register 5 (BAR5) dynamically via `pci_resource_start()`. Additionally disables PCI INTx signaling directly via the PCI Command Register configuration space.
* **Hypervisor-Aware Fallback:** If dynamic scanning is restricted or missed in specialized environments (such as VirtualBox), the engine seamlessly falls back to pre-defined architectural maps (e.g., ICH9 ABAR at `0xe1900000ULL`) via standard `ioremap()`.

### 2. Manual Direct Page Table Walk Engine (`CR3` Traversal)

To map controller-allocated physical addresses (like command lists and slot buffers) directly into kernel virtual memory without triggering standard kernel tracking hooks:

* **Hardware CR3 Inspection:** Reads the active page directory root via `read_cr3_pa()`.
* **Low-Level 4-Level Traversal:** Manually walks the full paging hierarchy ($\text{PGD} \rightarrow \text{P4D} \rightarrow \text{PUD} \rightarrow \text{PMD} \rightarrow \text{PTE}$), handling huge pages (`_PAGE_PSE`) to resolve physical addresses straight to usable virtual MMIO pointers.

### 3. Zero-Allocation Reused Buffer Architecture

Traditional kernel drivers allocate dedicated memory buffers via `kmalloc` or `dma_alloc_coherent`, leaving heavy forensic footprints.

* **Controller Piggybacking:** The module queries the active AHCI Command List (`PxCLB`) established by the firmware/BIOS.
* **Buffer Borrowing:** It maps existing pre-allocated command slots (Slot 0 and Slot 31), extracting their existing Command Table Buffer Addresses (`CTBA`) and Physical Region Descriptor Table (`PRDT`) fields to act as scratchpads, ensuring **zero new memory allocations**.

### 4. On-Chip MMIO Scratchpad Telemetry & Vendor Register Audit

To verify hardware responsiveness and execute control handshakes without invoking high-level storage drivers:

* **Peripheral Interaction Window:** Interacts directly with vendor-specific peripheral register windows (such as the Intel ICH scratchpad offset at `0xA0`).
* **State Verification & Restoration:** Performs non-destructive read-write telemetry checks (`scratch_val_orig ^ 0x5A5A5A5A`) to confirm that the AHCI controller's internal MMIO logic is active and responsive prior to command issuance.

### 5. Global Host Control (GHC) Intercept Suppression & Legacy Wire Isolation

To ensure complete out-of-band execution without triggering asynchronous system interrupts or race conditions with the host OS:

* **Global Interrupt Enable (IE) Cleansing:** Reads the Global Host Control register (`AHCI_GHC`) and clears the Global Interrupt Enable bit (`GHC_IE`).
* **Wire Isolation:** Suppresses legacy wire interrupts at the controller core level, allowing the polling engine to operate in complete isolation.

### 6. Hardware Interrupt Masking (`PxIE` Blinding) & Polling

The Linux kernel monitors drive activity primarily through hardware interrupts handled by `libata`.

* **The Algorithm:** Immediately before issuing a command, the module backs up and zeroes out the Port Interrupt Enable (`PxIE`) register. This commands the AHCI controller to withhold all completion interrupts from the CPU. The framework then executes an active polling loop against the Task File Data (`PxTFD`) and Command Issue (`PxCI`) registers to monitor completion out-of-band.

### 7. Transient Execution (Zero-Resident Module Footprint)

Standard kernel modules remain resident in memory (`lsmod`, `/proc/modules`), making them trivial to detect.

* **The Algorithm:** The entire exploitation and DCO payload executes sequentially inside the module initialization (`__init`) function. Upon completion, the module returns **`-ENODEV`**, forcing the kernel module loader to instantly abort registration and discard the binary from memory. The hardware configuration remains permanently altered in non-volatile NVRAM while software memory traces vanish.

### 8. DCO Micro-Trim Sequence

The capacity manipulation workflow executes in two controlled phases:

* **Phase 1 (Tail Sector Inspection):** Issues a native `READ DMA EXT` (`0x25`) command targeting the ultimate sector of the drive (`Native Max LBA - 1`) to verify drive responsiveness and inspect structural boundaries (e.g., validating backup GPT headers).
* **Phase 2 (Capacity Truncation):** Issues a Device Configuration Set (`0xB1` / `0xC2`) command with a reduced maximum LBA boundary (`STEALTH_MAX_LBA`), permanently clipping the addressable capacity at the hardware firmware level.

### 9. Memory Forensics & CPU Cache Sanitization

Advanced forensic tools can extract sensitive payloads, target LBAs, and command structures from physical RAM and CPU cache lines post-execution.

* **Cache Flushing:** Employs `clflush_cache_range()` to forcefully push dirty cache lines out to physical RAM and invalidate L1/L2/L3 CPU caches.
* **Explicit Memory Wiping:** Uses `memzero_explicit()` to securely overwrite all shared control buffers with zeroes.
* **Link Noise Elimination:** Clears the Port Error Register (`PxSERR`) and pending interrupt flags (`PxIS`) post-execution to prevent `libata` from throwing `qc_active` warnings when the link re-synchronizes.

### 10. APIC-Driven Telemetry Evasion, Asynchronous NMI Triggers, & Deferred Tasklet Execution

To bypass software execution tracing and execute payloads completely out-of-band without triggering kernel panic or thread-monitoring hooks:

* **Persistent MMIO Mapping (`0xFEE00000`):** Maps the Local APIC physical frame once during module load (`init_apic_stealth_subsystem`) and caches the pointer, completely eliminating runtime `ioremap`/`iounmap` page-table churn.
* **LVT Performance Monitor Masking (`0x0340`):** Automatically sets bit 16 of the Local Vector Table Performance Monitor Register during startup, commanding the CPU silicon to ignore and suppress performance monitoring interrupts and cache-miss counters on the active core.
* **Asynchronous NMI Injection (`0x300`):** Forces an immediate hardware-level Non-Maskable Interrupt via the Interrupt Command Register (`LAPIC_ICR_LOW`), transferring execution control directly to the hardware trap frame.
* **Custom NMI Interception & Warning Suppression:** Registers a persistent custom NMI handler (`register_nmi_handler`) that returns `NMI_HANDLED`. This intercepts the asynchronous trap and blocks the kernel's fallback routine (`arch/x86/kernel/nmi.c`) from emitting `"Uhhuh. NMI received for unknown reason..."` warnings in `dmesg`.
* **Deferred Softirq/Tasklet Dispatch (`DECLARE_TASKLET`):** Because hard NMI context cannot sleep or execute page-table operations (`ioremap`), the NMI handler instantly schedules a lightweight tasklet (`tasklet_schedule`). The core DCO payload and MMIO mapping execute safely in deferred softirq context, completely avoiding hard system freezes.
* **Timer Interval Mutation (`0x0380`):** Micro-mutates the core's APIC timer initial reload count (`init_count ^ 0x10`) to disrupt predictable sampling profiler schedules.

---

## Configuration Constants

Operational parameters can be fine-tuned via `#define` directives in the source code:

| Constant | Default Value | Description |
| --- | --- | --- |
| `VBOX_AHCI_FALLBACK_PHYS` | `0xe1900000ULL` | Fallback physical ABAR address for VirtualBox environments. |
| `AHCI_PORT_NUM` | `0` | Target SATA port index to manipulate. |
| `NATIVE_MAX_LBA` | `2097152ULL` | Baseline maximum Logical Block Address of the disk. |
| `TAIL_TRIM_SECTORS` | `1000ULL` | Number of sectors to trim from the tail end. |
| `STEALTH_MAX_LBA` | `NATIVE_MAX_LBA - 1000ULL` | The new restricted maximum LBA enforced via DCO. |
| `LAPIC_BASE_PHYS` | `0xFEE00000ULL` | Physical base address of the CPU Local APIC MMIO frame. |

---

## Disclaimer

> **Educational & Research Notice**: This module interacts directly with low-level storage controller hardware and silicon management registers. Improper use or incorrect LBA calculations can result in data loss or filesystem corruption. Use strictly in controlled laboratory environments.