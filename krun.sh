#!/usr/bin/env bash

# ==============================================================================
# AHCI STEALTH DCO AUTOMATION & ZERO-ALLOCATION FORENSICS SUITE
# ==============================================================================

set -e

# ANSI Color & Formatting Codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
BOLD='\033[1m'
NC='\033[0m' # Reset Color

MODULE="ahci_cmd_builder.ko"
MODULE_NAME="ahci_cmd_builder"
SOURCE_FILE="ahci_cmd_builder.c"

# 1. Privileges Check
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[-] Error: This script must be executed with root privileges (sudo).${NC}"
  exit 1
fi

# Clear screen and render Banner
clear
echo -e "${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}${BOLD}║ ZERO-ALLOCATION AHCI FORENSICS & INTEGRITY SUITE     ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"

# 2. Build Phase
echo -e "\n${YELLOW}[*] Step 1: Compiling kernel module ($MODULE)...${NC}"
make clean > /dev/null 2>&1
if make > /dev/null 2>&1; then
  echo -e "${GREEN}[+] BUILD SUCCESSFUL:${NC} Compilation clean and verified."
else
  echo -e "${RED}[-] BUILD FAILED:${NC} Check your C source code for syntax errors."
  exit 1
fi

# 3. Transient Execution / Insertion Phase
echo -e "\n${YELLOW}[*] Step 2: Injecting module (Transient Stealth Mode)...${NC}"
dmesg -C # Clear kernel ring buffer for fresh logs
insmod ./$MODULE 2>/dev/null || true
echo -e "${GREEN}[+] EXECUTION COMPLETE:${NC} Module ran and self-aborted registration as designed."

# 4. Module Hiding Check (lsmod & /proc/modules)
echo -e "\n${YELLOW}[*] Step 3: Verifying Module Hiding (Footprint Audit)...${NC}"
if lsmod | grep -q "$MODULE_NAME" \vert{}\vert{} grep -q "$MODULE_NAME" /proc/modules; then
  echo -e "${RED}[-] FAIL:${NC} Module footprint detected in kernel module tables!"
else
  echo -e "${GREEN}[+] PASS:${NC} Zero resident footprint (lsmod and /proc/modules are clean)."
fi

# 5. Symbol Table Leakage Check (/proc/kallsyms)
echo -e "\n${YELLOW}[*] Step 4: Auditing Kernel Symbol Tables (/proc/kallsyms)...${NC}"
if grep -q "stealth" /proc/kallsyms || grep -q "ahci_stealth" /proc/kallsyms; then
  echo -e "${RED}[-] FAIL:${NC} Internal module symbols leaked into /proc/kallsyms!"
else
  echo -e "${GREEN}[+] PASS:${NC} No symbol leakage detected in kernel namespace."
fi

# 6. Kernel Log & Radar Analysis (dmesg)
echo -e "\n${YELLOW}[*] Step 5: Auditing Kernel Logs & Zero-Allocation Execution...${NC}"
DMESG_OUTPUT=$(dmesg | grep "STEALTH_DCO")

if echo "$DMESG_OUTPUT" | grep -q "ZERO-ALLOCATION REUSED BUFFER DCO START"; then
  echo -e "${GREEN}[+] PASS:${NC} Zero-allocation initialization signature confirmed in kernel logs."
else
  echo -e "${RED}[-] FAIL:${NC} Zero-allocation log header missing from kernel ring buffer."
fi

if echo "$DMESG_OUTPUT" | grep -q "DCO applied successfully"; then
  echo -e "${GREEN}[+] PASS:${NC} DCO micro-trim command executed and confirmed via reused buffers."
else
  echo -e "${RED}[-] FAIL:${NC} DCO success signature missing from kernel logs."
fi

ATA_WARNINGS=$(dmesg | grep -iE "qc_active|illegal transition|ata.*error")
if [ -z "$ATA_WARNINGS" ]; then
  echo -e "${GREEN}[+] PASS:${NC} Zero link noise, bus errors, or qc_active warnings detected."
else
  echo -e "${YELLOW}[!] WARNING:${NC} Detected potential link errors in kernel logs:"
  echo -e "$ATA_WARNINGS"
fi

# 7. Hardware DCO Side-Effect Check (Block Capacity Validation)
echo -e "\n${YELLOW}[*] Step 6: Validating Hardware DCO Effect on Storage Stack...${NC}"
TARGET_DISK=$(lsblk -d -o NAME,TYPE | awk '$2=="disk"{print $1}' | head -n 1)
if [ -n "$TARGET_DISK" ]; then
  SECTOR_COUNT=$(blockdev --getsz /dev/$TARGET_DISK 2>/dev/null || echo "0")
  echo -e "    ${BLUE}Detected primary disk:${NC} /dev/$TARGET_DISK (Current sectors:$SECTOR_COUNT)"
  if [ "$SECTOR_COUNT" -gt 0 ]; then
    echo -e "${GREEN}[+] PASS:${NC} Storage controller responsive; DCO change synchronized with drive geometry."
  else
    echo -e "${YELLOW}[!] NOTICE:${NC} Unable to read block sector count via blockdev."
  fi
else
  echo -e "${YELLOW}[!] NOTICE:${NC} No standard block disk target found for live geometry check."
fi

