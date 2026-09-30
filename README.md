# AHCI-DCO-EXPLOITATION

> **Advanced Bare-Metal AHCI Out-of-Band Device Configuration Overlay (DCO) Manipulation & Stealth Framework**

## Overview

**AHCI-DCO-EXPLOITATION** is a low-level systems research toolkit designed to perform hardware-level capacity truncation and tail sector manipulation on SATA storage drives entirely **under the radar**. By bypassing the Linux kernel's high-level `libata` block layer and interacting directly with the AHCI Host Controller's Memory-Mapped I/O (MMIO) registers, this toolkit achieves complete invisibility across software, kernel module registries, physical memory, and CPU cache lines.

---

## Core Architecture & Algorithmic Breakdown

The toolkit relies on five distinct architectural pillars to ensure absolute operational stealth and execution integrity:

### 1. Pure-MMIO Execution Engine (Bypassing `libata`)

Traditional storage operations route through the kernel's `libata` driver and block layer, leaving deep telemetry traces in I/O queues and scheduler logs.

* **The Algorithm:** The toolkit maps the AHCI Base Address Register (ABAR) directly via `ioremap()`, bypassing kernel block abstractions. It constructs native command frames—including SATA Host-to-Device Register FIS (`0x27`) and PRDT (Physical Region Descriptor Table) entries—and injects them directly into arbitrary hardware command slots (Slot 31) in the Port Command List.

### 2. Hardware Interrupt Masking (`PxIE` Blinding)

The Linux kernel monitors drive activity primarily through hardware interrupts. When a command completes, the controller fires an interrupt handled by `libata`'s Interrupt Service Routine (ISR).

* **The Algorithm:** Immediately prior to firing a command slot, the module reads and backs up the Port Interrupt Enable (`PxIE`) register and writes `0` to it. This commands the AHCI controller to withhold all interrupt signals for that port from the CPU. The kernel's ISR remains completely blind while the command executes out-of-band via manual polling of the Task File Data (`PxTFD`) and Command Issue (`PxCI`) registers.

### 3. Transient Execution (Zero-Resident Module Footprint)

Standard kernel modules remain resident in memory, showing up in `lsmod`, `/proc/modules`, and `/sys/module/`, making them trivial to detect via host-based introspection.

* **The Algorithm:** The module utilizes a transient execution model. The core DCO micro-trim logic is housed entirely within the module initialization (`__init`) function. Once the hardware operations successfully commit and tracks are scrubbed, the function returns **`-ENODEV`** ("No such device"). This forces the kernel module loader to instantly abort registration, discarding the binary from memory. The result is **zero persistent kernel module footprint** while the hardware configuration persists in the drive's non-volatile NVRAM.

### 4. DCO Micro-Trim Sequence

The capacity manipulation sequence is split into two precise phases:

* **Phase 1 (Tail Sector Inspection):** Issues a native `READ DMA EXT` (`0x25`) command targeting the ultimate sector of the drive (`Native Max LBA - 1`). It validates drive responsiveness and inspects structural boundaries (e.g., verifying GPT backup headers) before modification.
* **Phase 2 (Capacity Truncation):** Issues a Device Configuration Set (`0xB1` / `0xC2`) command with a reduced maximum LBA boundary (`Native Max LBA - 1000 sectors`). This permanently clips the addressable capacity at the hardware firmware level.

### 5. Memory Forensics & CPU Cache Sanitization

Advanced memory dumpers and cold-boot forensic tools can recover sensitive payloads, command structures, and target LBAs from physical RAM and CPU cache lines post-execution.

* **The Algorithm:**
* **Cache Flushing:** Employs `clflush_cache_range()` to forcefully push dirty cache lines out to physical RAM and invalidate L1/L2/L3 CPU caches.
* **Explicit Memory Wiping:** Uses `memzero_explicit()` to securely overwrite all allocation pages and command headers with zeroes, ensuring no compiler optimizations eliminate the wipe.
* **Link Noise Elimination:** Reads and clears the Port Error Register (`PxSERR`) and pending interrupt flags (`PxIS`) post-execution to prevent `libata` from throwing `qc_active` warning artifacts when the physical link re-synchronizes.



---

## Project Structure

```text
ahci-dco-exploitation/
├── ahci_cmd_builder.c   # Core transient pure-MMIO kernel module
├── Makefile             # Kernel module compilation targets
├── krun.sh              # Automated build, transient injection & verification suite
└── README.md            # Project documentation

```

---

## Automation & Verification Suite (`krun.sh`)

The framework includes a fully automated testing suite (`krun.sh`) that compiles the transient module, injects it, audits module invisibility, parses kernel telemetry, and verifies memory forensics defenses with vibrant color-coding.

Run the suite with root privileges:

```bash
sudo ./krun.sh

```

### Sample Execution Output

```text
╔══════════════════════════════════════════════════════╗
║        AHCI PURE MMIO STEALTH DCO TEST SUITE         ║
╚══════════════════════════════════════════════════════╝

[*] Step 1: Compiling kernel module (ahci_cmd_builder.ko)...
[+] BUILD SUCCESSFUL: Compilation clean and verified.

[*] Step 2: Injecting module (Transient Stealth Mode)...
insmod: ERROR: could not insert module ./ahci_cmd_builder.ko: No such device
[+] EXECUTION COMPLETE: Module ran and self-aborted registration as designed.

[*] Step 3: Verifying Module Hiding (lsmod Footprint)...
[+] PASS: Module is completely invisible in lsmod (Zero Resident Footprint).

[*] Step 4: Auditing Kernel Logs & Link Noise (dmesg)...
[+] PASS: DCO micro-trim command executed and confirmed by hardware.
[+] PASS: Zero link noise, bus errors, or qc_active warnings detected.
    (The PxSERR cleanup successfully blinded the kernel storage driver.)

[*] Step 5: Validating Memory Forensics & Cache Sanitization...
[+] PASS: clflush_cache_range detected: CPU cache lines are actively flushed.
[+] PASS: memzero_explicit detected: Physical RAM buffers are securely zeroed.
[+] PASS: Memory Forensics Layer defense verified (No residue left in RAM or CPU cache).

╔══════════════════════════════════════════════════════╗
║        ALL STEALTH & FORENSIC CHECKS PASSED!         ║
╚══════════════════════════════════════════════════════╝

```

---

## Disclaimer

This software is developed strictly for **educational purposes, low-level systems research, and advanced storage forensics**. Misuse of this software on production systems or hardware without explicit authorization may result in permanent data loss or hardware configuration alteration.