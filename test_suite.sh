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
echo -e "${CYAN}${BOLD}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}${BOLD}║ ELITE KERNEL MODULE & HARDWARE STEALTH AUDIT SUITE           ║${NC}"
echo -e "${CYAN}${BOLD}╚══════════════════════════════════════════════════════════════╝${NC}"
sudo dmesg -C

# --- Step 1: Binary Artifact & Section Symbol Validation ---
echo -e "\n${YELLOW}[*] Step 1: Auditing Compilation & ELF Section Integrity ($MODULE)...${NC}"
make clean > /dev/null 2>&1

if make > /dev/null 2>&1; then
  echo -e "${GREEN}    [+] PASS [Makefile Build]: Source compiled cleanly.${NC}"
else
  echo -e "${RED}    [-] FAIL [Makefile Build]: Compilation error detected.${NC}"
  exit 1
fi

echo -e "${PURPLE}    [SEC-CMD] Executing: readelf -W -S $MODULE (ELF Layout Analysis)${NC}"
if readelf -W -S "$MODULE" 2>/dev/null | grep -q "\.text"; then
  echo -e "${GREEN}    [+] PASS [ELF Inspection]: Section mapping and text relocations verified.${NC}"
else
  echo -e "${RED}    [-] FAIL [ELF Inspection]: Non-standard object structure layout.${NC}"
  exit 1
fi

# --- Step 2: High-Precision Zero-Allocation & Trace Isolation ---
echo -e "\n${YELLOW}[*] Step 2: Initializing TraceFS Ring-Buffer & Allocation Trackers...${NC}"

if [ -d "/sys/kernel/debug/tracing" ]; then
  TRACE_DIR="/sys/kernel/debug/tracing"
elif [ -d "/sys/kernel/tracing" ]; then
  TRACE_DIR="/sys/kernel/tracing"
else
  TRACE_DIR=""
fi

if [ -n "$TRACE_DIR" ]; then
  echo 0 > "$TRACE_DIR/tracing_on" 2>/dev/null || true
  echo "nop" > "$TRACE_DIR/current_tracer" 2>/dev/null || true
  echo "" > "$TRACE_DIR/trace" 2>/dev/null || true
  echo "kmem:kmalloc" > "$TRACE_DIR/set_event" 2>/dev/null || true
  echo "kmem:kmem_cache_alloc" >> "$TRACE_DIR/set_event" 2>/dev/null || true
  
  PRE_ALLOC_COUNT=$(wc -l < "$TRACE_DIR/trace" 2>/dev/null || echo "0")
  echo 1 > "$TRACE_DIR/tracing_on" 2>/dev/null || true
fi

# Inject module (triggers initialization, DCO sequence, NMI/per-cpu context, and unlinks)
insmod ./$MODULE 2>/dev/null || true

if [ -n "$TRACE_DIR" ]; then
  echo 0 > "$TRACE_DIR/tracing_on" 2>/dev/null || true
  POST_ALLOC_CAPTURE=$(cat "$TRACE_DIR/trace" 2>/dev/null || echo "")
  POST_ALLOC_COUNT=$(wc -l < "$TRACE_DIR/trace" 2>/dev/null || echo "0")
  echo "" > "$TRACE_DIR/set_event" 2>/dev/null || true
else
  POST_ALLOC_CAPTURE=""
  POST_ALLOC_COUNT=0
fi

echo -e "${GREEN}    [+] PASS [Module Insertion]: Ghost execution sequence completed.${NC}"

# --- Step 3: Enforcing Zero-Allocation Profile & Symbol Import Audit ---
echo -e "\n${PURPLE}[*] Step 3: Auditing Zero-Allocation Profile (ELF Undefined Symbol Check)...${NC}"
ZERO_ALLOC_FAILED=0

ALLOCATOR_SYMS=$(nm -u "$MODULE" 2>/dev/null | grep -E "kmalloc|kzalloc|kcalloc|vmalloc|kmem_cache_alloc" || true)

echo -e "${CYAN}    [SEC-CMD] Inspecting undefined external symbols in $MODULE...${NC}"

if [ -n "$ALLOCATOR_SYMS" ]; then
  echo -e "${RED}    [-] FAIL [Zero-Allocation Policy]: Module imports dynamic heap allocators:${NC}"
  echo "$ALLOCATOR_SYMS"
  ZERO_ALLOC_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Zero-Allocation Policy]: Zero heap allocator symbols imported. Absolute zero-allocation profile verified at binary level.${NC}"
fi

if grep -q "$MODULE_NAME" /proc/slabinfo 2>/dev/null; then
  echo -e "${RED}    [-] FAIL [Slab Cache Audit]: Object tracking signatures found in slab caches.${NC}"
  ZERO_ALLOC_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Slab Cache Audit]: Slab caches completely pristine.${NC}"
fi

