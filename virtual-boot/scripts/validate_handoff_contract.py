#!/usr/bin/env python3
"""Fail-closed checks for the current SM-A205W kexec/UEFI handoff contract."""
import json
import re
import sys
from pathlib import Path

manifest_path, reloc_path, trampoline_path = map(Path, sys.argv[1:4])
manifest = json.loads(manifest_path.read_text())
reloc = reloc_path.read_text()
trampoline = trampoline_path.read_text()

assert manifest["kexec"]["trampoline_load_address"] is None
assert manifest["kexec"]["uefi_handoff_addr"] is None
assert "UNIMPLEMENTED" in manifest["kexec"]["argument_contract"]

# This vendor kexec implementation deliberately zeroes all four argument
# registers before jumping to the new image. The handoff must not assume x0=DTB.
required = [
    r"mov\s+x0,\s*xzr",
    r"mov\s+x1,\s*xzr",
    r"mov\s+x2,\s*xzr",
    r"mov\s+x3,\s*xzr",
    r"br\s+x18",
]
for pattern in required:
    assert re.search(pattern, reloc), f"Kernel kexec contract changed: missing {pattern}"

# Until the register/memory handoff contract is implemented, trampoline must
# remain a harmless WFE loop and must not touch GIC/PSCI or branch to firmware.
assert re.search(r"wfe\s*\n\s*b\s+\.Lhandoff_disabled", trampoline)
for forbidden in ("PSCI_CPU_OFF", "GICD_BASE", "GICC_BASE", "0x90000000", "br\s+x"):
    assert forbidden not in trampoline, f"Unsafe active handoff code detected: {forbidden}"

# Proposed payload ranges must not overlap one another.
ranges = []
for key in ("kernel", "initramfs", "dtb"):
    address = int(manifest[key]["load_address"], 16)
    # Exact artifact sizes are checked by the bundle job; reserve conservative
    # maximum windows here to catch the known fixed-address collision.
    size = {"kernel": 0x02000000, "initramfs": 0x02000000, "dtb": 0x00200000}[key]
    ranges.append((key, address, address + size))
for i, (name, start, end) in enumerate(ranges):
    for other, other_start, other_end in ranges[i + 1:]:
        assert end <= other_start or other_end <= start, (
            f"Proposed load windows overlap: {name} and {other}"
        )

print("PASS: kernel kexec argument behavior is explicitly acknowledged")
print("PASS: trampoline remains fail-closed and has no fixed load address")
print("PASS: proposed kernel/initramfs/DTB windows do not overlap")
print("HANDOFF STATUS: NOT IMPLEMENTED — execution remains blocked")
