#!/usr/bin/env python3
"""Strictly validate a Virtual Boot DTB against DeviceMemoryMap.h.

The reference header is authoritative for EDK2 RAM banks and MEM_RES ranges.
A dynamic reserved-memory node is not treated as proof of a fixed physical
reservation unless its alloc-ranges property pins it to that exact range.
"""
import os
import re
import struct
import subprocess
import sys


def run(*args):
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError("{} failed: {}".format(" ".join(args), p.stderr.strip()))
    return p.stdout.strip()


def fdtget(dtb, path, prop, kind="x"):
    p = subprocess.run(["fdtget", "-t", kind, dtb, path, prop],
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        return None
    return p.stdout.strip()


def cells(value):
    if not value:
        return []
    return [int(x, 16) for x in value.split()]


def decode_reg(value, address_cells, size_cells):
    words = cells(value)
    stride = address_cells + size_cells
    if not stride or len(words) % stride:
        raise ValueError("malformed reg/alloc-ranges property: {!r}".format(value))
    result = []
    for i in range(0, len(words), stride):
        address = 0
        size = 0
        for word in words[i:i + address_cells]:
            address = (address << 32) | word
        for word in words[i + address_cells:i + stride]:
            size = (size << 32) | word
        if size:
            result.append((address, address + size))
    return result


def merge_ranges(ranges, reject_overlap=True):
    merged = []
    for start, end in sorted(ranges):
        if end <= start:
            raise ValueError("invalid/empty range 0x{:X}-0x{:X}".format(start, end))
        if merged and start < merged[-1][1]:
            if reject_overlap:
                raise ValueError("reference descriptors overlap at 0x{:X}".format(start))
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        elif merged and start == merged[-1][1]:
            merged[-1] = (merged[-1][0], end)
        else:
            merged.append((start, end))
    return merged


def parse_reference(header_path):
    pattern = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*'
        r'(0x[0-9a-fA-F]+|\d+)\s*,\s*AddMem\s*,\s*(SYS_MEM|MEM_RES)\s*,'
    )
    descriptors = []
    with open(header_path, encoding="utf-8") as src:
        for line in src:
            m = pattern.search(line)
            if not m:
                continue
            name, address, length, kind = m.groups()
            address, length = int(address, 0), int(length, 0)
            if length:
                descriptors.append((name, address, address + length, kind))
    if not descriptors:
        raise ValueError("no AddMem SYS_MEM/MEM_RES descriptors parsed from " + header_path)
    return descriptors


def contained(inner, outer):
    return inner[0] >= outer[0] and inner[1] <= outer[1]


def uncovered_parts(target, covers):
    cursor = target[0]
    gaps = []
    for start, end in sorted(covers):
        if end <= cursor or start >= target[1]:
            continue
        if start > cursor:
            gaps.append((cursor, min(start, target[1])))
        cursor = max(cursor, end)
        if cursor >= target[1]:
            break
    if cursor < target[1]:
        gaps.append((cursor, target[1]))
    return gaps


