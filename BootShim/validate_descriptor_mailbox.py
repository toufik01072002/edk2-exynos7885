#!/usr/bin/env python3
"""Static layout guard for the BootShim in-image handoff mailbox."""
import re
from pathlib import Path

source = Path(__file__).with_name("BootShim.S").read_text()

def require(pattern, message):
    if not re.search(pattern, source, re.MULTILINE):
        raise SystemExit("FAIL: " + message)

require(r"^\.org\s+0xD000,\s*0$", "mailbox must begin at image offset 0xD000")
require(r"^_HandoffDescriptorMailbox:\s*$", "mailbox symbol is missing")
require(r"^\.space\s+0x100,\s*0$", "mailbox must be exactly 256 zeroed bytes")
require(r"^\.org\s+0xE000,\s*0$", "private stack must begin at 0xE000")
require(r"^_BootShimStackBottom:\s*$", "stack bottom symbol is missing")
require(r"^\.space\s+0x2000,\s*0$", "private stack must remain 8 KiB")
require(r"^_BootShimStackTop:\s*$", "stack top symbol is missing")
require(r"^\.org\s+0x10000,\s*0$", "FD boundary must remain at 64 KiB")
require(r"adr\s+x9,\s*_BootShimStackTop", "stack must remain position-independent")
require(r"wfe\s*\n\s*b\s+_Disabled", "handoff must remain disabled")

# The mailbox occupies [0xD000, 0xD100), leaving a 0xF00-byte gap
# before the stack at 0xE000. Stack and FD boundaries remain unchanged.
assert 0xD000 + 0x100 == 0xD100
assert 0xD100 <= 0xE000
assert 0xE000 + 0x2000 == 0x10000
print("PASS: 256-byte image-relative descriptor mailbox is disjoint from stack and FD")
print("PASS: BootShim remains position-independent and fail-closed")
print("STATUS: mailbox slot reserved; producer population and runtime consumption remain unimplemented")
