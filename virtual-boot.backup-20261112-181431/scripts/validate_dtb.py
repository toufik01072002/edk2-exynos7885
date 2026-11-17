#!/usr/bin/env python3
"""
Validate DTB memory map against DeviceMemoryMap.h reference
"""

import sys
import re

# Reference memory map from DeviceMemoryMap.h (current version)
REFERENCE_MAP = [
    ("Periphs", 0x00000000, 0x15000000, "MMIO"),
    ("RAM Bank 0 Low", 0x80000000, 0x10000000, "RAM"),
    ("BootShim/FD", 0x90000000, 0x00200000, "RESERVED"),
    ("RAM Bank 0 Mid", 0x90200000, 0x27E00000, "RAM"),
    ("TIMA Reserved", 0xB8000000, 0x00200000, "RESERVED"),
    ("RAM Bank 0 High", 0xB8200000, 0x03A00000, "RAM"),
    ("seclog Reserved", 0xC0000000, 0x00080000, "RESERVED"),
    ("RAM Bank 1 Low", 0xC0080000, 0x28F80000, "RAM"),
    ("WiFi/BT Reserved", 0xE9000000, 0x00400000, "RESERVED"),
    ("ABOX Reserved", 0xE9400000, 0x00400000, "RESERVED"),
    ("RAM Bank 1 Mid", 0xE9800000, 0x02800000, "RAM"),
    ("Framebuffer", 0xEC000000, 0x01400000, "RESERVED"),
    ("RAM Bank 1 High", 0xED400000, 0x03800000, "RAM"),
    ("CMA video_stream", 0xF0C00000, 0x04800000, "RESERVED"),
    ("Modem/CP Reserved", 0xF5400000, 0x09800000, "RESERVED"),
    ("RAM Bank 1 VeryHigh", 0xFEC00000, 0x01000000, "RAM"),
    ("GNSS Reserved", 0xFFC00000, 0x00400000, "RESERVED"),
    ("CMA Pool cma-3", 0x8BA000000, 0x01400000, "RESERVED"),
    ("CMA Pool crypto", 0x8BB800000, 0x02000000, "RESERVED"),
    ("CMA Pool video_fr", 0x8BD800000, 0x02800000, "RESERVED"),
    ("RAM Bank 2 Low", 0x880000000, 0x3A000000, "RAM"),
    ("RAM Bank 2 Gap1", 0x8BB400000, 0x00400000, "RAM"),
]

def parse_dts_memory_regions(dts_path):
    """Parse memory regions from DTS file"""
    regions = []
    with open(dts_path, 'r') as f:
        content = f.read()
    
    # Find all memory@ nodes
    memory_matches = re.finditer(r'memory@([0-9a-fA-F]+)\s*\{[^}]*reg\s*=\s*<([^>]+)>', content, re.DOTALL)
    for match in memory_matches:
        name = "memory@" + match.group(1)
        reg_str = match.group(2)
        reg_values = list(map(lambda x: int(x, 0), reg_str.split()))
        for i in range(0, len(reg_values), 4):
            if i + 3 < len(reg_values):
                addr_high = reg_values[i]
                addr_low = reg_values[i+1]
                size_high = reg_values[i+2]
                size_low = reg_values[i+3]
                addr = (addr_high << 32) | addr_low
                size = (size_high << 32) | size_low
                regions.append((name, addr, size))
    
    # Find reserved-memory regions
    reserved_start = content.find('reserved-memory')
    if reserved_start != -1:
        brace_count = 0
        reserved_end = reserved_start
        for i, ch in enumerate(content[reserved_start:], reserved_start):
            if ch == '{':
                brace_count += 1
            elif ch == '}':
                brace_count -= 1
                if brace_count == 0:
                    reserved_end = i + 1
                    break
        reserved_node = content[reserved_start:reserved_end]
        
        for child_match in re.finditer(r'(\w+(?:[@:][0-9a-fA-F]+)?)\s*\{[^}]*reg\s*=\s*<([^>]+)>', reserved_node, re.DOTALL):
            name = child_match.group(1)
            reg_str = child_match.group(2)
            reg_values = list(map(lambda x: int(x, 0), reg_str.split()))
            for i in range(0, len(reg_values), 4):
                if i + 3 < len(reg_values):
                    addr_high = reg_values[i]
                    addr_low = reg_values[i+1]
                    size_high = reg_values[i+2]
                    size_low = reg_values[i+3]
                    addr = (addr_high << 32) | addr_low
                    size = (size_high << 32) | size_low
                    regions.append((name, addr, size))
    
    return regions