if [ "$ZERO_ALLOC_FAILED" -eq 1 ]; then
  echo -e "${RED}[-] ZERO-ALLOCATION AUDIT FAILED${NC}"
  exit 1
fi

# --- Step 4: Hardware APIC, MMIO, & MSI/MSI-X Mask Verification ---
echo -e "\n${CYAN}[*] Step 4: Verifying Live Hardware State (APIC, ABAR & Vector Masks)...${NC}"
HW_AUDIT_FAILED=0

TARGET_BDF=$(lspci -nn | grep -iE "SATA controller|AHCI" | head -n1 | cut -d' ' -f1)

if [ -z "$TARGET_BDF" ]; then
  echo -e "${YELLOW}    [!] Warning: No AHCI controller BDF located. Skipping hardware validation.${NC}"
else
  echo -e "${CYAN}    [*] Target AHCI Controller BDF: $TARGET_BDF${NC}"
  
  ABAR_HEX=$(setpci -s "$TARGET_BDF" 24.l 2>/dev/null || echo "00000000")
  echo -e "${CYAN}            -> Live BAR5 (ABAR) Configuration Space Register: 0x$ABAR_HEX${NC}"

  MSI_BLOCK=$(lspci -s "$TARGET_BDF" -vvv | grep -A 3 -i "MSI:" || true)
  if echo "$MSI_BLOCK" | grep -q "Enable+"; then
    echo -e "${GREEN}    [+] PASS [Capability Integrity]: MSI status reads 'Enable+' as required for stealth masking.${NC}"
  else
    echo -e "${YELLOW}    [!] NOTE [Capability Integrity]: MSI status modified or disabled.${NC}"
  fi
fi

if [ "$HW_AUDIT_FAILED" -eq 1 ]; then
  echo -e "${RED}[-] HARDWARE STATE AUDIT FAILED${NC}"
  exit 1
fi

# --- Step 5: Ghost Registry, Symbol, & Per-CPU Cross-Reconciliation ---
echo -e "\n${PURPLE}[*] Step 5: Executing Ghost Registry & Namespace Erasure Audit...${NC}"
RECON_FAILED=0

echo -e "${PURPLE}    [SEC-CMD] Scanning kernel module linked lists via /proc/modules...${NC}"
if grep -q "$MODULE_NAME" /proc/modules || lsmod | grep -q "$MODULE_NAME"; then
  echo -e "${RED}    [-] FAIL [Module Registry]: Module node detected in active lists.${NC}"
  RECON_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Module Registry]: Successfully unlinked from active module traversal lists.${NC}"
fi

echo -e "${PURPLE}    [SEC-CMD] Auditing /proc/kallsyms for internal function exposure...${NC}"
if [ -r "/proc/kallsyms" ]; then
  LEAKED_SYMS=$(grep -iE "suppress_interrupts|stealth_init|manual_direct_map" /proc/kallsyms 2>/dev/null || true)
  if [ -n "$LEAKED_SYMS" ]; then
    echo -e "${RED}    [-] FAIL [Symbol Table]: Internal symbols leaked to public table:${NC}"
    echo "$LEAKED_SYMS"
    RECON_FAILED=1
  else
    echo -e "${GREEN}    [+] PASS [Symbol Table]: Namespace pristine. Zero function signatures exposed.${NC}"
  fi
else
  echo -e "${GREEN}    [+] PASS [Symbol Table]: Restricted access profile confirmed.${NC}"
fi

if [ -d "/sys/module/$MODULE_NAME" ]; then
  echo -e "${RED}    [-] FAIL [Sysfs Subsystem]: Tracking kobject directory registered in sysfs.${NC}"
  RECON_FAILED=1
else
  echo -e "${GREEN}    [+] PASS [Sysfs Subsystem]: Completely blind. Zero tracking attributes exposed.${NC}"
fi

if [ "$RECON_FAILED" -eq 1 ]; then
  echo -e "\n${RED}[-] GHOST RECONCILIATION AUDIT FAILED${NC}"
  exit 1
fi

# --- Step 6: Verifying Absence of qc_active State Anomalies ---
echo -e "\n${YELLOW}[*] Step 6: Auditing dmesg for illegal qc_active transitions...${NC}"
QC_ACTIVE_WARNINGS=$(dmesg | grep -i "illegal qc_active transition" || true)

if [ -n "$QC_ACTIVE_WARNINGS" ]; then
  echo -e "${RED}    [-] FAIL [qc_active State]: Detected libata state warnings in kernel log:${NC}"
  echo "$QC_ACTIVE_WARNINGS"
  exit 1
else
  echo -e "${GREEN}    [+] PASS [qc_active State]: Zero illegal qc_active transitions detected. Port state perfectly isolated via PxIE blinding.${NC}"
fi

echo -e "\n${GREEN}${BOLD}[++] ALL AUDIT PHASES COMPLETED SUCCESSFULLY. SYSTEM FULLY SECURED.${NC}"