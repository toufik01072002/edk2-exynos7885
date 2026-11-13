#!/usr/bin/env python3
"""Validate the EDK2 FD and the static BootShim+FD package.

These checks validate file layout and the built PrePi SEC entry only. They do
not prove kexec physical placement, processor state, or a runtime UEFI handoff.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FD = ROOT / "workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd"
SHIM = ROOT / "BootShim/BootShim.bin"
COMBINED = ROOT / "workspace/UEFI"
MANIFEST = ROOT / "workspace/phase2-fd-manifest.json"
FDF = ROOT / "EXYNOS7885Pkg/EXYNOS7885Pkg.fdf"

EXPECTED_BASE = 0x90000000
EXPECTED_SIZE = 0x00200000
EXPECTED_TEXT_OFFSET = 0x80000
HEADER_SIZE = 64
IMAGE_MAGIC = b"ARM\x64"

SEC_PREPI_GUID = "3e401783-cc94-4fcd-97bc-bd35ac369d2f"
EFI_FV_SIGNATURE = b"_FVH"
EFI_FFS_FILETYPE_SEC = 0x03
EFI_SECTION_TE = 0x12
TE_SIGNATURE = 0x5A56
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_SCN_MEM_EXECUTE = 0x20000000
AARCH64_BL_MASK = 0xFC000000
AARCH64_BL_OPCODE = 0x94000000


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect_fdf() -> tuple[int, int]:
    text = FDF.read_text(encoding="utf-8")
    base_match = re.search(r"(?im)^\s*BaseAddress\s*=\s*(0x[0-9a-f]+)", text)
    size_match = re.search(r"(?im)^\s*Size\s*=\s*(0x[0-9a-f]+)", text)
    if not base_match or not size_match:
        fail("could not read BaseAddress and Size from the EDK2 FDF")
    base = int(base_match.group(1), 16)
    size = int(size_match.group(1), 16)
    if base != EXPECTED_BASE:
        fail(f"FDF base is 0x{base:X}, expected 0x{EXPECTED_BASE:X}")
    if size != EXPECTED_SIZE:
        fail(f"FDF size is 0x{size:X}, expected 0x{EXPECTED_SIZE:X}")
    return base, size


def inspect_prepi_sec_entry(fd: bytes, fd_base: int) -> dict:
    """Find PrePi SEC's TE image and validate its XIP entry-point mapping."""
    if len(fd) < 0x38 or fd[0x28:0x2C] != EFI_FV_SIGNATURE:
        fail("FD does not begin with a valid EFI firmware-volume header")
    fv_length = struct.unpack_from("<Q", fd, 0x20)[0]
    fv_header_length = struct.unpack_from("<H", fd, 0x30)[0]
    if fv_length != len(fd) or not 0x38 <= fv_header_length <= len(fd):
        fail("invalid EFI firmware-volume length/header length")

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
        fail("PrePi SEC TE image is not valid AArch64")
    if stripped_size < 40 or section_count == 0:
        fail("invalid PrePi TE stripped size or section count")
    if 40 + section_count * 40 > te_size:
        fail("PrePi TE section table exceeds its section bounds")

    matches = []
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
                fail("PrePi TE entry point is not in an executable section")
            raw_offset = te_offset + raw_pointer - (stripped_size - 40)
            entry_file_offset = raw_offset + (entry_rva - virtual_address)
            if entry_file_offset < te_offset or entry_file_offset + 4 > te_offset + te_size:
                fail("PrePi TE entry-point bytes fall outside the TE image")
            matches.append({
                "section": name,
                "section_rva": virtual_address,
                "section_virtual_size": virtual_size,
                "entry_file_offset": entry_file_offset,
            })

    if len(matches) != 1:
        fail(f"PrePi SEC entry maps to {len(matches)} executable sections")
    match = matches[0]
    entry_address = image_base + entry_rva
    expected_xip_address = fd_base + match["entry_file_offset"]
    if entry_address != expected_xip_address:
        fail(
            f"PrePi SEC entry XIP mismatch: TE gives 0x{entry_address:X}, "
            f"FD offset gives 0x{expected_xip_address:X}"
        )

    instruction = struct.unpack_from("<I", fd, match["entry_file_offset"])[0]
    if instruction & AARCH64_BL_MASK != AARCH64_BL_OPCODE:
        fail(f"PrePi SEC entry does not begin with AArch64 BL (0x{instruction:08X})")
    branch_imm = instruction & 0x03FFFFFF
    if branch_imm & (1 << 25):
        branch_imm -= 1 << 26
    branch_target = entry_address + (branch_imm << 2)
    section_start = image_base + match["section_rva"]
    section_end = section_start + match["section_virtual_size"]
    if not section_start <= branch_target < section_end:
        fail("PrePi SEC entry BL target is outside its executable section")

    return {
        "te_file_offset": te_offset,
        "te_image_base": f"0x{image_base:08X}",
        "entry_rva": f"0x{entry_rva:X}",
        "entry_address": f"0x{entry_address:08X}",
        "entry_fd_offset": f"0x{match['entry_file_offset']:X}",
        "entry_instruction": instruction.to_bytes(4, "little").hex(" "),
        "entry_branch_target": f"0x{branch_target:08X}",
        "executable_section": match["section"],
    }


