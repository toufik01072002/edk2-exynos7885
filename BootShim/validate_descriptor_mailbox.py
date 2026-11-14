#!/usr/bin/env python3
"""Validate BootShim mailbox, SEC offset slot, stack and real entry branch."""
import re
from pathlib import Path

source = Path(__file__).with_name("BootShim.S").read_text()

def require(pattern, message):
    if not re.search(pattern, source, re.MULTILINE):
        raise SystemExit("FAIL: " + message)

require(r"^\.org\s+0xD000,\s*0$", "mailbox must begin at image offset 0xD000")
require(r"^_HandoffDescriptorMailbox:\s*$", "mailbox symbol is missing")
require(r"^\.space\s+0x100,\s*0$", "mailbox must be exactly 256 bytes")
require(r"^\.org\s+0xD100,\s*0$", "SEC entry offset slot must begin at 0xD100")
require(r"^_SecEntryOffset:\s*$", "SEC entry offset symbol is missing")
require(r"^\.quad\s+0$", "SEC entry offset must be build-patched as a 64-bit value")
require(r"^\.org\s+0xE000,\s*0$", "private stack must begin at 0xE000")
require(r"^_BootShimStackBottom:\s*$", "stack bottom symbol is missing")
require(r"^\.space\s+0x2000,\s*0$", "private stack must remain 8 KiB")
require(r"^_BootShimStackTop:\s*$", "stack top symbol is missing")
require(r"^\.org\s+0x10000,\s*0$", "FD boundary must remain at 64 KiB")
require(r"adr\s+x9,\s*_BootShimStackTop", "stack must remain position-independent")
require(r"adr\s+x16,\s*_Head", "bundle base must be derived from the current PC")
require(r"adr\s+x17,\s*_SecEntryOffset", "SEC offset slot must be addressed position-independently")
require(r"ldr\s+x17,\s*\[x17\]", "SEC bundle offset must be loaded")
require(r"add\s+x16,\s*x16,\s*x17", "SEC entry address must be computed from bundle base plus offset")
require(r"br\s+x16", "BootShim must branch to the built PrePi SEC entry")

assert 0xD000 + 0x100 == 0xD100
assert 0xD108 <= 0xE000
assert 0xE000 + 0x2000 == 0x10000
print("PASS: 256-byte descriptor mailbox and 64-bit SEC entry offset slot are in-image")
print("PASS: BootShim installs its private stack and branches to the build-resolved PrePi SEC entry")
print("STATUS: bundle-relative branch is wired; runtime physical placement remains unassigned")
