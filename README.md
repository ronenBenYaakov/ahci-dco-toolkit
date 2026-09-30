# AHCI-DCO-EXPLOITATION

> **Advanced Bare-Metal AHCI Out-of-Band Device Configuration Overlay (DCO) Manipulation & Stealth Framework**

## Overview

**AHCI-DCO-EXPLOITATION** is a low-level systems research toolkit designed to perform hardware-level capacity truncation and tail sector manipulation on SATA storage drives entirely **under the radar**. By bypassing the Linux kernel's high-level `libata` block layer and interacting directly with the AHCI Host Controller's Memory-Mapped I/O (MMIO) registers, this toolkit achieves complete invisibility across software, kernel module registries, physical memory, and CPU cache lines.

---

## Core Architecture & Algorithmic Breakdown

The toolkit relies on six distinct architectural pillars to ensure absolute operational stealth, universal compatibility, and execution integrity:

### 1. Universal PCI ABAR Resolution & Fallback Engine

Traditional hardcoded physical addresses or fragile driver-state lookups often fail across different virtualized environments (e.g., VirtualBox, QEMU/KVM) or diverse bare-metal motherboards.

* **The Algorithm:** The module scans all available PCI devices on the bus, inspecting their base classes (`Base Class 0x01` for Mass Storage Controllers). Upon discovering a matching storage controller, it dynamically extracts the BAR5 (ABAR) physical address using standard kernel APIs (`pci_resource_start()`) and maps it safely via `ioremap()`. If hypervisor quirks or firmware limitations obscure the dynamic scan, the engine automatically falls back to standard virtualization memory maps (such as the VirtualBox ICH9 ABAR at `0xe1900000ULL`), ensuring 100% reliability.

### 2. Pure-MMIO Execution Engine (Bypassing `libata`)

Traditional storage operations route through the kernel's `libata` driver and block layer, leaving deep telemetry traces in I/O queues and scheduler logs.

* **The Algorithm:** The toolkit bypasses kernel block abstractions entirely. It constructs native command frames—including SATA Host-to-Device Register FIS (`0x27`) and PRDT (Physical Region Descriptor Table) entries—and injects them directly into arbitrary hardware command slots (Slot 31) in the Port Command List.

### 3. Hardware Interrupt Masking (`PxIE` Blinding)

The Linux kernel monitors drive activity primarily through hardware interrupts. When a command completes, the controller fires an interrupt handled by `libata`'s Interrupt Service Routine (ISR).

* **The Algorithm:** Immediately prior to firing a command slot, the module reads and backs up the Port Interrupt Enable (`PxIE`) register and writes `0` to it. This commands the AHCI controller to withhold all interrupt signals for that port from the CPU. The kernel's ISR remains completely blind while the command executes out-of-band via manual polling of the Task File Data (`PxTFD`) and Command Issue (`PxCI`) registers.

### 4. Transient Execution (Zero-Resident Module Footprint)

Standard kernel modules remain resident in memory, showing up in `lsmod`, `/proc/modules`, and `/sys/module/`, making them trivial to detect via host-based introspection.

* **The Algorithm:** The module utilizes a transient execution model. The core DCO micro-trim logic is housed entirely within the module initialization (`__init`) function. Once the hardware operations successfully commit and tracks are scrubbed, the function returns **`-ENODEV`** ("No such device"). This forces the kernel module loader to instantly abort registration, discarding the binary from memory. The result is **zero persistent kernel module footprint** while the hardware configuration persists in the drive's non-volatile NVRAM.

### 5. DCO Micro-Trim Sequence

The capacity manipulation sequence is split into two precise phases:

* **Phase 1 (Tail Sector Inspection):** Issues a native `READ DMA EXT` (`0x25`) command targeting the ultimate sector of the drive (`Native Max LBA - 1`). It validates drive responsiveness and inspects structural boundaries (e.g., verifying GPT backup headers) before modification.
* **Phase 2 (Capacity Truncation):** Issues a Device Configuration Set (`0xB1` / `0xC2`) command with a reduced maximum LBA boundary (`Native Max LBA - 1000 sectors`). This permanently clips the addressable capacity at the hardware firmware level.

### 6. Memory Forensics & CPU Cache Sanitization

Advanced memory dumpers and cold-boot forensic tools can recover sensitive payloads, command structures, and target LBAs from physical RAM and CPU cache lines post-execution.

* **The Algorithm:**
  * **Cache Flushing:** Employs `clflush_cache_range()` to forcefully push dirty cache lines out to physical RAM and invalidate L1/L2/L3 CPU caches.
  * **Explicit Memory Wiping:** Uses `memzero_explicit()` to securely overwrite all allocation pages and command headers with zeroes, ensuring no compiler optimizations eliminate the wipe.
  * **Link Noise Elimination:** Reads and clears the Port Error Register (`PxSERR`) and pending interrupt flags (`PxIS`) post-execution to prevent `libata` from throwing `qc_active` warning artifacts when the physical link re-synchronizes.

---

## Project Structure

```text
ahci-dco-exploitation/
├── ahci_cmd_builder.c   # Core transient pure-MMIO kernel module with Universal PCI Scanner
├── Makefile             # Kernel module compilation targets
├── krun.sh              # Automated build, transient injection & verification suite
└── README.md            # Project documentation