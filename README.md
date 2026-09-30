# AHCI-DCO-EXPLOITATION

> **Advanced Bare-Metal AHCI Out-of-Band Device Configuration Overlay (DCO) Manipulation & Stealth Framework**

## Overview

**AHCI-DCO-EXPLOITATION** is a low-level systems research toolkit designed to perform hardware-level capacity truncation and tail sector manipulation on SATA storage drives entirely **under the radar**. By bypassing the Linux kernel's high-level `libata` block layer and interacting directly with the AHCI Host Controller's Memory-Mapped I/O (MMIO) registers, this toolkit achieves complete invisibility across software, kernel module registries, physical memory, and CPU cache lines.

---

## Core Architecture & Algorithmic Breakdown

The toolkit relies on seven distinct architectural pillars to ensure absolute operational stealth, universal compatibility, and execution integrity:

### 1. Universal PCI ABAR Resolution & Fallback Engine

To interface with the AHCI controller's memory space without relying on internal `libata` driver state structures that vary across kernel versions:

* **Dynamic PCI Scanner:** Iterates through active PCI storage controllers (`PCI_CLASS_STORAGE_SATA`), resolving Base Address Register 5 (BAR5) dynamically via `pci_resource_start()`.
* **Hypervisor-Aware Fallback:** If dynamic scanning is restricted or missed in specialized environments (such as VirtualBox), the engine seamlessly falls back to pre-defined architectural maps (e.g., ICH9 ABAR at `0xe1900000ULL`) via standard `ioremap()`.

### 2. Manual Direct Page Table Walk Engine (`CR3` Traversal)

To map controller-allocated physical addresses (like command lists and slot buffers) directly into kernel virtual memory without triggering standard kernel tracking hooks:

* **Hardware CR3 Inspection:** Reads the active page directory root via `read_cr3_pa()`.
* **Low-Level 4-Level Traversal:** Manually walks the full paging hierarchy ($\text{PGD} \rightarrow \text{P4D} \rightarrow \text{Pud} \rightarrow \text{PMD} \rightarrow \text{PTE}$), handling huge pages (`_PAGE_PSE`) to resolve physical addresses straight to usable virtual MMIO pointers.

### 3. Zero-Allocation Reused Buffer Architecture

Traditional kernel drivers allocate dedicated memory buffers via `kmalloc` or `dma_alloc_coherent`, leaving heavy forensic footprints.

* **Controller Piggybacking:** The module queries the active AHCI Command List (`PxCLB`) established by the firmware/BIOS.
* **Buffer Borrowing:** It maps existing pre-allocated command slots (Slot 0 and Slot 31), extracting their existing Command Table Buffer Addresses (`CTBA`) and Physical Region Descriptor Table (`PRDT`) fields to act as scratchpads, ensuring **zero new memory allocations**.

### 4. Hardware Interrupt Masking (`PxIE` Blinding) & Polling

The Linux kernel monitors drive activity primarily through hardware interrupts handled by `libata`.

* **The Algorithm:** Immediately before issuing a command, the module backs up and zeroes out the Port Interrupt Enable (`PxIE`) register. This commands the AHCI controller to withhold all completion interrupts from the CPU. The framework then executes an active polling loop against the Task File Data (`PxTFD`) and Command Issue (`PxCI`) registers to monitor completion out-of-band.

### 5. Transient Execution (Zero-Resident Module Footprint)

Standard kernel modules remain resident in memory (`lsmod`, `/proc/modules`), making them trivial to detect.

* **The Algorithm:** The entire exploitation and DCO payload executes sequentially inside the module initialization (`__init`) function. Upon completion, the module returns **`-ENODEV`**, forcing the kernel module loader to instantly abort registration and discard the binary from memory. The hardware configuration remains permanently altered in non-volatile NVRAM while software memory traces vanish.

### 6. DCO Micro-Trim Sequence

The capacity manipulation workflow executes in two controlled phases:

* **Phase 1 (Tail Sector Inspection):** Issues a native `READ DMA EXT` (`0x25`) command targeting the ultimate sector of the drive (`Native Max LBA - 1`) to verify drive responsiveness and inspect structural boundaries (e.g., validating backup GPT headers).
* **Phase 2 (Capacity Truncation):** Issues a Device Configuration Set (`0xB1` / `0xC2`) command with a reduced maximum LBA boundary (`STEALTH_MAX_LBA`), permanently clipping the addressable capacity at the hardware firmware level.

### 7. Memory Forensics & CPU Cache Sanitization

Advanced forensic tools can extract sensitive payloads, target LBAs, and command structures from physical RAM and CPU cache lines post-execution.

* **Cache Flushing:** Employs `clflush_cache_range()` to forcefully push dirty cache lines out to physical RAM and invalidate L1/L2/L3 CPU caches.
* **Explicit Memory Wiping:** Uses `memzero_explicit()` to securely overwrite all shared control buffers with zeroes.
* **Link Noise Elimination:** Clears the Port Error Register (`PxSERR`) and pending interrupt flags (`PxIS`) post-execution to prevent `libata` from throwing `qc_active` warnings when the link re-synchronizes.

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

---

## Disclaimer

> **Educational & Research Notice**: This module interacts directly with low-level storage controller hardware and modifies disk configuration overlays. Improper use or incorrect LBA calculations can result in data loss or filesystem corruption. Use strictly in controlled laboratory environments.