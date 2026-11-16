#!/usr/bin/env python3
import hashlib
import json
import subprocess
import sys
from pathlib import Path

bootshim = Path("BootShim/BootShim.bin")
firmware = Path("workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd")
bundle = Path("workspace/UEFI")
manifest = Path("workspace/UEFI.manifest.json")
COPY_SIZE = 0x00200000
BOOTSHIM_SIZE = 0x10000
SEC_ENTRY_SLOT = 0xD100
LINKED_IMAGE_BASE = 0x90000000

if not bootshim.is_file() or not firmware.is_file():
    raise SystemExit("ERROR: BootShim or firmware missing")

# Patch the actual PrePi SEC entry offset into BootShim before packaging.
subprocess.run([sys.executable, "BootShim/patch_sec_entry.py",
                "--fd", str(firmware), "--bootshim", str(bootshim)], check=True)
bs = bootshim.read_bytes()
fd = firmware.read_bytes()
if len(bs) != BOOTSHIM_SIZE:
    raise SystemExit("ERROR: BootShim must occupy exactly 64 KiB")
if not 0 < len(fd) <= COPY_SIZE:
    raise SystemExit("ERROR: Firmware size invalid or exceeds 2 MiB")

sec_entry_bundle_offset = int.from_bytes(bs[SEC_ENTRY_SLOT:SEC_ENTRY_SLOT + 8], "little")
if not BOOTSHIM_SIZE <= sec_entry_bundle_offset < BOOTSHIM_SIZE + COPY_SIZE or sec_entry_bundle_offset & 3:
    raise SystemExit("ERROR: invalid patched PrePi SEC entry bundle offset")

padded_fd = fd + bytes(COPY_SIZE - len(fd))
bundle_data = bs + padded_fd
bundle.write_bytes(bundle_data)

def sha(data):
    return hashlib.sha256(data).hexdigest()

metadata = {
    "format": "7K-EDK2-BUNDLE",
    "format_version": 2,
    "architecture": "AARCH64",
    "platform": "Samsung SM-A205W / Exynos 7885",
    "bootshim": {
        "offset": 0, "size": len(bs), "sha256": sha(bs),
        "sec_entry_offset_slot": hex(SEC_ENTRY_SLOT),
        "sec_entry_bundle_offset": hex(sec_entry_bundle_offset),
    },
    "firmware": {
        "source_size": len(fd), "offset": BOOTSHIM_SIZE,
        "padded_size": COPY_SIZE, "linked_image_base": hex(LINKED_IMAGE_BASE),
        "runtime_physical_load_address": None, "runtime_placement_status": "UNVERIFIED",
        "copy_size": COPY_SIZE, "sha256": sha(fd), "padded_sha256": sha(padded_fd),
    },
    "bundle": {"size": len(bundle_data), "sha256": sha(bundle_data)},
}
manifest.write_text(json.dumps(metadata, indent=2) + "\n")
print("Bundle generated:", bundle)
print("PrePi SEC entry bundle offset:", hex(sec_entry_bundle_offset))
print("Runtime physical load address: UNASSIGNED")
print("Bundle:", len(bundle_data), "bytes")
