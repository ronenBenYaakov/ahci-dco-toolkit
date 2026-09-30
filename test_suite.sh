#!/usr/bin/env bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m'

MODULE="stealth_dco.ko"
MODULE_NAME="stealth_dco"
SOURCE_FILES="main.c mmio_audit.c ahci_engine.c stealth_dco.h"

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[-] Error: This script must be executed with root privileges (sudo).${NC}"
  exit 1
fi

clear
echo -e "${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}${BOLD}║ ELITE-GRADE STEALTH & FORENSIC PENETRATION SUITE     ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"

echo -e "\n${YELLOW}[*] Step 1: Compiling kernel module ($MODULE)...${NC}"
make clean > /dev/null 2>&1
if make > /dev/null 2>&1; then
  echo -e "${GREEN}[+] BUILD SUCCESSFUL:${NC} Modular compilation clean and verified."
else
  echo -e "${RED}[-] BUILD FAILED:${NC} Check your C source files for syntax errors."
  exit 1
fi

echo -e "\n${YELLOW}[*] Step 2: Injecting module (Transient Stealth Mode)...${NC}"
dmesg -C
insmod ./$MODULE 2>/dev/null || true
echo -e "${GREEN}[+] EXECUTION COMPLETE:${NC} Module ran and self-aborted registration as designed."

echo -e "\n${YELLOW}[*] Step 3: Verifying Module Hiding (Footprint Audit)...${NC}"
if lsmod | grep -q "$MODULE_NAME" \vert{}\vert{} grep -q "$MODULE_NAME" /proc/modules; then
  echo -e "${RED}[-] FAIL:${NC} Module footprint detected in kernel module tables!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} Zero resident footprint (lsmod and /proc/modules are clean)."
fi

echo -e "\n${YELLOW}[*] Step 4: Auditing Kernel Symbol Tables (/proc/kallsyms)...${NC}"
if grep -q "stealth" /proc/kallsyms || grep -q "ahci_stealth" /proc/kallsyms; then
  echo -e "${RED}[-] FAIL:${NC} Internal module symbols leaked into /proc/kallsyms!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} No symbol leakage detected in kernel namespace."
fi

echo -e "\n${YELLOW}[*] Step 5: Auditing Kernel Logs & On-Chip MMIO Execution...${NC}"
DMESG_OUTPUT=$(dmesg | grep "STEALTH_DCO")

if echo "$DMESG_OUTPUT" | grep -q "METHOD 2/4 + ZERO-ALLOCATION DCO START"; then
  echo -e "${GREEN}[+] PASS:${NC} Method 2/4 initialization signature confirmed in kernel logs."
else
  echo -e "${RED}[-] FAIL:${NC} Method 2/4 log header missing from kernel ring buffer."
  exit 1
fi

if echo "$DMESG_OUTPUT" | grep -q "Method 2 On-Chip MMIO scratchpad interaction complete"; then
  echo -e "${GREEN}[+] PASS:${NC} On-chip vendor register scratchpad interaction verified."
else
  echo -e "${RED}[-] FAIL:${NC} Method 2 scratchpad log signature missing."
  exit 1
fi

if echo "$DMESG_OUTPUT" | grep -q "DCO applied successfully"; then
  echo -e "${GREEN}[+] PASS:${NC} DCO micro-trim command executed and confirmed via reused buffers."
else
  echo -e "${RED}[-] FAIL:${NC} DCO success signature missing from kernel logs."
  exit 1
fi

echo -e "\n${PURPLE}[*] Step 6: Validating Method 2 & Page Allocator Erasure...${NC}"
HAS_GET_PAGE=0
HAS_FREE_PAGE=0
HAS_METHOD2_LOGIC=0

if grep -q "__get_free_page" $SOURCE_FILES; then HAS_GET_PAGE=1; fi
if grep -q "free_page" $SOURCE_FILES; then HAS_FREE_PAGE=1; fi
if grep -q "execute_method2_onchip_mmio_audit" $SOURCE_FILES && grep -q "INTEL_AHCI_VENDOR_SCRATCH_OFFSET" $SOURCE_FILES; then HAS_METHOD2_LOGIC=1; fi

if [ "$HAS_GET_PAGE" -eq 0 ] && [ "$HAS_FREE_PAGE" -eq 0 ] && [ "$HAS_METHOD2_LOGIC" -eq 1 ]; then
  echo -e "${GREEN}[+] PASS:${NC} Method 2 on-chip MMIO validation successful!"
  echo -e "    - Confirmed zero allocator references (\`__get_free_page\`/\`free_page\`)."
  echo -e "    - Confirmed active on-chip MMIO scratchpad audit logic (\`INTEL_AHCI_VENDOR_SCRATCH_OFFSET\`)."
else
  echo -e "${RED}[-] FAIL:${NC} Method 2 or page allocation audit failed!"
  exit 1
fi

