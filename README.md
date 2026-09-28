# AHCI/ATA Low-Level Storage Research & DCO Exploitation Toolkit

---

## 1. Executive Summary

This repository houses a specialized low-level Linux kernel module and diagnostic framework engineered for direct AHCI (Advanced Host Controller Interface) Memory-Mapped I/O (MMIO) manipulation. The primary objective is research into low-profile, persistent capacity micro-trimming and ATA Device Configuration Overlay (DCO) feature masking at the hardware controller level.

By bypassing traditional driver queues and constructing custom Host-to-Device (H2D) Frame Information Structure (FIS) payloads directly within DMA-safe memory pages, the framework interacts asynchronously with SATA controllers to execute low-level configuration commands entirely outside standard OS storage management layers.

---

## 2. Stealth & Evasion Mechanics

Operating under the radar without triggering kernel panics or alerting system telemetry involves several architectural safeguards:

* **Bypassing Block Layer & Subsystems:** Direct MMIO register injection (`PxCI`) and out-of-band H2D FIS construction execute commands directly at the hardware controller level, bypassing standard `libata` queue management paths.
* **Micro-Profile Capacity Clipping:** Rather than applying aggressive geometry alterations that break existing partition tables, the engine targets a micro-trim (e.g., hiding a minuscule tail of 1,000 sectors). Active filesystems remain fully functional and uncorrupted.
* **Zero Persistent Userspace Artifacts:** No background user-space daemons, open handles, or modified binary wrappers are required. Once the kernel module performs the MMIO injection and unloads, the hardware controller natively retains the configuration state.

---

## 3. Project Architecture & File Manifest

The workspace is organized into core kernel modules for command injection and decoding, alongside helper scripts for build automation and real-time register monitoring:

| Component | Type | Description |
| --- | --- | --- |
| `ahci_cmd_builder.c` | Kernel Module | Core execution engine; maps ABAR, allocates CTBA pages, constructs DCO `B1h`/`C2h` FIS packets, and injects commands via slot 31 (`PxCI`). |
| `ahci_decoder.c` | Kernel Module | Dissects and parses raw AHCI structures and FIS payloads for runtime debugging. |
| `ahci_dump.c` | Kernel Module | Dumps live AHCI host controller registers and port state bitmaps (`PxCMD`, `PxIS`, `PxSERR`). |
| `Makefile` | Build System | Compiles all kernel modules in a single pass using standard kbuild targets. |
| `krun.sh` | Shell Script | Build automation pipeline handling compilation, insertion, ring-buffer logging, and cleanup. |
| `qc_active.sh` | Shell Script | Real-time monitoring script for tracking command issue register states. |
| `scan_ahci_64.py` / `decode_ahci.py` | Python Utilities | Scans 64-bit ABAR address spaces and parses raw ATA taskfile structures. |

---

## 4. Technical Implementation Details

### DCO Micro-Trim & Feature Masking

The `ahci_cmd_builder` module targets SATA Device Configuration Overlay (DCO) commands to adjust native drive geometry parameters invisibly:

* **Command:** ATA `0xB1` (Device Configuration)
* **Subcommand:** `0xC2` (Set Configuration)
* **Payload Structure:**
* Target Max LBA adjusted dynamically (`STEALTH_MAX_LBA = NATIVE_MAX_LBA - TAIL_TRIM_SECTORS`) to clip a controlled tail region.
* Feature configuration flags injected via `count_low` bits (`0x01`) to manage feature visibility masks during subsequent `IDENTIFY DEVICE` sequences.



### Execution Pipeline

1. **ABAR Mapping:** Maps the physical Host Controller Base Address (`0xE1900000`) via `ioremap`.
2. **Command List Resolution:** Reads port base registers (`PxCLB`/`PxCLBU`) to locate the physical command list array.
3. **CTBA Allocation:** Allocates a DMA-safe lower-memory page (`GFP_DMA32`) for the Command Table Base Address.
4. **Slot Injection:** Populates command slot 31 header parameters (Command FIS Length `CFL = 5`, PRDT Length = 0) pointing to the custom FIS.
5. **Hardware Trigger:** Sets bit 31 in the Port Command Issue (`PxCI`) register to initiate direct hardware execution.

---

## 5. Verification & Execution Telemetry

Execution of the build and test sequence yields the following verified diagnostic output captured from the kernel ring buffer (`dmesg`):

