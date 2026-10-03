# AHCI-DCO-EXPLOITATION

> **Advanced Bare-Metal AHCI Out-of-Band Device Configuration Overlay (DCO) Manipulation & Stealth Framework**

## Overview

**AHCI-DCO-EXPLOITATION** is a low-level systems research toolkit designed to perform hardware-level capacity truncation and tail sector manipulation on SATA storage drives entirely **under the radar**. By bypassing the Linux kernel's high-level `libata` block layer and interacting directly with the AHCI Host Controller's Memory-Mapped I/O (MMIO) registers—combined with hardware-level CPU silicon manipulation via the Local APIC and asynchronous NMI trapping—this toolkit achieves complete invisibility across software, kernel module registries, physical memory, CPU cache lines, and hardware performance monitoring profilers.

---

## Categorized Architecture, Engineering Analysis, Real-World Analogies, & Bypassed Tracking Systems

The underlying mechanisms are categorized into **Kernel Subsystem Bypassing & Stealth**, **Low-Level Hardware & MMIO Manipulation**, and **CPU Architecture, APIC, & Silicon-Level Execution**—complete with real-world analogies and the specific tracking systems each mechanism evades.

---

### Category 1: Kernel Subsystem Bypassing & Stealth

This category deals with evading traditional operating system visibility structures, kernel allocators, and forensic tracing mechanisms.

#### 1. Transient Execution (Zero-Resident Module Footprint)

* **The Engineering Smartness:** Standard kernel modules register themselves with the module subsystem, creating permanent footprints in `/proc/modules`, `lsmod`, and `/sys/module/`. This engine executes its entire payload inside the module initialization (`__init`) routine and immediately returns **`-ENODEV`**, forcing the kernel loader to abort registration, unlink internal references, and discard binary memory pages.
* **Real-World Analogy:** A ghost tenant entering an apartment building, performing a quick modification to the walls, and leaving instantly before the landlord can write their name on the lease or add them to the directory.
* **Bypassed Tracking System & Why:** Bypasses **`lsmod`, `/proc/modules`, and sysfs kobject registries (`/sys/module/`)**. *Why:* Returning `-ENODEV` signals failure to the kernel's module loader (`load_module`), causing it to instantly purge all internal module structures and tracking nodes from kernel linked lists.

#### 2. Zero-Allocation Reused Buffer Architecture

* **The Engineering Smartness:** Standard kernel modules allocate tracking buffers via `kmalloc` or `dma_alloc_coherent`, leaving immutable signatures in kernel slab caches (`/proc/slabinfo`). Instead, this engine queries the active AHCI Command List (`PxCLB`) established by firmware, "borrows" pre-allocated slots (Slot 0 and Slot 31), and repurposes their existing Command Table Buffer Addresses (`CTBA`) and PRDT fields.
* **Real-World Analogy:** Walking into a busy restaurant and sitting down at a booth someone else just stepped away from for a second, swapping out the menu on top rather than ordering new furniture from a supplier.
* **Bypassed Tracking System & Why:** Bypasses **Kernel Slab Allocators (`/proc/slabinfo`), Kmemleak, and dynamic memory tracking hooks (`kmem:kmalloc`)**. *Why:* Because zero dynamic heap allocation functions (`kmalloc`, `kzalloc`, `vmalloc`) are invoked, no object tracking records or allocation signatures are ever written to slab caches.

#### 3. Ghost Registry, Symbol, & Namespace Erasure

* **The Engineering Smartness:** Even if a module unlinks its primary registration, exported or internal function names can leak into `/proc/kallsyms`. The framework isolates its codebase, avoids exporting symbols, and ensures zero kobject/sysfs directory registrations are generated.
* **Real-World Analogy:** A secret agent operating inside a government agency who uses no official badge, leaves no name on office doors, and speaks in code so personnel directories find nothing.
* **Bypassed Tracking System & Why:** Bypasses **Kernel Symbol Tables (`/proc/kallsyms`) and Debug/Trace Symbol Scanners**. *Why:* By keeping all internal helper functions unexported and statically contained, public symbol resolution tables remain completely unaware of the framework's internal code signatures.

---

### Category 2: Low-Level Hardware, MMIO & Silicon Manipulation

This category handles direct interaction with storage controller silicon, bypassing high-level block layer drivers like `libata`.

#### 4. Manual Direct Page Table Walk Engine (`CR3` Traversal)

