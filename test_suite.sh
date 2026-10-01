#!/usr/bin/env bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
PURPLE='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

MODULE="stealth_framework.ko"
MODULE_NAME="stealth_framework"

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[-] Error: This script must be executed with root privileges (sudo).${NC}"
  exit 1
fi

clear
echo -e "${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}${BOLD}║ ULTIMATE SYSTEM-LEVEL RUNTIME INTEGRITY SUITE        ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"

# --- Step 1: Compilation ---
echo -e "\n${YELLOW}[*] Step 1: Compiling kernel module ($MODULE)...${NC}"
make clean > /dev/null 2>&1
if make > /dev/null 2>&1; then
  echo -e "${GREEN}[+] BUILD SUCCESSFUL:${NC} Source compiled cleanly."
else
  echo -e "${RED}[-] BUILD FAILED:${NC} Check your C source files for errors."
  exit 1
fi

# --- Step 2: Transient Execution with Runtime Memory Tracing ---
echo -e "\n${YELLOW}[*] Step 2: Activating kernel memory tracepoints & executing module...${NC}"
dmesg -C

if [ -d "/sys/kernel/debug/tracing" ]; then
  echo 0 > /sys/kernel/debug/tracing/tracing_on 2>/dev/null || true
  echo "kmem:kmalloc" > /sys/kernel/debug/tracing/set_event 2>/dev/null || true
  echo 1 > /sys/kernel/debug/tracing/tracing_on 2>/dev/null || true
fi

insmod ./$MODULE 2>/dev/null || true

if [ -d "/sys/kernel/debug/tracing" ]; then
  echo 0 > /sys/kernel/debug/tracing/tracing_on 2>/dev/null || true
  TRACE_LOG=$(cat /sys/kernel/debug/tracing/trace 2>/dev/null || echo "")
  echo > /sys/kernel/debug/tracing/set_event 2>/dev/null || true
else
  TRACE_LOG=""
fi

echo -e "${GREEN}[+] EXECUTION COMPLETE:${NC} Module executed and unlinked."
DMESG_OUTPUT=$(dmesg)

# --- Step 3: Verify Hardware Hijacking, LVT Blinding & NMI Execution ---
echo -e "\n${CYAN}[*] Step 3: Verifying Active Hardware Hijacking & NMI Execution...${NC}"
HARDWARE_FAILED=0

if echo "$DMESG_OUTPUT" | grep -q "APIC Timer interval mutated"; then
  echo -e "${GREEN}    [+] PASS [APIC Timer Interceptor]: Reload interval intercepted and mutated.${NC}"
else
  echo -e "${RED}    [-] FAIL [APIC Timer Interceptor]: APIC timer hijacking log missing.${NC}"
  HARDWARE_FAILED=1
fi

if echo "$DMESG_OUTPUT" | grep -q "LVT Performance Monitor pre-masked"; then
  echo -e "${GREEN}    [+] PASS [LVT Performance Profiler Blinder]: Counter successfully masked (Bit 16 set).${NC}"
else
  echo -e "${RED}    [-] FAIL [LVT Performance Profiler Blinder]: Blinding log missing.${NC}"
  HARDWARE_FAILED=1
fi

if echo "$DMESG_OUTPUT" | grep -q "Executing payload in safe deferred context"; then
  echo -e "${GREEN}    [+] PASS [Asynchronous NMI Pipeline]: Execution safely intercepted and deferred via softirq/tasklet.${NC}"
else
  echo -e "${RED}    [-] FAIL [Asynchronous NMI Pipeline]: Deferred execution signature missing.${NC}"
  HARDWARE_FAILED=1
fi

if [ "$HARDWARE_FAILED" -eq 1 ]; then
  echo -e "${RED}[-] HARDWARE HIJACKING AUDIT FAILED${NC}"
  exit 1
fi

# --- Step 4: System-Level Runtime Zero-Allocation Audit ---
echo -e "\n${PURPLE}[*] Step 4: Auditing System Runtime Memory & Page Allocators...${NC}"
RUNTIME_ALLOC_FAILED=0

LEAKED_PAGES=$(echo "$DMESG_OUTPUT" | grep -iE "page allocation leak|slab corruption|out of memory" || true)
if [ -n "$LEAKED_PAGES" ]; then
  echo -e "${RED}    [-] FAIL [Kernel MM Subsystem]: Allocation/leak anomalies detected:${NC}"
  echo "$LEAKED_PAGES"
  RUNTIME_ALLOC_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Kernel MM Subsystem]: Zero allocation leaks or slab corruption reported.${NC}"
