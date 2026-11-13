#!/usr/bin/env python3
"""Validate the EDK2 FD, its PrePi SEC entry, and the BootShim bundle.

The checks are static packaging/entrypoint checks only. They do not prove
physical placement, processor state, or a runtime Linux-to-UEFI handoff.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import struct
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
AARCH64_BL_MASK = 0xFC000000
AARCH64_BL_OPCODE = 0x94000000


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
    entry_instruction = struct.unpack_from("<I", fd, entry_file_offset)[0]
    if entry_instruction & AARCH64_BL_MASK != AARCH64_BL_OPCODE:
        fail(
            "PrePi SEC entry does not begin with the expected AArch64 BL "
            f"instruction (found 0x{entry_instruction:08X})"
        )
    branch_imm = entry_instruction & 0x03FFFFFF
    if branch_imm & (1 << 25):
        branch_imm -= 1 << 26
    branch_target = entry_address + (branch_imm << 2)
    section_rva_start = next(
        struct.unpack_from("<I", fd, te_offset + 40 + i * 40 + 12)[0]
        for i in range(section_count)
        if fd[te_offset + 40 + i * 40:te_offset + 40 + i * 40 + 8].rstrip(b"\\x00").decode("ascii", "replace") == section_name
    )
    section_virtual_size = next(
        struct.unpack_from("<I", fd, te_offset + 40 + i * 40 + 8)[0]
        for i in range(section_count)
        if fd[te_offset + 40 + i * 40:te_offset + 40 + i * 40 + 8].rstrip(b"\\x00").decode("ascii", "replace") == section_name
    )
    if not image_base + section_rva_start <= branch_target < image_base + section_rva_start + section_virtual_size:
        fail("PrePi SEC entry BL target is outside its executable section")

