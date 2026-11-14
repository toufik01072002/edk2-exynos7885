#!/usr/bin/env python3
"""Fail-closed checks for the current SM-A205W kexec/UEFI handoff contract."""
import json
import re
import sys
from pathlib import Path

manifest_path, reloc_path, trampoline_path, reset_path, machine_kexec_path = map(Path, sys.argv[1:6])
manifest = json.loads(manifest_path.read_text())
reloc = reloc_path.read_text()
trampoline = trampoline_path.read_text()
reset = reset_path.read_text()
machine_kexec = machine_kexec_path.read_text()

assert manifest["kexec"]["trampoline_load_address"] is None
assert manifest["kexec"]["uefi_handoff_addr"] is None
assert "UNIMPLEMENTED" in manifest["kexec"]["argument_contract"]

# Trace the actual argument path from machine_kexec through cpu_soft_restart.
# Current implementation passes kimage->head, kimage_start, and a zero arg2.
call = re.search(
    r"cpu_soft_restart\(in_crash_kexec\s*\?\s*0\s*:\s*is_hyp_mode_available\(\)\s*,"
    r"\s*reboot_code_buffer_phys\s*,\s*kimage->head\s*,\s*kimage_start\s*,\s*0\s*\)",
    machine_kexec, re.S
)
assert call, "machine_kexec argument tuple changed; re-audit the handoff ABI"
for pattern in (
    r"mov\s+x18,\s*x1",
    r"mov\s+x0,\s*x2",
    r"mov\s+x1,\s*x3",
    r"mov\s+x2,\s*x4",
):
    assert re.search(pattern, reset), f"cpu_soft_restart argument mapping changed: {pattern}"

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

# Versioned descriptor ABI: schema exists, but no transport or physical
# placement is assigned. These assertions deliberately prevent activation.
protocol = manifest.get("handoff_protocol")
assert isinstance(protocol, dict), "Missing versioned handoff protocol descriptor"
assert protocol.get("name") == "SM-A205W-ARM64-HANDOFF"
assert protocol.get("version") == 1, "Unsupported handoff descriptor version"
assert protocol.get("byte_order") == "little-endian"
assert protocol.get("header_size") == 256
assert protocol.get("producer") == "virtual-boot-initramfs"
assert protocol.get("consumer") == "BootShim"
assert protocol.get("transport") == "UNASSIGNED"
assert protocol.get("descriptor_address") is None
assert protocol.get("descriptor_capacity") is None
assert protocol.get("state") == "DISABLED"
assert protocol.get("integrity") == {"algorithm": "SHA-256", "required": True}

segments = protocol.get("segments")
expected_segments = ("kernel", "initramfs", "dtb", "bootshim", "uefi_fd")
assert isinstance(segments, dict) and set(segments) == set(expected_segments)
for segment_name in expected_segments:
    segment = segments[segment_name]
    assert isinstance(segment, dict), f"Invalid descriptor segment: {segment_name}"
    assert set(segment) == {"physical_address", "size", "sha256"}
    assert all(segment[field] is None for field in segment), (
        f"{segment_name} physical metadata must remain unassigned"
    )

requirements = protocol.get("activation_requirements")
required_gates = (
    "physical_reservations_verified",
    "kernel_register_contract_verified",
    "el_mmu_cache_contract_verified",
    "dma_quiescence_verified",
    "secondary_cpu_state_verified",
    "runtime_hardware_validation",
)
assert isinstance(requirements, dict)
assert all(requirements.get(gate) is False for gate in required_gates), (
    "Handoff activation gate unexpectedly marked verified"
)
assert protocol.get("required_flags") == [
    "KERNEL_VALID", "INITRAMFS_VALID", "DTB_VALID", "RANGES_RESERVED",
    "CACHE_STATE_KNOWN", "EL_STATE_KNOWN"
]

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

print("PASS: machine_kexec -> cpu_soft_restart argument flow is verified")
print("PASS: relocation entry argument clearing is explicitly acknowledged")
print("PASS: versioned handoff descriptor ABI is structurally valid and remains unassigned")
print("PASS: trampoline remains fail-closed and has no fixed load address")
print("PASS: proposed kernel/initramfs/DTB windows do not overlap")
print("HANDOFF STATUS: NOT IMPLEMENTED — execution remains blocked")
