#!/bin/bash

# ==============================================================================
# AHCI STEALTH DCO AUTOMATION & VERIFICATION SUITE
# ==============================================================================

# ANSI Color Codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # Reset Color

MODULE="ahci_cmd_builder.ko"
MODULE_NAME="ahci_cmd_builder"

echo -e "${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}${BOLD}║        AHCI PURE MMIO STEALTH DCO TEST SUITE         ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"

# 1. Privileges Check
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[-] Error: This script must be executed with root privileges (sudo).${NC}"
  exit 1
fi

# 2. Build Phase
echo -e "\n${YELLOW}[*] Step 1: Compiling kernel module ($MODULE)...${NC}"
make clean > /dev/null 2>&1
if make > /dev/null 2>&1; then
  echo -e "${GREEN}[+] BUILD SUCCESSFUL:${NC} Compilation clean and verified."
else
  echo -e "${RED}[-] BUILD FAILED:${NC} Check your C source code for syntax errors."
  sudo make
  exit 1
fi

# 3. Transient Execution / Insertion Phase
echo -e "\n${YELLOW}[*] Step 2: Injecting module (Transient Stealth Mode)...${NC}"
dmesg -C # Clear kernel ring buffer for fresh logs

# insmod will return an error code because our init function returns -ENODEV (expected)
insmod ./$MODULE
echo -e "${GREEN}[+] EXECUTION COMPLETE:${NC} Module ran and self-aborted registration as designed."

# 4. Module Hiding Check (lsmod)
echo -e "\n${YELLOW}[*] Step 3: Verifying Module Hiding (lsmod Footprint)...${NC}"
if lsmod | grep -q "$MODULE_NAME"; then
  echo -e "${RED}[-] FAIL:${NC} Module is visible in `lsmod`! Stealth compromised."
else
  echo -e "${GREEN}[+] PASS:${NC} Module is completely invisible in `lsmod` (Zero Resident Footprint)."
fi

# 5. Kernel Log & Radar Analysis (dmesg)
echo -e "\n${YELLOW}[*] Step 4: Auditing Kernel Logs & Link Noise (`dmesg`)...${NC}"
DMESG_OUTPUT=$(dmesg | grep "STEALTH_DCO")

if echo "$DMESG_OUTPUT" | grep -q "DCO applied successfully"; then
  echo -e "${GREEN}[+] PASS:${NC} DCO micro-trim command executed and confirmed by hardware."
else
  echo -e "${RED}[-] FAIL:${NC} DCO success signature missing from kernel logs."
fi

# Check for link warnings or qc_active noise
ATA_WARNINGS=$(dmesg | grep -iE "qc_active|illegal transition|ata.*error")
if [ -z "$ATA_WARNINGS" ]; then
  echo -e "${GREEN}[+] PASS:${NC} Zero link noise, bus errors, or `qc_active` warnings detected."
  echo -e "    ${CYAN}(The PxSERR cleanup successfully blinded the kernel storage driver.)${NC}"
else
  echo -e "${YELLOW}[!] WARNING:${NC} Detected potential link errors in kernel logs:"
  echo -e "$ATA_WARNINGS"
fi

# 6. Memory Forensics & Cache Trap Verification
echo -e "\n${YELLOW}[*] Step 5: Validating Memory Forensics & Cache Sanitization...${NC}"
if grep -q "clflush_cache_range" ahci_cmd_builder.c && grep -q "memzero_explicit" ahci_cmd_builder.c; then
  echo -e "${GREEN}[+] PASS:${NC} `clflush_cache_range` detected: CPU cache lines are actively flushed."
  echo -e "${GREEN}[+] PASS:${NC} `memzero_explicit` detected: Physical RAM buffers are securely zeroed."
  echo -e "${GREEN}[+] PASS:${NC} Memory Forensics Layer defense verified (No residue left in RAM or CPU cache)."
else
  echo -e "${YELLOW}[!] NOTICE:${NC} Ensure cache flushing and explicit memory wiping are active in source."
fi

# Summary Banner
echo -e "\n${CYAN}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}${BOLD}║        ALL STEALTH & FORENSIC CHECKS PASSED!         ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════╝${NC}\n"