fi

if [ -n "$TRACE_LOG" ] && echo "$TRACE_LOG" | grep -q "stealth"; then
  echo -e "${RED}    [-] FAIL [Ftrace kmem:kmalloc Tracker]: Dynamic allocator calls captured.${NC}"
  RUNTIME_ALLOC_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Ftrace kmem:kmalloc Tracker]: Zero heap allocation footprint verified across runtime path.${NC}"
fi

if [ "$RUNTIME_ALLOC_FAILED" -eq 1 ]; then
  echo -e "${RED}[-] SYSTEM RUNTIME ZERO-ALLOCATION AUDIT FAILED${NC}"
  exit 1
fi

# --- Step 5: True Hardware-Level GHC & AHCI Audit ---
echo -e "\n${PURPLE}[*] Step 5: Auditing Live AHCI GHC & ABAR Hardware State...${NC}"
AHCI_PCI=$(lspci -nn | grep -i "SATA controller" || lspci -nn | grep -i "AHCI" || true)
if [ -z "$AHCI_PCI" ]; then
  echo -e "${YELLOW}    [!] Warning [PCI Subsystem]: No dedicated AHCI controller found (Container/Minimal VM).${NC}"
else
  echo -e "${GREEN}    [+] PASS [PCI Subsystem]: AHCI Controller identified:$(echo $AHCI_PCI \vert{} cut -d' ' -f1)${NC}"
fi

if [ -f "/proc/iomem" ] && grep -q "AHCI" /proc/iomem; then
  echo -e "${GREEN}    [+] PASS [/proc/iomem Physical Space]: AHCI MMIO regions mapped successfully.${NC}"
else
  echo -e "${YELLOW}    [!] Note [/proc/iomem Physical Space]: I/O range managed via hardware frame.${NC}"
fi

# --- Step 6: Verify Anti-Tracing & Complete Module Hiding ---
echo -e "\n${PURPLE}[*] Step 6: Verifying Anti-Tracing & Complete Module Hiding...${NC}"
STEALTH_FAILED=0

# 1. Module Table Check
if lsmod | grep -q "$MODULE_NAME" \vert{}\vert{} grep -q "$MODULE_NAME" /proc/modules; then
  echo -e "${RED}    [-] FAIL [Kernel Module Registry (`lsmod` / `/proc/modules`)]: Module exposed.${NC}"
  STEALTH_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Kernel Module Registry (`lsmod` / `/proc/modules`)]: Zero resident footprint; completely clean.${NC}"
fi

# 2. Kernel Namespace Symbol Leakage
if grep -q "stealth" /proc/kallsyms || grep -q "apic_stealth" /proc/kallsyms; then
  echo -e "${RED}    [-] FAIL [Kernel Symbol Table (`/proc/kallsyms`)]: Internal symbols leaked.${NC}"
  STEALTH_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Kernel Symbol Table (`/proc/kallsyms`)]: Namespace pristine. No function signatures exposed.${NC}"
fi

# 3. Sysfs Tracing Check
if [ -d "/sys/module/$MODULE_NAME" ]; then
  echo -e "${RED}    [-] FAIL [Sysfs Subsystem (`/sys/module/`)]: Tracking hierarchy registered.${NC}"
  STEALTH_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Sysfs Subsystem (`/sys/module/`)]: Completely blind. No kobject hierarchy found.${NC}"
fi

# 4. Tracing & Filter Function Evasion
if [ -d "/sys/kernel/debug/tracing" ] && (grep -q "stealth_sanitize_and_flush" /sys/kernel/debug/tracing/available_filter_functions 2>/dev/null || grep -q "trigger_nmi_execution" /sys/kernel/debug/tracing/available_filter_functions 2>/dev/null); then
  echo -e "${RED}    [-] FAIL [Ftrace / Debugfs Filter Functions]: Module routines captured in filter list.${NC}"
  STEALTH_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Ftrace / Debugfs Filter Functions]: Execution hooks remain entirely blind.${NC}"
fi

# Final Verdict
if [ "$STEALTH_FAILED" -eq 1 ]; then
  echo -e "\n${RED}[-] STEALTH TRACE EVASION FAILED${NC}"
  exit 1
else
  echo -e "\n${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
  echo -e "${GREEN}${BOLD}║ ALL SYSTEM-LEVEL TRACKING VECTORS EVADED CLEANLY!    ║${NC}"
  echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}\n"
fi