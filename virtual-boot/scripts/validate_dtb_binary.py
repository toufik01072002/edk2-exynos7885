#!/usr/bin/env python3
"""Validate a compiled DTB against EXYNOS7885Pkg/Include/Configuration/DeviceMemoryMap.h.

Uses dtc/fdtget to read the binary FDT. RAM descriptors are merged (including
reserved subregions) and compared with /memory nodes. Fixed reserved-memory
reg entries must be contained by matching MEM_RES descriptors; they are not
incorrectly treated as overlaps with the RAM banks that contain them.
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
        raise ValueError("malformed reg property: {!r}".format(value))
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


def merge_ranges(ranges):
    merged = []
    for start, end in sorted(ranges):
        if end <= start:
            raise ValueError("invalid/empty range 0x{:X}-0x{:X}".format(start, end))
        if merged and start < merged[-1][1]:
            raise ValueError("reference descriptors overlap at 0x{:X}".format(start))
        if merged and start == merged[-1][1]:
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
    reference_banks = merge_ranges([(a, e) for _, a, e, _ in descriptors])
    expected_banks = [
        (0x80000000, 0xBBC00000),
        (0xC0000000, 0x100000000),
        (0x880000000, 0x8C0000000),
    ]
    if reference_banks != expected_banks:
        raise ValueError("DeviceMemoryMap.h bank spans differ from approved reference: {}".format(
            [(hex(a), hex(e)) for a, e in reference_banks]))
    print("PASS: DeviceMemoryMap.h descriptors merge to the 3 approved RAM banks")

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
    memory_ranges = sorted(memory_ranges)
    if memory_ranges != expected_banks:
        raise ValueError("DTB /memory ranges do not match DeviceMemoryMap.h: {}".format(
            [(hex(a), hex(e)) for a, e in memory_ranges]))
    print("PASS: DTB /memory ranges match DeviceMemoryMap.h exactly")

    reserved_addr_cells = int(fdtget(dtb, "/reserved-memory", "#address-cells", "u") or str(root_addr_cells), 10)
    reserved_size_cells = int(fdtget(dtb, "/reserved-memory", "#size-cells", "u") or str(root_size_cells), 10)
    reserved_nodes = run("fdtget", "-l", dtb, "/reserved-memory").splitlines()
    fixed_reserved = []
    dynamic_reserved = []
    reference_reserved = [(a, e) for _, a, e, kind in descriptors if kind == "MEM_RES"]

    for name in reserved_nodes:
        path = "/reserved-memory/" + name
        reg = fdtget(dtb, path, "reg", "x")
        if reg:
            regions = decode_reg(reg, reserved_addr_cells, reserved_size_cells)
            no_map = fdtget(dtb, path, "no-map") is not None
            for region in regions:
                if not any(contained(region, bank) for bank in expected_banks):
                    raise ValueError("{} reservation lies outside reference RAM: {}".format(
                        path, tuple(hex(x) for x in region)))
                if not any(contained(region, ref) for ref in reference_reserved):
                    raise ValueError("{} reservation is not covered by a MEM_RES descriptor: {}".format(
                        path, tuple(hex(x) for x in region)))
                fixed_reserved.append((region[0], region[1], path, no_map))
        else:
            size = fdtget(dtb, path, "size", "x")
            if size:
                words = cells(size)
                amount = 0
                for word in words:
                    amount = (amount << 32) | word
                if amount <= 0:
                    raise ValueError("{} has invalid dynamic reservation size".format(path))
                dynamic_reserved.append((path, amount))

    fixed_reserved.sort()
    for i, current in enumerate(fixed_reserved):
        for other in fixed_reserved[i + 1:]:
            if other[0] >= current[1]:
                break
            raise ValueError("fixed reserved-memory regions overlap: {} and {}".format(
                current[2], other[2]))
    if not fixed_reserved:
        raise ValueError("DTB contains no fixed reserved-memory reg entries")
    print("PASS: {} fixed reservations are inside RAM and covered by MEM_RES descriptors".format(
        len(fixed_reserved)))
    actual_fixed = sorted((start, end) for start, end, _, _ in fixed_reserved)
    expected_fixed = sorted(reference_reserved)
    if actual_fixed != expected_fixed:
        missing = sorted(set(expected_fixed) - set(actual_fixed))
        unexpected = sorted(set(actual_fixed) - set(expected_fixed))
        raise ValueError("fixed DTB reservations do not match DeviceMemoryMap.h; missing={}, unexpected={}".format(
            [(hex(a), hex(e)) for a, e in missing],
            [(hex(a), hex(e)) for a, e in unexpected]))
    print("PASS: fixed DTB reservations match every MEM_RES descriptor exactly")
    print("PASS: fixed reserved-memory regions do not overlap one another")
    if dynamic_reserved:
        print("INFO: dynamic reservations: " + ", ".join(
            "{}={} bytes".format(n, s) for n, s in dynamic_reserved))

    # Samsung's /memreserve/ map is separate from /reserved-memory. It may
    # legitimately lie inside a RAM bank, so only containment is checked.
    decompiled = run("dtc", "-I", "dtb", "-O", "dts", dtb)
    memreserve = re.findall(r"/memreserve/\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*;", decompiled)
    for address, length in memreserve:
        start, size = int(address, 16), int(length, 16)
        region = (start, start + size)
        if size <= 0 or not any(contained(region, bank) for bank in expected_banks):
            raise ValueError("/memreserve/ entry outside reference RAM: {}".format(region))
    print("PASS: {} /memreserve/ entries are contained in reference RAM".format(len(memreserve)))
    print("DTB MEMORY MAP VALIDATION: PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("DTB MEMORY MAP VALIDATION: FAILED: {}".format(exc), file=sys.stderr)
        sys.exit(1)
