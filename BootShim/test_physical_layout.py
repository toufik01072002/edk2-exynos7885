#!/usr/bin/env python3
"""Host-side address proof for the SM-A205W BootShim -> fixed EDK2 FD path.

This is a design/layout test only. It does not run kexec, touch a device,
validate live /proc/iomem, or authorize a firmware handoff.
"""
from __future__ import annotations

import json
from pathlib import Path

MIB = 0x100000
KEXEC_ALIGN = 2 * MIB
TEXT_OFFSET = 0x80000

RAM0_LOW = (0x80000000, 0x90000000)       # [start, end)
FD_RANGE = (0x90000000, 0x90200000)       # reserved, 2 MiB
RAM0_MID = (0x90200000, 0xB8000000)       # [start, end)
RAM_RANGES = (RAM0_LOW, RAM0_MID)

FD_BASE = FD_RANGE[0]
FD_SIZE = FD_RANGE[1] - FD_RANGE[0]
SEC_ENTRY = 0x90008280
SEC_ENTRY_OFFSET = SEC_ENTRY - FD_BASE

# Reserve a 64 KiB prefix for position-independent BootShim code and stack.
# The FD payload follows immediately after this prefix.
PREFIX_SIZE = 0x10000
STACK_BOTTOM_OFFSET = 0x8000
STACK_TOP_OFFSET = PREFIX_SIZE

# First-fit candidate from the first usable RAM range in DeviceMemoryMap.h.
# The actual kexec -d -l trace must still confirm the selected hole.
KEXEC_HOLE_CANDIDATE = 0x80000000

BOOTSHIM_BIN = Path(__file__).with_name("BootShim.bin")
CANDIDATE_BIN = Path(__file__).with_name("BootShim-handoff-candidate.bin")
BUILD_SCRIPT = Path(__file__).resolve().parents[1] / "build_bootshim.sh"
MAKEFILE = Path(__file__).with_name("Makefile")
WFE_OPCODE = bytes.fromhex("5f 20 03 d5")


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def contained(start: int, end: int, region: tuple[int, int]) -> bool:
    return region[0] <= start < end <= region[1]


def overlaps(a: tuple[int, int], b: tuple[int, int]) -> bool:
    return a[0] < b[1] and b[0] < a[1]