def main() -> None:
    for path in (FDF, FD, SHIM, COMBINED):
        if not path.is_file():
            fail(f"required build output is missing: {path}")
    fd_base, fd_size = inspect_fdf()
    fd = FD.read_bytes()
    if len(fd) != fd_size:
        fail(f"FD length is 0x{len(fd):X}, expected exactly 0x{fd_size:X}")
    sec = inspect_prepi_sec_entry(fd, fd_base)
    inf = (ROOT / "EXYNOS7885Pkg/PrePi/PrePi.inf").read_text(encoding="utf-8")
    asm = (ROOT / "EXYNOS7885Pkg/PrePi/AArch64/ModuleEntryPoint.S").read_text(encoding="utf-8")
    csrc = (ROOT / "EXYNOS7885Pkg/PrePi/PrePi.c").read_text(encoding="utf-8")
    if not re.search(r"(?m)^\\s*MODULE_TYPE\\s*=\\s*SEC\\s*$", inf): fail("PrePi.inf is not MODULE_TYPE = SEC")
    if "_ModuleEntryPoint" not in asm or "CEntryPoint" not in asm: fail("SEC assembly entry chain is incomplete")
    if not re.search(r"(?m)^CEntryPoint\\s*\\(", csrc): fail("PrePi.c does not define CEntryPoint")
    shim, combined = SHIM.read_bytes(), COMBINED.read_bytes()
    if len(shim) != 0x10000: fail("BootShim must be exactly 64 KiB")
    if combined[:len(shim)] != shim: fail("BootShim prefix mismatch")
    if combined[len(shim):len(shim)+len(fd)] != fd: fail("FD is not at bundle offset 0x10000")
    if len(combined) != len(shim) + EXPECTED_SIZE: fail("bundle must contain 64 KiB BootShim plus 2 MiB FD")
    manifest = {"format":"7K-EDK2-BUNDLE-SEC-MANIFEST", "handoff_enabled":False, "validation_scope":"static SEC entry and bundle layout only; no runtime handoff validation", "fd":{"base":f"0x{fd_base:08X}","size":len(fd),"sha256":sha256(fd),"bundle_offset":len(shim),"sec_entry":sec}, "bootshim":{"size":len(shim),"sha256":sha256(shim)}, "bundle":{"size":len(combined),"sha256":sha256(combined)}}
    (ROOT / "workspace/UEFI.sec-manifest.json").write_text(json.dumps(manifest, indent=2) + "\\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
