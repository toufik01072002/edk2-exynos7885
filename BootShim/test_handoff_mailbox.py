#!/usr/bin/env python3
"""Validate the shared fixed-address Linux-to-UEFI handoff mailbox contract."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "BootShim/A20_HANDOFF_RECORD.h").read_text()
MEMMAP = (ROOT / "EXYNOS7885Pkg/Include/Configuration/DeviceMemoryMap.h").read_text()
DTS = (ROOT / "virtual-boot/dtb/exynos7885-a205w-virtual.dts").read_text()
SHIM = (ROOT / "BootShim/BootShim.S").read_text()
BUILD = (ROOT / "build_bootshim.sh").read_text()
ADDR, SIZE = 0x90200000, 0x1000
def require(ok, message):
    if not ok: raise SystemExit("FAIL: " + message)
for token, expected in (("A20_HANDOFF_MAILBOX_ADDRESS", ADDR), ("A20_HANDOFF_MAILBOX_SIZE", SIZE), ("A20_HANDOFF_VERSION", 1), ("A20_HANDOFF_HEADER_SIZE", 56)):
    m = re.search(r"#define\s+" + token + r"\s+(0x[0-9A-Fa-f]+|[0-9]+)", HEADER)
    require(m is not None and int(m.group(1).rstrip("UuLl"), 0) == expected, token + " mismatch")
m = re.search(r'\{"Handoff Mailbox",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*AddMem,\s*MEM_RES', MEMMAP)
require(m is not None and int(m.group(1), 16) == ADDR and int(m.group(2), 16) == SIZE, "DeviceMemoryMap mailbox reservation mismatch")
m = re.search(r'\{"RAM Bank 0 Mid",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*AddMem,\s*SYS_MEM', MEMMAP)
require(m is not None and int(m.group(1), 16) == ADDR + SIZE, "usable RAM does not resume after mailbox")
m = re.search(r'handoff-mailbox@90200000\s*\{([^}]+)\}', DTS, re.S)
require(m is not None and re.search(r'reg\s*=\s*<0x0\s+0x90200000\s+0x00001000>', m.group(1)), "DTS mailbox address/size mismatch")
require(re.search(r'\bno-map\s*;', m.group(1)), "DTS mailbox must be no-map")
require("ENABLE_HANDOFF=0" in BUILD and "#define ENABLE_HANDOFF 0" in SHIM and "_HandoffBlocked" in SHIM, "mailbox reservation must not enable handoff")
print("PASS: mailbox ABI constants, DeviceMemoryMap reservation and DTS no-map node")
print("PASS: packaged BootShim remains WFE-gated")
