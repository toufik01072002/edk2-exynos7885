#!/usr/bin/env python3
"""Static contract checks for the gated ARM64 BootShim handoff candidate."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ASM_RAW = (ROOT / "BootShim" / "BootShim.S").read_text(encoding="utf-8")
BUILD = (ROOT / "build_bootshim.sh").read_text(encoding="utf-8")
MAKE = (ROOT / "BootShim" / "Makefile").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github" / "workflows" / "build-a205w.yml").read_text(encoding="utf-8")

def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")

# Normalize source whitespace so the contract is independent of tabs/spaces.
ASM = re.sub(r"\\s+", " ", ASM_RAW)
require('.ascii "ARM\\\\x64"' in ASM, "ARM64 Image magic is missing")
require(".org 0x10000" in ASM, "BootShim prefix is not fixed at 64 KiB")
require("UEFI_SIZE != 0x00200000" in ASM, "FD size is not compile-time fixed at 2 MiB")
require("SEC_ENTRY != 0x90008278" in ASM, "PrePi SEC entry constant changed unexpectedly")

candidate = ASM.split("#if ENABLE_HANDOFF", 1)[1].split("#else", 1)[0]
sequence = [
    "mov x9, x1", "mov sp, x9", "ldr x10, =UEFI_BASE",
    "ldr x11, =UEFI_SIZE", "ldp x12, x13, [x9], #16",
    "stp x12, x13, [x10], #16", "subs x11, x11, #16",
    "b.ne 1b", "dsb sy", "ic iallu", "dsb sy", "isb",
    "ldr x16, =SEC_ENTRY", "br x16",
]
cursor = 0
for instruction in sequence:
    pos = candidate.find(instruction, cursor)
    require(pos >= 0, f"handoff sequence missing or reordered: {instruction}")
    cursor = pos + len(instruction)

require("ENABLE_HANDOFF=0" in BUILD, "packaged BootShim build must force handoff off")
require("make handoff-candidate" in BUILD, "handoff candidate is not assembled in CI build")
require("ENABLE_HANDOFF ?= 0" in re.sub(r"\\s+", " ", MAKE),
        "Makefile default must remain disabled")
require("python3 BootShim/test_handoff_contract.py" in WORKFLOW,
        "GitHub Actions does not execute this handoff contract test")
require(not re.search(r"\\b(mov|ldr)\\s+x[0-3]\\s*,", candidate),
        "candidate unexpectedly writes an undocumented x0-x3 entry contract")

print("PASS: ARM64 BootShim handoff candidate source contract")
print("PASS: 64 KiB prefix, 2 MiB FD copy, cache maintenance and SEC branch")
print("PASS: packaged image remains ENABLE_HANDOFF=0")
print("NOTE: static source validation only; no runtime handoff is authorized")