def main() -> None:
    for path in (BOOTSHIM_BIN, CANDIDATE_BIN, BUILD_SCRIPT, MAKEFILE):
        if not path.is_file():
            fail(f"required BootShim build output or control file is missing: {path}")

    shim_bytes = BOOTSHIM_BIN.read_bytes()
    candidate_bytes = CANDIDATE_BIN.read_bytes()
    if len(shim_bytes) != PREFIX_SIZE:
        fail(f"packaged BootShim prefix is 0x{len(shim_bytes):X}, expected 0x{PREFIX_SIZE:X}")
    if len(candidate_bytes) != PREFIX_SIZE:
        fail(f"handoff candidate prefix is 0x{len(candidate_bytes):X}, expected 0x{PREFIX_SIZE:X}")
    if WFE_OPCODE not in shim_bytes:
        fail("packaged BootShim does not contain the WFE safety gate")
    if "ENABLE_HANDOFF=0" not in BUILD_SCRIPT.read_text(encoding="utf-8"):
        fail("build_bootshim.sh does not force the packaged handoff path off")
    if "ENABLE_HANDOFF    ?= 0" not in MAKEFILE.read_text(encoding="utf-8"):
        fail("Makefile default does not keep handoff disabled")

    # Verify the first instruction after the 64-byte Image header branches
    # directly to the WFE instruction in the packaged (disabled) image.
    branch = int.from_bytes(shim_bytes[64:68], "little")
    if branch & 0xFC000000 != 0x14000000:
        fail("packaged BootShim entry does not begin with an unconditional safety branch")
    immediate = branch & 0x03FFFFFF
    if immediate & (1 << 25):
        immediate -= 1 << 26
    blocked_target = 64 + (immediate << 2)
    if shim_bytes[blocked_target:blocked_target + 4] != WFE_OPCODE:
        fail("packaged BootShim entry does not branch directly to WFE")

    image_size = PREFIX_SIZE + FD_SIZE
    hole_size = TEXT_OFFSET + image_size
    image_start = KEXEC_HOLE_CANDIDATE + TEXT_OFFSET
    image_end = image_start + image_size

    stack = (
        image_start + STACK_BOTTOM_OFFSET,
        image_start + STACK_TOP_OFFSET,
    )
    payload = (image_start + PREFIX_SIZE, image_end)
    destination = FD_RANGE
    kexec_hole = (
        KEXEC_HOLE_CANDIDATE,
        KEXEC_HOLE_CANDIDATE + hole_size,
    )

    if KEXEC_HOLE_CANDIDATE % KEXEC_ALIGN:
        fail("candidate kexec hole is not 2 MiB aligned")
    if image_size != 0x210000:
        fail(f"unexpected combined Image size: 0x{image_size:X}")
    if hole_size != 0x290000:
        fail(f"unexpected kexec hole requirement: 0x{hole_size:X}")
    if not contained(*kexec_hole, RAM0_LOW):
        fail("candidate kexec hole is not wholly inside usable RAM Bank 0 Low")
    if not contained(image_start, image_end, RAM0_LOW):
        fail("combined Image segment is not wholly inside usable RAM Bank 0 Low")
    if not contained(*stack, (image_start, image_start + PREFIX_SIZE)):
        fail("scratch stack is outside the BootShim prefix")
    if stack[1] != payload[0]:
        fail("stack top must meet the payload boundary exactly")
    if stack[0] % 16 or stack[1] % 16:
        fail("stack bounds must be 16-byte aligned")
    if not contained(*payload, (image_start, image_end)):
        fail("FD source payload is outside the combined Image")
    if payload[1] - payload[0] != FD_SIZE:
        fail("FD source payload is not exactly 2 MiB")
    if overlaps(payload, destination):
        fail("FD source payload overlaps the fixed FD destination")
    if not contained(*destination, FD_RANGE):
        fail("FD destination does not match the reserved 2 MiB window")
    if not FD_BASE <= SEC_ENTRY < FD_BASE + FD_SIZE:
        fail("SEC entry is outside the FD")
    if SEC_ENTRY_OFFSET != 0x8278:
        fail(f"unexpected SEC entry offset: 0x{SEC_ENTRY_OFFSET:X}")

    # Reject the earlier direct-placement proposal: a 0x180000 prefix makes
    # kexec require a 4 MiB contiguous hole from 0x8FE00000 to 0x90200000.
    # That span crosses RAM0_LOW into the reserved FD range and is not one
    # usable RAM range in the platform memory map.
    direct_prefix = 0x180000
    direct_image_size = direct_prefix + FD_SIZE
    direct_hole_size = TEXT_OFFSET + direct_image_size
    direct_hole_start = FD_BASE - TEXT_OFFSET - direct_prefix
    direct_hole = (direct_hole_start, direct_hole_start + direct_hole_size)
    if direct_hole_start % KEXEC_ALIGN:
        fail("the historical direct-placement equation is not 2 MiB aligned")
    if any(contained(*direct_hole, region) for region in RAM_RANGES):
        fail("direct-placement candidate unexpectedly fits a single usable RAM range")
    if not overlaps(direct_hole, FD_RANGE):
        fail("direct-placement candidate no longer overlaps the reserved FD")

    result = {
        "validation_scope": "static address arithmetic and reference memory-map model only",
        "handoff_enabled": False,
        "text_offset": f"0x{TEXT_OFFSET:X}",
        "prefix_size": f"0x{PREFIX_SIZE:X}",
        "fd_size": f"0x{FD_SIZE:X}",
        "combined_image_size": f"0x{image_size:X}",
        "kexec_hole_candidate": f"0x{KEXEC_HOLE_CANDIDATE:X}",
        "kexec_hole_size": f"0x{hole_size:X}",
        "image_entry_candidate": f"0x{image_start:X}",
        "image_end_exclusive": f"0x{image_end:X}",
        "stack": [f"0x{stack[0]:X}", f"0x{stack[1]:X}"],
        "fd_source": [f"0x{payload[0]:X}", f"0x{payload[1]:X}"],
        "fd_destination": [f"0x{FD_BASE:X}", f"0x{FD_BASE + FD_SIZE:X}"],
        "sec_entry": f"0x{SEC_ENTRY:X}",
        "rejected_direct_placement_hole": [
            f"0x{direct_hole[0]:X}",
            f"0x{direct_hole[1]:X}",
        ],
        "runtime_kexec_hole_confirmation_required": True,
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
