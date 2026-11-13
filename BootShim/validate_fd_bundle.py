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
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FD = ROOT / "workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd"
SHIM = ROOT / "BootShim/BootShim.bin"
COMBINED = ROOT / "workspace/UEFI"
FDF = ROOT / "EXYNOS7885Pkg/EXYNOS7885Pkg.fdf"
EXPECTED_BASE = 0x90000000


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

    edk2_dir = ROOT.parent / "edk2"
    if not edk2_dir.is_dir():
        fail("EDK2 checkout directory not found beside project")
    manifest = {
        "format": "SM-A205W-UEFI-PACKAGING",
        "format_version": 1,
        "validation_scope": "FD size/base and exact BootShim+FD concatenation only",
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
    }
    output = ROOT / "workspace/phase2-fd-manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    print("PASS: FD packaging integrity checks completed.")


if __name__ == "__main__":
    main()