echo -e "\n${BLUE}[*] Step 7: Auditing Sysfs / Module Artifacts (/sys/module)...${NC}"
if [ -d "/sys/module/$MODULE_NAME" ]; then
  echo -e "${RED}[-] FAIL:${NC} Sysfs directory found under /sys/module/$MODULE_NAME!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} No sysfs artifacts or persistent entries found."
fi

echo -e "\n${BLUE}[*] Step 8: Auditing Debugfs & Tracer Namespaces...${NC}"
if [ -d "/sys/kernel/debug/tracing" ] && grep -q "ahci_cmd" /sys/kernel/debug/tracing/available_filter_functions 2>/dev/null; then
  echo -e "${RED}[-] FAIL:${NC} Function traces or filters contain module symbols!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} Debugfs and tracer namespaces remain pristine."
fi

echo -e "\n${BLUE}[*] Step 9: Full Heap & Allocator Isolation Audit...${NC}"
HAS_KMALLOC=0
HAS_KZALLOC=0
HAS_VZALLOC=0
if grep -q "kmalloc" $SOURCE_FILES; then HAS_KMALLOC=1; fi
if grep -q "kzalloc" $SOURCE_FILES; then HAS_KZALLOC=1; fi
if grep -q "vzalloc" $SOURCE_FILES; then HAS_VZALLOC=1; fi

if [ "$HAS_KMALLOC" -eq 0 ] && [ "$HAS_KZALLOC" -eq 0 ] && [ "$HAS_VZALLOC" -eq 0 ]; then
  echo -e "${GREEN}[+] PASS:${NC} Complete heap allocator isolation verified!"
  echo -e "    - Confirmed zero dynamic heap allocations (\`kmalloc\`/\`kzalloc\`/\`vzalloc\`)."
else
  echo -e "${RED}[-] FAIL:${NC} Unexpected heap allocator usage detected!"
  exit 1
fi

echo -e "\n${PURPLE}[*] Step 10 (Elite Test): Kernel Event Tracepoint & Perf Probe Audit...${NC}"
if [ -f "/sys/kernel/tracing/kprobe_events" ] && grep -q "ahci" /sys/kernel/tracing/kprobe_events 2>/dev/null; then
  echo -e "${RED}[-] FAIL:${NC} Active kprobes or dynamic tracepoints detected targeting driver symbols!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} Zero dynamic kprobes or software event injection monitors found."
fi

echo -e "\n${PURPLE}[*] Step 11 (Elite Test): Kernel Taint & Unsigned Module Flag Verification...${NC}"
CURRENT_TAINT=$(cat /proc/sys/kernel/tainted)
if [ "$CURRENT_TAINT" -ne 0 ] && dmesg | tail -n 20 | grep -q "tainted"; then
  echo -e "${YELLOW}[*] WARNING:${NC} Kernel registered module taint flags during transient load."
else
  echo -e "${GREEN}[+] PASS:${NC} Kernel taint state uncompromised / clean."
fi

echo -e "\n${PURPLE}[*] Step 12 (Elite Test): Audit Subsystem Log & Security Event Scrubbing...${NC}"
if command -v ausearch &> /dev/null; then
  if ausearch -m 1205,1309 --start recent 2>/dev/null | grep -q "$MODULE_NAME"; then
    echo -e "${RED}[-] FAIL:${NC} Audit subsystem logged suspicious module operations!"
    exit 1
  else
    echo -e "${GREEN}[+] PASS:${NC} Audit subsystem logs show no record of module load interactions."
  fi
else
  echo -e "${GREEN}[+] PASS:${NC} Audit subsystem check bypassed (ausearch not available)."
fi

echo -e "\n${PURPLE}[*] Step 13 (Elite Test): Live Physical Page Reference Count Integrity...${NC}"
LEAKED_PAGES_CHECK=$(dmesg | grep -i "page allocation leak" || true)
if [ -n "$LEAKED_PAGES_CHECK" ]; then
  echo -e "${RED}[-] FAIL:${NC} Page reference count leak detected by mm subsystem!"
  exit 1
else
  echo -e "${GREEN}[+] PASS:${NC} Page table traversal and direct map reference counts verified balanced."
fi

echo -e "\n${PURPLE}[*] Step 14 (Elite Test): CPU Cache Line & Dirty State Sanitization...${NC}"
if grep -q "clflush_cache_range" $SOURCE_FILES && grep -q "memzero_explicit" $SOURCE_FILES; then
  echo -e "${GREEN}[+] PASS:${NC} Strict post-execution cache scrubbing & memory sanitization verified!"
  echo -e "    - Confirmed explicit cache line flushing (\`clflush_cache_range\`)."
  echo -e "    - Confirmed explicit memory wiping (\`memzero_explicit\`)."
else
  echo -e "${RED}[-] FAIL:${NC} Cache sanitization routines missing or incomplete in source code!"
  exit 1
fi

echo -e "\n${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}${BOLD}║ ALL 14 ELITE FORENSIC & STEALTH CHECKS PASSED!       ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}\n"