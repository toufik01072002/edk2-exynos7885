#!/usr/bin/env python3
"""Finalize and validate an ARM64 Linux Image header for the BootShim bundle.

This only makes the wrapper recognizable to ARM64 Linux image loaders.
It deliberately does not enable the UEFI copy/branch handoff.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"ARM\x64"
HEADER_SIZE = 64
TEXT_OFFSET = 0x80000
IMAGE_SIZE_OFFSET = 16
TEXT_OFFSET_OFFSET = 8
MAGIC_OFFSET = 56


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--text-offset", type=lambda x: int(x, 0), default=TEXT_OFFSET)
    args = parser.parse_args()

    image = args.image
    data = bytearray(image.read_bytes())
    if len(data) < HEADER_SIZE:
        raise SystemExit("ERROR: combined image is shorter than the 64-byte ARM64 header")
    if data[MAGIC_OFFSET:MAGIC_OFFSET + 4] != MAGIC:
        raise SystemExit("ERROR: ARM64 Image magic missing at offset 0x38")
    if args.text_offset < 0 or args.text_offset >= 0x200000 or args.text_offset % 4:
        raise SystemExit("ERROR: text_offset must be 4-byte aligned and below 2 MiB")
    if len(data) > 0xFFFFFFFFFFFFFFFF:
        raise SystemExit("ERROR: image length cannot be represented in the ARM64 header")

    struct.pack_into("<Q", data, TEXT_OFFSET_OFFSET, args.text_offset)
    struct.pack_into("<Q", data, IMAGE_SIZE_OFFSET, len(data))
    image.write_bytes(data)

    verify = image.read_bytes()
    text_offset, image_size = struct.unpack_from("<QQ", verify, TEXT_OFFSET_OFFSET)
    if text_offset != args.text_offset or image_size != len(verify):
        raise SystemExit("ERROR: finalized ARM64 header failed read-back validation")
    result = {
        "format": "ARM64-LINUX-IMAGE",
        "validation_scope": "header structure only; no kexec placement or UEFI handoff validation",
        "path": str(image),
        "length_bytes": len(verify),
        "text_offset": f"0x{text_offset:016X}",
        "image_size": image_size,
        "magic": verify[MAGIC_OFFSET:MAGIC_OFFSET + 4].decode("ascii", errors="replace"),
        "sha256": hashlib.sha256(verify).hexdigest(),
        "handoff_enabled": False,
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