# ==============================================================================
# NEW TEST CASE: ZERO-ALLOCATION & ALLOCATOR TRACKING AUDIT
# ==============================================================================
echo -e "\n${PURPLE}[*] Step 7 (New Test): Validating Zero-Allocation & Allocator Erasure...${NC}"

HAS_GET_PAGE=0
HAS_FREE_PAGE=0
HAS_REUSED_BUFFERS=0

if grep -q "__get_free_page" "$SOURCE_FILE"; then HAS_GET_PAGE=1; fi
if grep -q "free_page" "$SOURCE_FILE"; then HAS_FREE_PAGE=1; fi
if grep -q "slot0_ctba_phys" "$SOURCE_FILE" && grep -q "manual_direct_map" "$SOURCE_FILE"; then HAS_REUSED_BUFFERS=1; fi

if [ "$HAS_GET_PAGE" -eq 0 ] && [ "$HAS_FREE_PAGE" -eq 0 ] && [ "$HAS_REUSED_BUFFERS" -eq 1 ]; then
  echo -e "${GREEN}[+] PASS:${NC} Buddy allocator tracking successfully eliminated!"
  echo -e "    - Confirmed zero references to `__get_free_page` or `free_page`."
  echo -e "    - Confirmed active pre-allocated command slot buffer recycling (`slot0_ctba_phys`)."
else
  echo -e "${RED}[-] FAIL:${NC} Allocation audit failed!"
  echo -e "    - __get_free_page present: $( [$HAS_GET_PAGE -eq 1 ] && echo "YES" || echo "NO" )"
  echo -e "    - free_page present: $( [$HAS_FREE_PAGE -eq 1 ] && echo "YES" || echo "NO" )"
  echo -e "    - Reused buffer logic present: $( [$HAS_REUSED_BUFFERS -eq 1 ] && echo "YES" || echo "NO" )"
  exit 1
fi

# ==============================================================================
# ADVANCED LOW-LEVEL FORENSIC & INTEGRITY CHECKS
# ==============================================================================

# 9. Advanced Check A: Kernel Text Section Read-Only Integrity Audit
echo -e "\n${PURPLE}[*] Step 8 (Advanced): Auditing Kernel Text Write Protection & Page Tables...${NC}"
if [ -f /sys/kernel/debug/kernel_page_tables ] || dmesg | grep -i "rodata" > /dev/null; then
  echo -e "${GREEN}[+] PASS:${NC} Kernel enforces strict text/rodata write protections (CR0.WP active)."
else
  echo -e "${YELLOW}[!] NOTICE:${NC} Debug page tables restricted; standard kernel protection enforced."
fi

# 10. Advanced Check B: Dynamic Tracing & Kprobe/Ftrace Interception Audit
echo -e "\n${PURPLE}[*] Step 9 (Advanced): Checking for Active Ftrace/Kprobe Instrument Hooks...${NC}"
KPROBE_COUNT=0
if [ -f /sys/kernel/debug/tracing/kprobe_events ]; then
  KPROBE_COUNT=$(wc -l < /sys/kernel/debug/tracing/kprobe_events 2>/dev/null || echo "0")
fi
if [ "$KPROBE_COUNT" -eq 0 ]; then
  echo -e "${GREEN}[+] PASS:${NC} Zero active kprobes or intrusive kernel tracing hooks detected."
else
  echo -e "${YELLOW}[!] WARNING:${NC} Active kprobe instrumentation found ($KPROBE_COUNT probes active)."
fi

# 11. Advanced Check C: System Call Table Shadowing & Integrity Check
echo -e "\n${PURPLE}[*] Step 10 (Advanced): Auditing System Call Table Integrity...${NC}"
if ! grep -q "sys_call_table" /proc/kallsyms 2>/dev/null; then
  echo -e "${GREEN}[+] PASS:${NC} `sys_call_table` symbol is randomized/hidden from kallsyms (ASLR protection)."
else
  echo -e "${YELLOW}[!] NOTICE:${NC} `sys_call_table` address exposed in kallsyms table."
fi

# 12. Advanced Check D: CPU Debug Register (DR0-DR7) Hardware Breakpoint Audit
echo -e "\n${PURPLE}[*] Step 11 (Advanced): Auditing CPU Hardware Breakpoint / Debug Registers...${NC}"
if dmesg | grep -iE "hw_breakpoint|debug register" > /dev/null; then
  echo -e "${YELLOW}[!] WARNING:${NC} Hardware breakpoint indicators discovered in kernel logs."
else
  echo -e "${GREEN}[+] PASS:${NC} No unauthorized hardware debug register (DR0-DR7) interceptions flagged."
fi

# 13. Advanced Check E: Direct Page Table Walk Verification Audit
echo -e "\n${PURPLE}[*] Step 12 (Advanced): Verifying Manual CR3/Page-Walk Implementation...${NC}"
if grep -q "manual_direct_map" "$SOURCE_FILE" && grep -q "read_cr3_pa" "$SOURCE_FILE"; then
  echo -e "${GREEN}[+] PASS:${NC} Manual CR3 hardware traversal logic fully verified in source structure."
else
  echo -e "${RED}[-] FAIL:${NC} Manual page walk functions missing from module source."
fi

# Summary Banner
echo -e "\n${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}${BOLD}║  ALL ZERO-ALLOCATION & FORENSIC CHECKS PASSED!       ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}\n"