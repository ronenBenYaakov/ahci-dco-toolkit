#!/usr/bin/env python3
import subprocess
import sys

BAR5_BASE = 0xF0806000

def read_mmio(offset):
    addr = BAR5_BASE + offset
    try:
        cmd = f"sudo busybox devmem 0x{addr:08X} 32"
        res = subprocess.check_output(cmd, shell=True, stderr=subprocess.DEVNULL).decode().strip()
        return int(res, 16)
    except Exception:
        return 0

def main():
    print("=== AHCI 64-bit Physical MMIO Register Scanner ===")
    
    # Check Generic Host Control registers
    ghc = read_mmio(0x04)
    pi  = read_mmio(0x0C)  # Ports Implemented
    
    print(f"Global Host Control (GHC) : 0x{ghc:08X}")
    print(f"Ports Implemented (PI)    : 0x{pi:08X}\n")

    if pi == 0:
        print("[!] Could not read Ports Implemented. Verify root privileges and BAR5 address.")
        sys.exit(1)

    for port in range(32):
        if pi & (1 << port):
            port_base = 0x100 + (port * 0x80)
            clb  = read_mmio(port_base + 0x00)  # Command List Base Low
            clbu = read_mmio(port_base + 0x04)  # Command List Base High
            fb   = read_mmio(port_base + 0x08)  # Received FIS Base Low
            fbu  = read_mmio(port_base + 0x0C)  # Received FIS Base High
            cmd  = read_mmio(port_base + 0x18)  # Port Command / Status

            # Reconstruct 64-bit addresses
            clb_64 = (clbu << 32) | clb
            fb_64  = (fbu << 32) | fb

            print(f"[+] Port {port} Configured:")
            print(f"    - Command List Base (Low)  : 0x{clb:08X}")
            print(f"    - Command List Base (High) : 0x{clbu:08X}")
            print(f"    => Full 64-bit Physical RAM : 0x{clb_64:09X}")
            print(f"    - Received FIS Base        : 0x{fb_64:09X}")
            print(f"    - Port Command Status      : 0x{cmd:08X} (ST={cmd & 1}, FRE={(cmd >> 4) & 1})\n")

if __name__ == "__main__":
    main()