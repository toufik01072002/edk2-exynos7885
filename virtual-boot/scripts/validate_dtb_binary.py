#!/usr/bin/env python3
"""Validate the compiled SM-A205W Virtual Boot DTB using dtc's fdtget."""
import argparse
import subprocess
import sys

def prop(dtb, node, name, kind="x"):
    p = subprocess.run(["fdtget", "-t", kind, dtb, node, name],
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        print(f"fdtget failed for node={node!r} property={name!r}: {p.stderr.strip()}", file=sys.stderr)
        return None
    return p.stdout.strip()

def cells(dtb, node, name):
    value = prop(dtb, node, name)
    return [] if not value else [int(x, 16) for x in value.split()]

def value(words):
    result = 0
    for word in words:
        result = (result << 32) | word
    return result

def children(dtb, node):
    p = subprocess.run(["fdtget", "-l", dtb, node], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError("Cannot enumerate " + node + ": " + p.stderr.strip())
    return [x.strip() for x in p.stdout.splitlines() if x.strip()]

def entries(dtb, node, ac, sc):
    raw = cells(dtb, node, "reg")
    stride = ac + sc
    if not raw or len(raw) % stride:
        return []
    out = []
    for i in range(0, len(raw), stride):
        addr, size = value(raw[i:i+ac]), value(raw[i+ac:i+stride])
        if size:
            out.append((addr, size))
    return out

def check_nonoverlap(regions, label):
    ordered = sorted(regions, key=lambda x: x[1])
    for a, b in zip(ordered, ordered[1:]):
        if a[1] + a[2] > b[1]:
            raise RuntimeError(f"{label} overlap: {a[0]} and {b[0]}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dtb")
    dtb = ap.parse_args().dtb
    ac, sc = prop(dtb, "/", "#address-cells"), prop(dtb, "/", "#size-cells")
    if ac is None or sc is None:
        print("=== DTB root property listing (fdtget -p) ===", file=sys.stderr)
        listing = subprocess.run(["fdtget", "-p", dtb, "/"], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print(listing.stdout, file=sys.stderr)
        print(listing.stderr, file=sys.stderr)
        print("=== Decompiled DTB root (dtc) ===", file=sys.stderr)
        dump = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", dtb],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print("\\n".join(dump.stdout.splitlines()[:75]), file=sys.stderr)
        print(dump.stderr, file=sys.stderr)
        raise RuntimeError("DTB root lacks address/size cells")
    ac, sc = int(ac, 16), int(sc, 16)
    if ac not in (1, 2) or sc not in (1, 2):
        raise RuntimeError(f"Unsupported root cells {ac}/{sc}")
    model = prop(dtb, "/", "model", "s")
    compatible = prop(dtb, "/", "compatible", "s") or ""
    if not model or "SM-A205W" not in model or "exynos7885" not in compatible:
        raise RuntimeError(f"Wrong platform identity: {model!r} {compatible!r}")

    ram = []
    for name in children(dtb, "/"):
        if name.startswith("memory@"):
            ram.extend((name, a, s) for a, s in entries(dtb, "/" + name, ac, sc))
    if len(ram) < 3:
        raise RuntimeError(f"Expected at least 3 non-empty RAM banks, got {len(ram)}")
    check_nonoverlap(ram, "RAM bank")

    parent = "/reserved-memory"
    rac, rsc = prop(dtb, parent, "#address-cells"), prop(dtb, parent, "#size-cells")
    if rac is None or rsc is None:
        raise RuntimeError("Missing reserved-memory cell declarations")
    rac, rsc = int(rac, 16), int(rsc, 16)
    reserved = []
    for name in children(dtb, parent):
        reserved.extend((name, a, s) for a, s in entries(dtb, parent + "/" + name, rac, rsc))
    if not reserved:
        raise RuntimeError("No fixed reserved-memory regions found")
    check_nonoverlap(reserved, "Reserved region")
    for name, addr, size in reserved:
        if not any(addr >= base and addr + size <= base + length for _, base, length in ram):
            raise RuntimeError(f"{name} at 0x{addr:x}+0x{size:x} is outside RAM")
    if sum(1 for _, a, s in reserved if a == 0x90000000 and s == 0x200000) != 1:
        raise RuntimeError("Expected exactly one 2 MiB UEFI reservation at 0x90000000")

    print(f"DTB identity: {model}")
    print(f"RAM banks: {len(ram)}; fixed reserved regions: {len(reserved)}")
    for n, a, s in sorted(ram + reserved, key=lambda x: x[1]):
        print(f"{n}: 0x{a:x}-0x{a+s-1:x}")
    print("PASS: binary FDT identity, RAM ranges, reserved ranges and UEFI reservation")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"DTB VALIDATION FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
