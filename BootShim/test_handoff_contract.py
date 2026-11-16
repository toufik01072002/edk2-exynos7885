#!/usr/bin/env python3
"""Static contract checks for the fixed-segment ARM64 BootShim handoff."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ASM_RAW = (ROOT / "BootShim" / "BootShim.S").read_text(encoding="utf-8")
ASM = re.sub(r"\s+", " ", ASM_RAW)
BUILD = (ROOT / "build_bootshim.sh").read_text(encoding="utf-8")
MAKE = re.sub(r"\s+", " ", (ROOT / "BootShim" / "Makefile").read_text(encoding="utf-8"))
WORKFLOW = (ROOT / ".github" / "workflows" / "build-a205w.yml").read_text(encoding="utf-8")

def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")

require('.ascii "ARM\\x64"' in ASM, "ARM64 Image magic is missing")
require(".org 0x10000" in ASM, "BootShim prefix is not fixed at 64 KiB")
require("UEFI_SIZE != 0x00200000" in ASM, "FD size is not compile-time fixed at 2 MiB")
require("SEC_ENTRY != 0x90028278" in ASM, "PrePi SEC entry constant changed unexpectedly")
require("ldr x1, =FD_SOURCE" in ASM, "handoff candidate does not use the fixed FD staging address")

candidate = ASM.split("_Start:", 1)[1].split("#else", 1)[0]
sequence = [
    "ldr x12, =(BOOTSHIM_BASE + 0x10000)", "mov sp, x12",
    "mov x9, x1", "ldr x10, =UEFI_BASE", "ldr x11, =UEFI_SIZE",
    "ldp x12, x13, [x9], #16", "stp x12, x13, [x10], #16",
    "subs x11, x11, #16", "b.ne 1b", "dsb sy", "ic iallu",
    "dsb sy", "isb", "ldr x16, =SEC_ENTRY", "br x16",
]
cursor = 0
for instruction in sequence:
    pos = candidate.find(instruction, cursor)
    require(pos >= 0, f"fixed-segment handoff missing or reordered: {instruction}")
    cursor = pos + len(instruction)

require("ENABLE_HANDOFF=0" in BUILD, "packaged BootShim must keep its WFE gate")
require("make handoff-candidate" in BUILD, "fixed-segment candidate is not assembled")
require("BOOTSHIM_BASE=0x80080000 FD_SOURCE=0x80400000" in BUILD,
        "build script does not use the fixed-segment addresses")
require("ENABLE_HANDOFF ?= 0" in MAKE, "Makefile default must keep handoff disabled")
require("python3 BootShim/test_handoff_contract.py" in WORKFLOW,
        "GitHub Actions does not execute the handoff contract test")

print("PASS: fixed-segment BootShim uses FD staging address 0x80400000")
print("PASS: 64 KiB private stack, 2 MiB FD copy, cache maintenance and SEC branch")
print("PASS: legacy packaged image remains WFE-gated")
print("NOTE: static source validation only; candidate is not executed")