* **The Engineering Smartness:** Standard kernel virtual-to-physical address translation APIs can trigger logging hooks or depend on active kernel page tables. This engine reads the active page directory root via `read_cr3_pa()` and manually walks the entire 4-level paging hierarchy ($\text{PGD} \rightarrow \text{P4D} \rightarrow \text{PUD} \rightarrow \text{PMD} \rightarrow \text{PTE}$), explicitly handling huge pages (`_PAGE_PSE`) to resolve physical addresses straight to usable virtual MMIO pointers completely out-of-band.
* **Real-World Analogy:** Navigating a massive labyrinth by checking master archives and street block numbers yourself rather than using the official city tour guide tracking network.
* **Bypassed Tracking System & Why:** Bypasses **Kernel Memory Management Audit Tracing and Virtual-to-Physical Translation Hooks**. *Why:* It sidesteps standard kernel mapping and translation helper routines that might log or monitor memory boundary transformations.

#### 5. Universal PCI ABAR Resolution & Hypervisor Fallback Engine

* **The Engineering Smartness:** AHCI Base Address Register 5 (BAR5) locations vary across motherboards and hypervisors. The dynamic scanner iterates through active PCI storage controllers (`PCI_CLASS_STORAGE_SATA`), resolves BAR5 via `pci_resource_start()`, and disables PCI INTx signaling via the PCI Command Register, falling back to pre-defined architectural maps (`0xe1900000ULL`) in virtual environments.
* **Real-World Analogy:** Finding a secret room in a skyscraper using building blueprints when the electronic lobby directory is missing or restricted.
* **Bypassed Tracking System & Why:** Bypasses **Rigid Device Driver Initialization Watchers and Hypervisor Emulation Monitors**. *Why:* It abstracts away dependency on rigid kernel driver states by querying live PCI configuration space directly and providing a hardcoded architectural failsafe.

#### 6. Advanced Port Interrupt Blinding & PxIE Quiescence (`qc_active` Bypassing)

* **The Engineering Smartness:** When out-of-band commands are injected into storage controllers, the host kernel's `libata` driver normally detects unexpected slot activity and triggers critical errors (`illegal qc_active transition`). To prevent this, the engine dynamically reads active port configurations and clears the Port Interrupt Enable register (`PxIE` at offset `0x14`), completely blinding `libata`'s interrupt service routine during execution.
* **Real-World Analogy:** Blinding the security camera monitoring a specific hallway and temporarily jamming guard walkie-talkies for 5 seconds while moving furniture, then restoring them so no incident reports are filed.
* **Bypassed Tracking System & Why:** Bypasses **`libata` Core Error-Handling Subsystems and Kernel Ring Buffer (`dmesg`) Warning Monitors**. *Why:* Clearing `PxIE` disables port-level interrupt reporting, preventing `libata` from asynchronously observing raw hardware slot state changes or logging state mismatch warnings.

---

### Category 3: CPU Architecture, APIC, & Execution Flow Traps

This category leverages low-level CPU hardware features, interrupt controllers, and cache management to execute payloads invisibly.

#### 7. Asynchronous Non-Maskable Interrupt (NMI) Trapping & Silicon Intercept Architecture

* **The Engineering Smartness:** Standard function calls execute synchronously within normal kernel threads, exposing them to software tracing frameworks. To execute code entirely out-of-band, the toolkit maps the Local APIC physical frame once (`0xFEE00000`) and forces an immediate hardware-level Non-Maskable Interrupt by writing to the Interrupt Command Register (`LAPIC_ICR_LOW` at offset `0x300`). Because NMIs cannot be masked by software (`cli`), execution control is seized instantly on the target core regardless of its current state. A custom NMI handler intercepts the trap, suppresses noisy `dmesg` warnings, and dispatches a lightweight tasklet for safe deferred MMIO execution.
* **Real-World Analogy:** Pulling the master emergency fire alarm in a building to grab everyone's attention instantly, followed by a quiet backstage cleanup crew handling the situation.
* **Bypassed Tracking System & Why:** Bypasses **Software Execution Profilers, Thread-Monitoring Hooks, and Debugger Execution Hooks**. *Why:* NMIs operate at the hardware silicon level, overriding normal thread scheduling and software monitoring hooks. The custom handler intercepts the trap to prevent standard panic logs.

#### 8. Active-Slot Piggybacking & On-the-Fly H2D FIS Payload Swapping