```text
vboxuser@ronen:~/ssd-exploit$ sudo ./krun.sh
[+] Compiling modules...
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
make[1]: Entering directory '/usr/src/linux-headers-6.8.0-52-generic'
  CC [0]  ahci_cmd_builder.o
  LD [1]  ahci_cmd_builder.ko
make[1]: Leaving directory '/usr/src/linux-headers-6.8.0-52-generic'
[+] Inserting module...
[+] Reading kernel logs:
[ 2548.910211] [STEALTH_DCO] ==================================================
[ 2548.910213] [STEALTH_DCO] ---- initializing stealth sequence (DETAILED) ----
[ 2548.910214] [STEALTH_DCO] ==================================================
[ 2548.910216] [STEALTH_DCO] Step 1: Mapping ABAR physical address 0xE1900000...
[ 2548.910220] [STEALTH_DCO] ABAR successfully mapped at virtual address 0xffffc90000100000
[ 2548.910221] [STEALTH_DCO] Target Port Index: 0 | Calculated Port Base Offset: 0x100
[ 2548.910223] [STEALTH_DCO] Step 2: Reading Command List Base registers (PxCLB / PxCLBU)...
[ 2548.910225] [STEALTH_DCO] PxCLB low register value  = 0x3af48000
[ 2548.910226] [STEALTH_DCO] PxCLBU high register value = 0x00000000
[ 2548.910228] [STEALTH_DCO] Resolved Command List Physical Address (lst_phys) = 0x000000003af48000
[ 2548.910231] [STEALTH_DCO] Step 3: Allocating DMA-safe memory page for Command Table (CTBA)...
[ 2548.910250] [STEALTH_DCO] CTBA allocated successfully:
[ 2548.910252] [STEALTH_DCO]   -> Virtual Address : 0xffff88810234a000
[ 2548.910253] [STEALTH_DCO]   -> Physical Address: 0x000000010234a000
[ 2548.910255] [STEALTH_DCO] Step 4: Constructing Host-to-Device FIS (DCO B1h / Set C2h)...
[ 2548.910258] [STEALTH_DCO] FIS Structure Populated:
[ 2548.910259] [STEALTH_DCO]   -> Command Code     : 0xB1 (DEVICE CONFIGURATION)
[ 2548.910260] [STEALTH_DCO]   -> Feature Low      : 0xC2 (SET Subcommand)
[ 2548.910262] [STEALTH_DCO]   -> Native Max LBA   : 2097152 sectors
[ 2548.910263] [STEALTH_DCO]   -> Tail Trimmed     : 1000 sectors
[ 2548.910265] [STEALTH_DCO]   -> Stealth Target   : 0x1FFC18 (2096152 active sectors)
[ 2548.910268] [STEALTH_DCO] Step 5: Mapping and populating command list slot 31 header...
[ 2548.910275] [STEALTH_DCO] Slot 31 Header Written Successfully:
[ 2548.910276] [STEALTH_DCO]   -> DW0  = 0x00000005 (CFL=5, PRDTL=0)
[ 2548.910278] [STEALTH_DCO]   -> DW1  = 0x00000000
[ 2548.910279] [STEALTH_DCO]   -> CTBA = 0x00000000010234a0
[ 2548.910281] [STEALTH_DCO] Pre-Execution Register Snapshot:
[ 2548.910283] [STEALTH_DCO]   -> PxCMD before = 0x00000317
[ 2548.910284] [STEALTH_DCO]   -> PxIS before  = 0x00000000
[ 2548.910286] [STEALTH_DCO] Step 6: Ensuring Port Command Engine (ST and FRE) is active...
[ 2548.910289] [STEALTH_DCO] PxCMD updated to: 0x00000317
[ 2548.910291] [STEALTH_DCO] Step 7: Triggering execution. Current PxCI = 0x00000000
[ 2548.910295] [STEALTH_DCO] PxCI written with Slot 31 set (0x80000000). Awaiting controller response...
[ 2548.922380] [STEALTH_DCO] Slot 31 cleared by HBA at iteration 1 (10 ms elapsed)
[ 2548.922383] [STEALTH_DCO] ==================================================
[ 2548.922385] [STEALTH_DCO] POST-EXECUTION TELEMETRY SUMMARY:
[ 2548.922387] [STEALTH_DCO]   -> Final PxCI   = 0x00000000
[ 2548.922389] [STEALTH_DCO]   -> Final PxIS   = 0x00000041
[ 2548.922391] [STEALTH_DCO]   -> Final PxSERR = 0x00000000
[ 2548.922392] [STEALTH_DCO] ==================================================
[ 2548.922395] [STEALTH_DCO] SUCCESS: Stealth DCO micro-trim and feature mask applied successfully.
[ 2548.922397] [STEALTH_DCO] Cleanup: Unmapped slot 31 header memory.
[ 2548.922410] [STEALTH_DCO] Cleanup: Freed CTBA page.
[ 2548.922432] [STEALTH_DCO] Cleanup: Unmapped ABAR region.
[ 2548.922434] [STEALTH_DCO] ---- initialization sequence complete ----
[+] Removing module...

```

---

## 6. Build & Quick Start Instructions

### Compile All Modules

```bash
make

```

### Manual Verification

```bash
sudo insmod ahci_cmd_builder.ko
sudo dmesg | tail -n 30
sudo rmmod ahci_cmd_builder

```# ahci-dco-toolkit
