#!/usr/bin/env python3
"""Validate the EDK2 FD and the existing BootShim+FD concatenation.

This is a packaging gate only. It does not validate the Linux Image header,
the EDK2 entry point, or a Linux-to-UEFI handoff.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FD = ROOT / "workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd"
SHIM = ROOT / "BootShim/BootShim.bin"
COMBINED = ROOT / "workspace/UEFI"
FDF = ROOT / "EXYNOS7885Pkg/EXYNOS7885Pkg.fdf"
EXPECTED_BASE = 0x90000000

SEC_PREPI_GUID = "3e401783-cc94-4fcd-97bc-bd35ac369d2f"
EFI_FV_SIGNATURE = b"_FVH"
EFI_FFS_FILETYPE_SEC = 0x03
EFI_SECTION_TE = 0x12
TE_SIGNATURE = 0x5A56
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def inspect_prepi_sec_entry(fd: bytes, fd_base: int) -> dict:
    """Find the PrePi SEC TE image and verify its XIP entry address."""
    if len(fd) < 0x38 or fd[0x28:0x2C] != EFI_FV_SIGNATURE:
        fail("FD does not begin with a valid EFI firmware-volume header")
    fv_length = struct.unpack_from("<Q", fd, 0x20)[0]
    fv_header_length = struct.unpack_from("<H", fd, 0x30)[0]
    if fv_length != len(fd) or not 0x38 <= fv_header_length <= len(fd):
        fail("invalid EFI firmware-volume length/header length")

    # EFI FFS GUIDs are stored in UEFI mixed-endian byte order.
    import uuid
    target_guid_le = uuid.UUID(SEC_PREPI_GUID).bytes_le
    file_pos = align(fv_header_length, 8)
    prepi = None
    while file_pos + 24 <= fv_length:
        if fd[file_pos:file_pos + 24] == b"\xff" * 24:
            file_pos = align(file_pos + 8, 8)
            continue
        file_guid = fd[file_pos:file_pos + 16]
        file_size = int.from_bytes(fd[file_pos + 20:file_pos + 23], "little")
        file_header_size = 24
        if file_size == 0xFFFFFF:
            if file_pos + 32 > fv_length:
                fail("truncated extended FFS header")
            file_size = struct.unpack_from("<Q", fd, file_pos + 24)[0]
            file_header_size = 32
        if file_size < file_header_size or file_pos + file_size > fv_length:
            file_pos = align(file_pos + 8, 8)
            continue
        if file_guid == target_guid_le:
            prepi = (file_pos, file_size, file_header_size, fd[file_pos + 18])
            break
        file_pos = align(file_pos + file_size, 8)

    if prepi is None:
        fail(f"PrePi SEC FFS file {SEC_PREPI_GUID} not found")
    ffs_offset, ffs_size, ffs_header_size, ffs_type = prepi
    if ffs_type != EFI_FFS_FILETYPE_SEC:
        fail(f"PrePi FFS file type is 0x{ffs_type:02x}, expected SEC (0x03)")

    section_pos = align(ffs_offset + ffs_header_size, 4)
    ffs_end = ffs_offset + ffs_size
    te_image = None
    while section_pos + 4 <= ffs_end:
        section_size = int.from_bytes(fd[section_pos:section_pos + 3], "little")
        section_type = fd[section_pos + 3]
        section_header_size = 4
        if section_size == 0xFFFFFF:
            if section_pos + 8 > ffs_end:
                fail("truncated extended FFS section header")
            section_size = struct.unpack_from("<I", fd, section_pos + 4)[0]
            section_header_size = 8
        if section_size < section_header_size or section_pos + section_size > ffs_end:
            fail("invalid section bounds in PrePi FFS file")
        if section_type == EFI_SECTION_TE:
            te_image = (section_pos + section_header_size, section_size - section_header_size)
            break
        section_pos = align(section_pos + section_size, 4)

    if te_image is None:
        fail("PrePi SEC FFS file does not contain a TE image section")
    te_offset, te_size = te_image
    if te_size < 40:
        fail("truncated PrePi TE image header")
    signature, machine, section_count, _subsystem, stripped_size = struct.unpack_from(
        "<HHBBH", fd, te_offset
    )
    entry_rva, _base_of_code = struct.unpack_from("<II", fd, te_offset + 8)
    image_base = struct.unpack_from("<Q", fd, te_offset + 16)[0]
    if signature != TE_SIGNATURE or machine != IMAGE_FILE_MACHINE_ARM64:
        fail("PrePi TE image is not a valid AArch64 TE image")
    if stripped_size < 40 or section_count == 0:
        fail("invalid PrePi TE stripped size or section count")
    section_table_end = 40 + section_count * 40
    if section_table_end > te_size:
        fail("PrePi TE section table exceeds its section bounds")

    executable_matches = []
    for index in range(section_count):
        section = te_offset + 40 + index * 40
        name = fd[section:section + 8].rstrip(b"\x00").decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", fd, section + 8
        )
        characteristics = struct.unpack_from("<I", fd, section + 36)[0]
        extent = max(virtual_size, raw_size)
        if virtual_address <= entry_rva < virtual_address + extent:
            if not characteristics & IMAGE_SCN_MEM_EXECUTE:
                fail("PrePi TE entry point is not inside an executable section")
            raw_file_offset = te_offset + raw_pointer - (stripped_size - 40)
            entry_file_offset = raw_file_offset + (entry_rva - virtual_address)
            if entry_file_offset < te_offset or entry_file_offset + 4 > te_offset + te_size:
                fail("PrePi TE entry-point bytes fall outside the TE image")
            executable_matches.append((name, entry_file_offset))

    if len(executable_matches) != 1:
        fail(f"PrePi TE entry point maps to {len(executable_matches)} executable sections")
    section_name, entry_file_offset = executable_matches[0]
    entry_address = image_base + entry_rva
    if not fd_base <= image_base < fd_base + len(fd):
        fail("PrePi TE image base is outside the reserved FD window")
    if not fd_base <= entry_address <= fd_base + len(fd) - 4:
        fail("PrePi SEC entry point is outside the reserved FD window")
    if entry_address - fd_base != entry_file_offset:
        fail(
            "PrePi SEC image is not linked for XIP at its FD file offset "
            f"(address offset 0x{entry_address - fd_base:x}, "
            f"file offset 0x{entry_file_offset:x})"
        )
    entry_bytes = fd[entry_file_offset:entry_file_offset + 4]
    if entry_bytes in (b"\x00" * 4, b"\xff" * 4):
        fail("PrePi SEC entry point contains erased/empty bytes")

    return {
        "ffs_file_guid": SEC_PREPI_GUID,
        "ffs_file_type": "SEC",
        "ffs_offset_bytes": ffs_offset,
        "te_offset_bytes": te_offset,
        "te_image_base": f"0x{image_base:016X}",
        "te_entry_rva": f"0x{entry_rva:08X}",
        "entrypoint_address": f"0x{entry_address:016X}",
        "entrypoint_offset_bytes": entry_file_offset,
        "entrypoint_section": section_name,
        "entrypoint_bytes_hex": entry_bytes.hex(),
        "entrypoint_mode": "XIP address/file-offset match; runtime ABI not validated",
    }


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def git_commit(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def main() -> None:
    for path in (FD, SHIM, COMBINED, FDF):
        if not path.is_file() or path.stat().st_size == 0:
            fail(f"missing or empty build input: {path.relative_to(ROOT)}")

    fdf = FDF.read_text()
    fd_section = re.search(
        r"\[FD\.EXYNOS7885PKG_UEFI\](.*?)(?=^\[|\Z)",
        fdf, re.MULTILINE | re.DOTALL
    )
    if not fd_section:
        fail("cannot locate [FD.EXYNOS7885PKG_UEFI] in FDF")
    base_match = re.search(r"^BaseAddress\s*=\s*(0x[0-9A-Fa-f]+)", fd_section.group(1), re.MULTILINE)
    size_match = re.search(r"^Size\s*=\s*(0x[0-9A-Fa-f]+)", fd_section.group(1), re.MULTILINE)
    if not base_match or not size_match:
        fail("cannot parse FD BaseAddress/Size from FDF")
    base = int(base_match.group(1), 16)
    capacity = int(size_match.group(1), 16)
    if base != EXPECTED_BASE:
        fail(f"FD base is 0x{base:x}, expected 0x{EXPECTED_BASE:x}")

    fd_size = FD.stat().st_size
    shim_size = SHIM.stat().st_size
    combined_size = COMBINED.stat().st_size
    if fd_size > capacity:
        fail(f"FD is {fd_size} bytes; FDF window permits {capacity}; refusing truncation")
    if combined_size != shim_size + fd_size:
        fail(
            f"combined image size {combined_size} != shim {shim_size} + FD {fd_size}"
        )
    with COMBINED.open("rb") as stream:
        stream.seek(shim_size)
        suffix_hash = hashlib.sha256(stream.read()).hexdigest()
    if suffix_hash != sha256(FD):
        fail("combined image does not contain the exact FD bytes after BootShim.bin")

    with COMBINED.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[56:60] != b"ARM\x64":
        fail("combined image does not have a valid ARM64 Image magic/header")
    text_offset, image_size = struct.unpack_from("<QQ", header, 8)
    if text_offset != 0x80000:
        fail(f"ARM64 Image text_offset is 0x{text_offset:x}, expected 0x80000")
    if image_size != combined_size:
        fail(f"ARM64 Image image_size is {image_size}, expected {combined_size}")
    if header[24:32] != b"\x00" * 8:
        fail("ARM64 Image flags are not the expected zero/default encoding")

    sec_entry = inspect_prepi_sec_entry(FD.read_bytes(), base)

    edk2_dir = ROOT.parent / "edk2"
    if not edk2_dir.is_dir():
        fail("EDK2 checkout directory not found beside project")
    manifest = {
        "format": "SM-A205W-UEFI-PACKAGING",
        "format_version": 1,
        "validation_scope": "FD packaging, ARM64 Image header, and static PrePi SEC TE entrypoint extraction; no runtime handoff validation",
        "fd": {
            "path": str(FD.relative_to(ROOT)),
            "base_address": f"0x{base:08X}",
            "capacity_bytes": capacity,
            "length_bytes": fd_size,
            "sha256": sha256(FD),
        },
        "bootshim": {
            "path": str(SHIM.relative_to(ROOT)),
            "length_bytes": shim_size,
            "sha256": sha256(SHIM),
        },
        "combined": {
            "path": str(COMBINED.relative_to(ROOT)),
            "length_bytes": combined_size,
            "fd_offset_bytes": shim_size,
            "sha256": sha256(COMBINED),
        },
        "edk2_source_commit": git_commit(edk2_dir),
        "prepi_sec_entry": sec_entry,
    }
    output = ROOT / "workspace/phase2-fd-manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    print("PASS: FD packaging integrity checks completed.")


if __name__ == "__main__":
    main()
