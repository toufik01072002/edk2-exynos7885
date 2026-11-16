#!/usr/bin/env python3
"""Check the current validation-only BootShim layout and fail-closed path."""
import re
from pathlib import Path

root = Path(__file__).parent
asm = (root / "BootShim.S").read_text()
ld = (root / "bootshim.ld").read_text()

def require(source, pattern, message):
    if not re.search(pattern, source, re.MULTILINE):
        raise SystemExit("FAIL: " + message)

# The linker script, not assembler .org directives, owns fixed image offsets.
require(ld, r"^    \. = 0x00000000;\s*$", "entry section must start at image offset zero")
require(ld, r"^    \. = 0x0000D000;\s*$", "mailbox output section must start at 0xD000")
require(ld, r"^    \.mailbox\s*:", "mailbox output section is missing")
require(ld, r"^    \. = 0x0000D100;\s*$", "SEC offset slot must start at 0xD100")
require(ld, r"^    \.sec_offset\s*:", "SEC offset output section is missing")
require(ld, r"^    \. = 0x0000E000;\s*$", "private stack must start at 0xE000")
require(ld, r"^    \.stack\s*\(NOLOAD\)", "private stack output section is missing")

require(asm, r"^\.global _Head$", "global BootShim entry symbol is missing")
require(asm, r"^_Head:\s*$", "BootShim entry label is missing")
require(asm, r"adr\s+x9,\s*_BootShimStackTop", "stack must be installed position-independently")
require(asm, r"bl\s+bootshim_validate_embedded_handoff_descriptor",
        "entry must call the linked descriptor validator")
require(asm, r"cbnz\s+w0,\s*_DescriptorFault",
        "invalid descriptor must enter the fault halt")
require(asm, r"^_RuntimeEvidenceUnavailable:\s*$", "runtime evidence halt is missing")
require(asm, r"^_DescriptorFault:\s*$", "descriptor fault halt is missing")
require(asm, r"^\.section \.mailbox,", "mailbox must be an in-image section")
require(asm, r"^_HandoffDescriptorMailbox:\s*$", "mailbox symbol is missing")
require(asm, r"^\.space\s+0x100,\s*0$", "mailbox must be exactly 256 bytes")
require(asm, r"^\.section \.sec_offset,", "SEC offset slot must be an in-image section")
require(asm, r"^_SecEntryOffset:\s*$", "SEC offset symbol is missing")
require(asm, r"^\.quad\s+0$", "SEC offset slot must remain zero/unpatched")
require(asm, r"^\.section \.stack,", "private stack section is missing")
require(asm, r"^\.space\s+0x2000$", "private stack must remain 8 KiB")

# This phase is deliberately validation-only: never branch to SEC or kexec.
if re.search(r"^\s*(?:br|blr)\s+x(?:16|17)\b", asm, re.MULTILINE):
    raise SystemExit("FAIL: SEC transfer instruction is forbidden in validation-only BootShim")
if re.search(r"_SecEntryOffset", asm[asm.find("_Head:"):asm.find(".section .mailbox")]):
    raise SystemExit("FAIL: entry path must not consume the SEC offset slot")
if re.search(r"\bkexec\b", asm):
    raise SystemExit("FAIL: BootShim must not invoke kexec")

print("PASS: BootShim entry, descriptor mailbox, SEC slot and private stack layout")
print("PASS: invalid descriptors halt; valid descriptors remain fail-closed without SEC transfer")
