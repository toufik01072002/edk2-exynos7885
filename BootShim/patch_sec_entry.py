#!/usr/bin/env python3
"""Patch BootShim with the actual PrePi SEC entry's bundle-relative offset."""
from __future__ import annotations
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "BootShim"))
from validate_prepi_entry import inspect_fdf, inspect_prepi_sec_entry

BOOTSHIM_SIZE = 0x10000
FD_BUNDLE_OFFSET = 0x10000
FD_MAX_SIZE = 0x200000
ENTRY_SLOT_OFFSET = 0xD100


def patch_bootshim(image: bytes, fd_entry_offset: int) -> bytes:
    if len(image) != BOOTSHIM_SIZE:
        raise ValueError("BootShim must be exactly 64 KiB")
    if fd_entry_offset < 0 or fd_entry_offset >= FD_MAX_SIZE or fd_entry_offset & 3:
        raise ValueError("PrePi SEC entry FD offset is outside the aligned 2 MiB FD")
    bundle_offset = FD_BUNDLE_OFFSET + fd_entry_offset
    patched = bytearray(image)
    current = struct.unpack_from("<Q", patched, ENTRY_SLOT_OFFSET)[0]
    if current not in (0, bundle_offset):
        raise ValueError("SEC entry slot already contains a different offset")
    struct.pack_into("<Q", patched, ENTRY_SLOT_OFFSET, bundle_offset)
    return bytes(patched)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fd", type=Path, default=ROOT / "workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd")
    parser.add_argument("--bootshim", type=Path, default=ROOT / "BootShim/BootShim.bin")
    args = parser.parse_args()
    fd_base, _ = inspect_fdf()
    sec = inspect_prepi_sec_entry(args.fd.read_bytes(), fd_base)
    fd_offset = int(sec["entry_fd_offset"], 16)
    args.bootshim.write_bytes(patch_bootshim(args.bootshim.read_bytes(), fd_offset))
    print(f"PrePi SEC FD offset: 0x{fd_offset:X}")
    print(f"Patched bundle-relative entry offset: 0x{FD_BUNDLE_OFFSET + fd_offset:X}")


if __name__ == "__main__":
    main()
