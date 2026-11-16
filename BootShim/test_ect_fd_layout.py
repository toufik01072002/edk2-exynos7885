#!/usr/bin/env python3
"""Regression guard for the Samsung ECT and fixed EDK2 FD memory layout."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MEMORY_MAP = ROOT / "EXYNOS7885Pkg/Include/Configuration/DeviceMemoryMap.h"
FDF = ROOT / "EXYNOS7885Pkg/EXYNOS7885Pkg.fdf"
DTS = ROOT / "virtual-boot/dtb/exynos7885-a205w-virtual.dts"
GATE_H = ROOT / "BootShim/sec_entry_gate.h"
GATE_C = ROOT / "BootShim/sec_entry_gate.c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def main() -> None:
    memory = MEMORY_MAP.read_text(encoding="utf-8")
    fdf = FDF.read_text(encoding="utf-8")
    dts = DTS.read_text(encoding="utf-8")
    gate_h = GATE_H.read_text(encoding="utf-8")
    gate_c = GATE_C.read_text(encoding="utf-8")

    expected_memory = (
        r'\{"ECT Reserved",\s*0x90000000,\s*0x00020000,',
        r'\{"UEFI FD Reserved",\s*0x90020000,\s*0x00200000,',
        r'\{"RAM Bank 0 Mid",\s*0x90220000,\s*0x27DE0000,',
    )
    for pattern in expected_memory:
        require(re.search(pattern, memory) is not None,
                f"DeviceMemoryMap.h is missing expected region: {pattern}")

    require(re.search(r'(?im)^\s*BaseAddress\s*=\s*0x90020000\|', fdf) is not None,
            "EDK2 FDF must link the 2 MiB FD at 0x90020000")
    require(re.search(r'(?im)^\s*Size\s*=\s*0x00200000\|', fdf) is not None,
            "EDK2 FD size must remain 2 MiB")

    dts_regions = (
        r'ect-parameters@90000000\s*\{[^}]*reg\s*=\s*<0x0\s+0x90000000\s+0x00020000>;[^}]*no-map;',
        r'uefi-fd@90020000\s*\{[^}]*reg\s*=\s*<0x0\s+0x90020000\s+0x00200000>;[^}]*no-map;',
    )
    for pattern in dts_regions:
        require(re.search(pattern, dts, re.S) is not None,
                f"Virtual Boot DTS is missing the matching no-map reservation: {pattern}")

    require("reg = <0x0 0x80000000 0x3BC00000>;" in dts,
            "Virtual Boot DTS bank 0 must match the established 956 MiB map")
    require("BOOTSHIM_FD_LINK_BASE     UINT64_C(0x90020000)" in gate_h,
            "SEC evidence gate FD base does not match the ECT-safe destination")
    require("ranges_overlap(e->bootshim_base, shim_end, e->fd_base, fd_end)" in gate_c,
            "SEC gate must reject BootShim/FD overlap without assuming they are adjacent")
    require("shim_end != e->fd_base" not in gate_c,
            "SEC gate still incorrectly requires BootShim to be adjacent to the FD")

    require("uefi-bundle@8fff0000" not in dts and "0x8FFF0000-0x9020FFFF" not in memory,
            "stale combined BootShim/FD reservation remains in the memory layout")
    print("PASS: Samsung ECT reservation is 0x90000000-0x90020000")
    print("PASS: EDK2 FD is linked and reserved at 0x90020000-0x90220000")
    print("PASS: DTS and EDK2 memory map agree; BootShim staging remains independent")
    print("PASS: SEC gate rejects overlapping ranges and does not require adjacency")
    print("LIMIT: static layout only; no physical runtime placement or handoff is enabled")


if __name__ == "__main__":
    main()
