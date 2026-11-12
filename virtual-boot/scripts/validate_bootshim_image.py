#!/usr/bin/env python3
"""Validate the ARM64 Linux Image wrapper and its appended EDK2 FD payload."""
import argparse
import struct
from pathlib import Path

MAGIC = b"ARM\x64"
FV_SIGNATURE = b"_FVH"


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--fd-size", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--fd-window", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--fd-base", type=lambda x: int(x, 0), required=True)
    args = parser.parse_args()

    data = args.image.read_bytes()
    if len(data) < 64:
        fail("bundle is shorter than the 64-byte ARM64 Image header")
    if args.fd_size <= 0 or args.fd_size % 16:
        fail("FD size must be positive and 16-byte aligned")
    if args.fd_size > args.fd_window:
        fail("FD exceeds its reserved physical window")
    if len(data) <= args.fd_size:
        fail("bundle does not contain a BootShim before the FD")

    code0, reserved = struct.unpack_from("<II", data, 0)
    text_offset, image_size, flags = struct.unpack_from("<QQQ", data, 8)
    if code0 == 0:
        fail("ARM64 Image code0 branch is empty")
    if reserved != 0:
        fail("ARM64 Image reserved word at offset 4 must be zero")
    if text_offset != 0x80000 or text_offset % 0x1000:
        fail(f"unexpected text_offset: 0x{text_offset:x}")
    if image_size != len(data):
        fail(f"image_size field {image_size} != bundle length {len(data)}")
    if data[56:60] != MAGIC:
        fail(f"ARM64 Image magic missing at offset 56: {data[56:60]!r}")
    if struct.unpack_from("<I", data, 60)[0] != 0:
        fail("non-EFI Linux Image PE header offset must be zero")

    fd_offset = len(data) - args.fd_size
    fd = data[fd_offset:]
    if len(fd) < 64 or fd[40:44] != FV_SIGNATURE:
        fail("appended payload has no PI firmware-volume signature at FD offset 0x28")
    fv_length = struct.unpack_from("<Q", fd, 32)[0]
    if fv_length > args.fd_size or fv_length == 0:
        fail(f"firmware-volume length {fv_length} is invalid for FD size {args.fd_size}")

    print("PASS: ARM64 Image header fields and magic")
    print(f"PASS: image_size={image_size}, bundle_size={len(data)}")
    print(f"PASS: appended PI firmware volume at file offset 0x{fd_offset:x}")
    print(f"PASS: FD destination window 0x{args.fd_base:x}-0x{args.fd_base + args.fd_window:x}")
    print("NOTE: SEC PE/COFF entry point is not yet resolved; shim intentionally does not branch to FD base.")


if __name__ == "__main__":
    main()