def main():
    if len(sys.argv) != 3:
        print("Usage: validate_dtb_binary.py <dtb> <DeviceMemoryMap.h>")
        return 2
    dtb, header = sys.argv[1:3]
    if not os.path.isfile(dtb) or not os.path.getsize(dtb):
        raise ValueError("DTB missing or empty: " + dtb)
    if not os.path.isfile(header):
        raise ValueError("reference header missing: " + header)

    with open(dtb, "rb") as f:
        raw = f.read(40)
    if len(raw) != 40 or struct.unpack(">I", raw[:4])[0] != 0xD00DFEED:
        raise ValueError("invalid flattened-device-tree magic/header")
    total_size = struct.unpack(">I", raw[4:8])[0]
    if total_size > os.path.getsize(dtb) or total_size < 40:
        raise ValueError("invalid DTB totalsize {}".format(total_size))

    descriptors = parse_reference(header)
    all_ranges = [(start, end) for _, start, end, _ in descriptors]
    reference_banks = merge_ranges(all_ranges)
    if len(reference_banks) != 3:
        raise ValueError("expected 3 contiguous reference RAM banks, parsed {}".format(
            [(hex(a), hex(e)) for a, e in reference_banks]))
    reference_reserved = [(start, end) for _, start, end, kind in descriptors
                          if kind == "MEM_RES"]
    print("Reference RAM banks:")
    for start, end in reference_banks:
        print("  0x{:X}-0x{:X} ({} MiB)".format(start, end, (end - start) // 0x100000))

    root_addr_cells = int(fdtget(dtb, "/", "#address-cells", "u") or "2", 10)
    root_size_cells = int(fdtget(dtb, "/", "#size-cells", "u") or "1", 10)
    root_nodes = run("fdtget", "-l", dtb, "/").splitlines()
    memory_ranges = []
    for name in root_nodes:
        if not name.startswith("memory@"):
            continue
        path = "/" + name
        device_type = fdtget(dtb, path, "device_type", "s")
        reg = fdtget(dtb, path, "reg", "x")
        if device_type == "memory" and reg:
            memory_ranges.extend(decode_reg(reg, root_addr_cells, root_size_cells))
    memory_ranges = merge_ranges(memory_ranges)
    if memory_ranges != reference_banks:
        raise ValueError("DTB /memory ranges differ from DeviceMemoryMap.h; DTB={} reference={}".format(
            [(hex(a), hex(e)) for a, e in memory_ranges],
            [(hex(a), hex(e)) for a, e in reference_banks]))
    print("PASS: DTB /memory banks match DeviceMemoryMap.h exactly")

    reserved_addr_cells = int(fdtget(dtb, "/reserved-memory", "#address-cells", "u")
                              or str(root_addr_cells), 10)
    reserved_size_cells = int(fdtget(dtb, "/reserved-memory", "#size-cells", "u")
                              or str(root_size_cells), 10)
    reserved_nodes = run("fdtget", "-l", dtb, "/reserved-memory").splitlines()
    fixed_reserved = []
    dynamic_reserved = []
    errors = []

    for name in reserved_nodes:
        path = "/reserved-memory/" + name
        reg = fdtget(dtb, path, "reg", "x")
        if reg:
            for region in decode_reg(reg, reserved_addr_cells, reserved_size_cells):
                if not any(contained(region, bank) for bank in reference_banks):
                    errors.append("{} reservation outside reference RAM: {}".format(
                        path, tuple(hex(x) for x in region)))
                if not any(contained(region, ref) for ref in reference_reserved):
                    errors.append("{} reservation is not contained in a MEM_RES descriptor: {}".format(
                        path, tuple(hex(x) for x in region)))
                fixed_reserved.append((region[0], region[1], path))
            continue

        size = fdtget(dtb, path, "size", "x")
        if size:
            amount = 0
            for word in cells(size):
                amount = (amount << 32) | word
            if amount <= 0:
                errors.append("{} has invalid dynamic reservation size".format(path))
                continue
            alloc = fdtget(dtb, path, "alloc-ranges", "x")
            alloc_ranges = decode_reg(alloc, reserved_addr_cells, reserved_size_cells) if alloc else []
            for region in alloc_ranges:
                if not any(contained(region, bank) for bank in reference_banks):
                    errors.append("{} alloc-range outside reference RAM: {}".format(
                        path, tuple(hex(x) for x in region)))
            dynamic_reserved.append((path, amount, alloc_ranges))
            print("UNPINNED dynamic reservation: {} size={} bytes alloc-ranges={}".format(
                path, amount, [(hex(a), hex(e)) for a, e in alloc_ranges]))

    fixed_reserved.sort()
    for i, current in enumerate(fixed_reserved):
        for other in fixed_reserved[i + 1:]:
            if other[0] >= current[1]:
                break
            errors.append("fixed reserved-memory overlap: {} and {}".format(
                current[2], other[2]))

    # Every EDK2 MEM_RES descriptor must be represented by a fixed DT reg.
    # Dynamic pools count only when their alloc-range pins the exact full range.
    fixed_ranges = [(a, e) for a, e, _ in fixed_reserved]
    exact_dynamic_ranges = []
    for path, amount, alloc_ranges in dynamic_reserved:
        for alloc_start, alloc_end in alloc_ranges:
            if alloc_end - alloc_start == amount:
                exact_dynamic_ranges.append((alloc_start, alloc_end))
    represented = fixed_ranges + exact_dynamic_ranges
    for name, start, end, kind in descriptors:
        if kind != "MEM_RES":
            continue
        gaps = uncovered_parts((start, end), represented)
        for gap_start, gap_end in gaps:
            errors.append("reference MEM_RES '{}' not fully represented in DTB: 0x{:X}-0x{:X}".format(
                name, gap_start, gap_end))

    decompiled = run("dtc", "-I", "dtb", "-O", "dts", dtb)
    memreserve = re.findall(r"/memreserve/\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*;", decompiled)
    for address, length in memreserve:
        start, size = int(address, 16), int(length, 16)
        region = (start, start + size)
        if size <= 0 or not any(contained(region, bank) for bank in reference_banks):
            errors.append("/memreserve/ entry outside reference RAM: {}".format(
                tuple(hex(x) for x in region)))

    if errors:
        print("DTB MEMORY MAP VALIDATION: FAILED", file=sys.stderr)
        for item in errors:
            print("  ERROR: " + item, file=sys.stderr)
        return 1

    print("PASS: all fixed reservations are valid and all MEM_RES descriptors are represented")
    print("PASS: fixed reserved-memory regions do not overlap")
    print("PASS: {} /memreserve/ entries are contained in reference RAM".format(len(memreserve)))
    print("DTB MEMORY MAP VALIDATION: PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("DTB MEMORY MAP VALIDATION: FAILED: {}".format(exc), file=sys.stderr)
        sys.exit(1)