def check_overlap(regions):
    """Check for overlapping regions"""
    sorted_regions = sorted(regions, key=lambda x: x[1])
    overlaps = []
    for i in range(len(sorted_regions) - 1):
        name1, addr1, size1 = sorted_regions[i]
        name2, addr2, size2 = sorted_regions[i + 1]
        end1 = addr1 + size1
        if end1 > addr2:
            overlaps.append((name1, addr1, end1, name2, addr2, addr2 + size2))
    return overlaps

def compare_with_reference(dts_regions):
    """Compare DTS regions with reference map"""
    print("=== Memory Map Validation ===\n")
    
    print("DTS Regions:")
    for name, addr, size in sorted(dts_regions, key=lambda x: x[1]):
        end = addr + size - 1
        print(f"  {name:30s}  0x{addr:012X} - 0x{end:012X}  ({size / (1024*1024):.1f} MiB)")
    
    print("\nReference Map:")
    for name, addr, size, typ in REFERENCE_MAP:
        end = addr + size - 1
        print(f"  {name:30s}  0x{addr:012X} - 0x{end:012X}  ({size / (1024*1024):.1f} MiB)  [{typ}]")
    
    overlaps = check_overlap(dts_regions)
    if overlaps:
        print("\n❌ OVERLAPS DETECTED in DTS:")
        for o in overlaps:
            print(f"  {o[0]} (0x{o[1]:X}-0x{o[2]:X}) overlaps {o[3]} (0x{o[4]:X}-0x{o[5]:X})")
    else:
        print("\n✓ No overlaps in DTS regions")
    
    # Check key regions by address/size (name is secondary)
    key_regions = [
        ("RAM Bank 0 Low", 0x80000000, 0x10000000),
        ("RAM Bank 0 Mid", 0x90200000, 0x27E00000),
        ("RAM Bank 0 High", 0xB8200000, 0x03A00000),
        ("RAM Bank 1 Low", 0xC0080000, 0x28F80000),
        ("RAM Bank 1 Mid", 0xE9800000, 0x02800000),
        ("RAM Bank 1 High", 0xED400000, 0x03800000),
        ("RAM Bank 1 VeryHigh", 0xFEC00000, 0x01000000),
        ("RAM Bank 2 Low", 0x880000000, 0x3A000000),
        ("RAM Bank 2 Gap1", 0x8BB400000, 0x00400000),
        ("UEFI FD (BootShim)", 0x90000000, 0x00200000),
        ("TIMA Reserved", 0xB8000000, 0x00200000),
        ("seclog Reserved", 0xC0000000, 0x00080000),
        ("WiFi/BT Reserved", 0xE9000000, 0x00400000),
        ("ABOX Reserved", 0xE9400000, 0x00400000),
        ("Framebuffer", 0xEC000000, 0x01400000),
        ("CMA video_stream", 0xF0C00000, 0x04800000),
        ("Modem/CP Reserved", 0xF5400000, 0x09800000),
        ("GNSS Reserved", 0xFFC00000, 0x00400000),
        ("CMA Pool cma-3", 0x8BA000000, 0x01400000),
        ("CMA Pool crypto", 0x8BB800000, 0x02000000),
        ("CMA Pool video_fr", 0x8BD800000, 0x02800000),
    ]
    
    print("\n=== Key Region Validation ===")
    all_ok = True
    for key_name, exp_addr, exp_size in key_regions:
        found = False
        for name, addr, size in dts_regions:
            if addr == exp_addr and size == exp_size:
                print(f"✓ {key_name}: 0x{addr:X} size 0x{size:X} - MATCHES (found as '{name}')")
                found = True
                break
        if not found:
            print(f"❌ {key_name}: EXPECTED 0x{exp_addr:X} size 0x{exp_size:X} - NOT FOUND")
            all_ok = False
    
    return len(overlaps) == 0 and all_ok

if __name__ == "__main__":
    dts_path = "/data/data/com.termux/files/home/edk2-exynos7885/virtual-boot/dtb/exynos7885-a205w.dts"
    regions = parse_dts_memory_regions(dts_path)
    ok = compare_with_reference(regions)
    sys.exit(0 if ok else 1)
