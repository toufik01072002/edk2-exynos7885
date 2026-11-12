#!/usr/bin/env python3
"""Locate the A205W SEC PrePi TE image in a built PI firmware volume."""
import argparse
import json
import struct
import sys
import uuid
from pathlib import Path

SEC_GUID = uuid.UUID("3e401783-cc94-4fcd-97bc-bd35ac369d2f")
FV_SIGNATURE = b"_FVH"
TE_SIGNATURE = b"VZ"
TE_MACHINE_AARCH64 = 0xAA64
FFS_FILETYPE_SEC = 0x03
SECTION_TE = 0x12
TE_HEADER_SIZE = 40


def align(value, boundary):
    return (value + boundary - 1) & ~(boundary - 1)


def fail(message):
    raise SystemExit("ERROR: " + message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("fd", type=Path)
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0x90000000)
    args = parser.parse_args()
    data = args.fd.read_bytes()
    if len(data) < 0x1000:
        fail("FD is too small to contain a PI firmware volume")
    if data[40:44] != FV_SIGNATURE:
        fail("PI FV signature _FVH not found at FD offset 0x28")
    fv_length = struct.unpack_from("<Q", data, 32)[0]
    header_length = struct.unpack_from("<H", data, 48)[0]
    if fv_length > len(data) or fv_length < header_length or header_length < 56:
        fail(f"invalid FV length/header length: {fv_length:#x}/{header_length:#x}")

    target = SEC_GUID.bytes_le
    cursor = align(header_length, 8)
    found = []
    while cursor + 24 <= fv_length:
        header = data[cursor:cursor + 24]
        if header == b"\xff" * 24:
            break
        name = header[:16]
        file_type = header[18]
        file_size = int.from_bytes(header[20:23], "little")
        if file_size == 0 or file_size == 0xFFFFFF:
            cursor = align(cursor + 8, 8)
            continue
        if file_size < 24 or cursor + file_size > fv_length:
            fail(f"invalid FFS file size at FD offset {cursor:#x}: {file_size:#x}")
        if name == target:
            if file_type != FFS_FILETYPE_SEC:
                fail(f"PrePi GUID found but FFS type is {file_type:#x}, not SEC")
            section_cursor = cursor + 24
            file_end = cursor + file_size
            while section_cursor + 4 <= file_end:
                section_size = int.from_bytes(data[section_cursor:section_cursor + 3], "little")
                section_type = data[section_cursor + 3]
                section_header_size = 4
                if section_size == 0xFFFFFF:
                    if section_cursor + 8 > file_end:
                        fail("truncated extended section header")
                    section_size = struct.unpack_from("<I", data, section_cursor + 4)[0]
                    section_header_size = 8
                if section_size < section_header_size or section_cursor + section_size > file_end:
                    fail(f"invalid section size at FD offset {section_cursor:#x}")
                if section_type == SECTION_TE:
                    payload_offset = section_cursor + section_header_size
                    if payload_offset + TE_HEADER_SIZE > section_cursor + section_size:
                        fail("truncated TE image header")
                    te = data[payload_offset:payload_offset + TE_HEADER_SIZE]
                    sig, machine, sections, subsystem, stripped, entry_rva, base_code, image_base = struct.unpack_from(
                        "<HHBBHIIQ", te, 0
                    )
                    if sig != 0x5A56 or data[payload_offset:payload_offset + 2] != TE_SIGNATURE:
                        fail("TE signature mismatch")
                    if machine != TE_MACHINE_AARCH64:
                        fail(f"SEC TE machine is {machine:#x}, expected AArch64 0xAA64")
                    if stripped < TE_HEADER_SIZE:
                        fail(f"invalid TE StrippedSize {stripped}")
                    adjustment = stripped - TE_HEADER_SIZE
                    xip_entry = args.base + payload_offset + entry_rva - adjustment
                    te_entry = image_base + entry_rva - adjustment if image_base else None
                    found.append({
                        "fd_base": hex(args.base),
                        "fv_length": hex(fv_length),
                        "prepi_ffs_offset": hex(cursor),
                        "prepi_ffs_size": hex(file_size),
                        "te_section_offset": hex(section_cursor),
                        "te_payload_offset": hex(payload_offset),
                        "te_image_base_field": hex(image_base),
                        "te_entry_rva": hex(entry_rva),
                        "te_stripped_size": hex(stripped),
                        "te_number_of_sections": sections,
                        "te_subsystem": subsystem,
                        "xip_entry_address": hex(xip_entry),
                        "te_imagebase_entry_if_nonzero": hex(te_entry) if te_entry is not None else None,
                    })
                section_cursor = align(section_cursor + section_size, 4)
        cursor = align(cursor + file_size, 8)

    if len(found) != 1:
        fail(f"expected exactly one PrePi SEC TE image, found {len(found)}")
    result = found[0]
    if int(result["xip_entry_address"], 16) < args.base or int(result["xip_entry_address"], 16) >= args.base + fv_length:
        fail("computed XIP SEC entry falls outside the firmware volume")
    if result["te_image_base_field"] != "0x0" and int(result["te_imagebase_entry_if_nonzero"], 16) != int(result["xip_entry_address"], 16):
        fail("TE ImageBase-derived entry disagrees with the in-place FV address")
    print(json.dumps({"status": "SEC_TE_FOUND", **result}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as exc:
        fail(str(exc))
