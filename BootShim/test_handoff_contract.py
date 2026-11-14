#!/usr/bin/env python3
"""Static contract checks for the gated ARM64 BootShim handoff candidate.

The candidate is assembled by build_bootshim.sh but is never packaged as the
launch image. These checks do not execute kexec or firmware.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ASM = (ROOT / "BootShim" / "BootShim.S").read_text(encoding="utf-8")
BUILD = (ROOT / "build_bootshim.sh").read_text(encoding="utf-8")
MAKE = (ROOT / "BootShim" / "Makefile").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github" / "workflows" / "build-a205w.yml").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def ordered(source: str, fragments: list[str]) -> bool:
    cursor = 0
    for fragment in fragments:
        pos = source.find(fragment, cursor)
        if pos < 0:
            return False
        cursor = pos + len(fragment)
    return True


# The Linux Image header and FD payload must remain at their protocol offsets.
require(".ascii\t\"ARM\\x64\"" in ASM, "ARM64 Image magic is missing")
require(".org\t0x10000" in ASM, "BootShim prefix is not fixed at 64 KiB")
require("UEFI_SIZE != 0x00200000" in ASM, "FD size is not compile-time fixed at 2 MiB")
require("SEC_ENTRY != 0x90008278" in ASM, "PrePi SEC entry constant changed unexpectedly")

# Verify the candidate's source-level transfer sequence, including its private
# stack, exact FD copy size, memory ordering and branch to the SEC entry.
candidate = ASM.split("#if ENABLE_HANDOFF", 1)[1].split("#else", 1)[0]
require(ordered(candidate, [
    "mov\\tx9, x1",
    "mov\\tsp, x9",
    "ldr\\tx10, =UEFI_BASE",
    "ldr\\tx11, =UEFI_SIZE",
    "ldp\\tx12, x13, [x9], #16",
    "stp\\tx12, x13, [x10], #16",
    "subs\\tx11, x11, #16",
    "b.ne\\t1b",
    "dsb\\tsy",
    "ic\\tiallu",
    "dsb\\tsy",
    "isb",
    "ldr\\tx16, =SEC_ENTRY",
    "br\\tx16",
]), "handoff candidate is missing or reorders a required copy/entry operation")

require("ENABLE_HANDOFF=0" in BUILD, "packaged BootShim build must force handoff off")
require("make handoff-candidate" in BUILD, "handoff candidate is not assembled in CI build")
require("ENABLE_HANDOFF    ?= 0" in MAKE, "Makefile default must remain disabled")
require("python3 BootShim/test_handoff_contract.py" in WORKFLOW,
        "GitHub Actions does not execute this handoff contract test")

# The incoming Linux kexec register state is not a source of DTB or arguments
# for this exact PrePi entry path; don't introduce an implicit x0-x3 contract.
require(not re.search(r"\b(mov|ldr)\s+x[0-3]\s*,", candidate),
        "candidate unexpectedly writes an undocumented x0-x3 entry contract")

print("PASS: ARM64 BootShim handoff candidate source contract")
print("PASS: 64 KiB prefix, 32 KiB private-stack boundary, 2 MiB FD copy")
print("PASS: DSB/IC IALLU/DSB/ISB sequence and PrePi SEC branch")
print("PASS: packaged image remains ENABLE_HANDOFF=0")
print("NOTE: static source validation only; no runtime handoff is authorized")