* **The Engineering Smartness:** Instead of creating conflicting command queues, the engine identifies slots currently marked active in `PxCI` (`offset 0x38`), resolves the corresponding Command Table via direct page table walks, and dynamically overwrites the Host-to-Device Frame Information Structure (H2D FIS) payload on-the-fly with target opcodes (such as `0xB1` for DCO).
* **Real-World Analogy:** A spy sliding an envelope into an existing mail carrier's bag just as they walk out the door, replacing the contents of one specific letter mid-transit.
* **Bypassed Tracking System & Why:** Bypasses **Command Queue Monitors and Storage I/O Schedulers**. *Why:* By modifying an already active slot recognized by the firmware, it blends in with legitimate operating system traffic patterns, avoiding unauthorized queue injection alerts.

#### 9. Memory Forensics & CPU Cache Sanitization

* **The Engineering Smartness:** Advanced forensic tools can scrape physical RAM and CPU cache lines post-execution to extract sensitive target LBAs or command layouts. The engine employs `clflush_cache_range()` to forcefully push dirty cache lines out to physical RAM and invalidate CPU caches, followed by `memzero_explicit()` to wipe control buffers and clearing of the Port Error Register (`PxSERR`).
* **Real-World Analogy:** A master thief wiping down every doorknob, desktop, and trash can with bleach and burning blueprints right after finishing a job.
* **Bypassed Tracking System & Why:** Bypasses **RAM Forensic Scrapers, Core Dump Analyzers, and CPU Cache Dump Utilities**. *Why:* Explicitly flushing cache lines (`clflush`) and zeroing buffers (`memzero_explicit`) destroys residual data artifacts before forensic tools can inspect them.

#### 10. DCO Micro-Trim Sequence

* **The Engineering Smartness:** The capacity manipulation workflow executes in two controlled phases: Phase 1 inspects the ultimate sector of the drive (`Native Max LBA - 1`) via a native `READ DMA EXT` (`0x25`) command to validate structural boundaries and backup GPT headers. Phase 2 issues a Device Configuration Set (`0xB1` / `0xC2`) command with a reduced maximum LBA boundary (`STEALTH_MAX_LBA`), permanently clipping addressable capacity at the hardware firmware level.
* **Real-World Analogy:** Going directly to a warehouse's master configuration office and officially rewriting the electronic ledger to state that a container holds fewer boxes, so future inspectors enforce that lower limit.
* **Bypassed Tracking System & Why:** Bypasses **High-Level Filesystem Capacity Verification Checks and OS Partition Tables**. *Why:* Capacity constraints are enforced inside the drive controller's non-volatile NVRAM firmware overlay, making the truncated boundary appear native to any operating system querying the drive.

---

## Configuration Constants

Operational parameters can be fine-tuned via `#define` directives in the source code:

| Constant Name | Default Value | Description |
| --- | --- | --- |
| `STEALTH_MAX_LBA` | `0x0FFFFFFF` | Target maximum addressable logical block address after DCO micro-trim truncation. |
| `TARGET_INSPECT_LBA` | `0x0FFEEFFE` | Sector address targeted during Phase 1 tail inspection and GPT boundary checks. |
| `INTEL_AHCI_VENDOR_SCRATCH_OFFSET` | `0xA0` | Vendor-specific scratchpad register offset for hardware interaction telemetry. |
| `VBOX_AHCI_FALLBACK_PHYS` | `0xe1900000ULL` | Fallback physical MMIO address for virtualized AHCI environments. |

---

## Build & Execution Suite

The framework includes a comprehensive automated validation script (`audit.sh`) that executes a 6-phase verification pipeline:

1. **ELF Section Integrity:** Compiles the source and verifies section layouts using `readelf`.
2. **TraceFS Ring-Buffer Validation:** Initializes kernel tracing to track allocation metrics.
3. **Zero-Allocation Policy Audit:** Confirms zero external heap allocator symbols (`kmalloc`, `kzalloc`, `vmalloc`) are imported.
4. **Hardware State Verification:** Inspects live PCI configuration spaces, BAR5 ABAR pointers, and MSI capability flags.
5. **Ghost Registry Reconciliation:** Verifies that the module is completely unlinked from active module traversal lists (`/proc/modules`, `/sys/module/`).
6. **`qc_active` State Verification:** Scans the kernel ring buffer (`dmesg`) to guarantee zero `illegal qc_active transition` warnings occurred.

---

## Disclaimer

> **Educational and Authorized Research Use Only**
> This software and documentation are provided strictly for educational purposes, low-level systems engineering research, and authorized security testing. The techniques demonstrated involve direct hardware manipulation, capacity truncation, and kernel-level bypassing which can lead to permanent data loss or system instability if executed improperly. Always test within isolated, non-production virtual environments or dedicated research hardware.