#!/usr/bin/env python3
"""Check PrePi's SEC entry and the fail-closed, linker-laid-out BootShim.

These are static source/manifest checks. They do not prove live EL, MMU,
cache, DMA, physical placement, or a runtime UEFI handoff.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "EXYNOS7885Pkg/PrePi/AArch64/ModuleEntryPoint.S"
PREPI = ROOT / "EXYNOS7885Pkg/PrePi/PrePi.c"
INF = ROOT / "EXYNOS7885Pkg/PrePi/PrePi.inf"
SHIM = ROOT / "BootShim/BootShim.S"
LINKER = ROOT / "BootShim/bootshim.ld"
MANIFEST = ROOT / "workspace/UEFI.sec-manifest.json"
ABI_DOC = ROOT / "virtual-boot/docs/ARM64_SEC_ENTRY_ABI.md"

STACK_BOTTOM = 0xE000
STACK_SIZE = 0x2000
SHIM_SIZE = 0x10000
MAILBOX_OFFSET = 0xD000
MAILBOX_SIZE = 0x100
SEC_SLOT_OFFSET = 0xD100


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def main() -> None:
    for path in (ENTRY, PREPI, INF, SHIM, LINKER, MANIFEST, ABI_DOC):
        require(path.is_file(), f"missing required input: {path.relative_to(ROOT)}")

    asm = ENTRY.read_text(encoding="utf-8")
    csrc = PREPI.read_text(encoding="utf-8")
    inf = INF.read_text(encoding="utf-8")
    shim = SHIM.read_text(encoding="utf-8")
    linker = LINKER.read_text(encoding="utf-8")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    abi_doc = ABI_DOC.read_text(encoding="utf-8")

    early_call = re.search(r"(?im)^\s*bl\s+ASM_PFX\(ArmPlatformPeiBootAction\)", asm)
    prepi_stack = re.search(r"(?im)^\s*mov\s+sp,\s*x1\s*$", asm)
    require(early_call is not None, "SEC entry no longer calls ArmPlatformPeiBootAction")
    require(prepi_stack is not None, "PrePi's own stack installation was not found")
    require(early_call.start() < prepi_stack.start(),
            "entry ordering changed: review the initial-stack ABI before updating this test")
    require("PcdSystemMemoryUefiRegionSize" in asm and "PcdFdBaseAddress" in asm,
            "PrePi stack placement no longer derives from the configured FD/memory layout")
    require(re.search(r"(?im)^\s*mov\s+x0,\s*xzr", asm) is None,
            "unexpected register clobber inserted before CEntryPoint setup")
    require("x0 = MpId" in asm and "x1 = UefiMemoryBase" in asm and "x2 = StacksBase" in asm,
            "PrePi's internal CEntryPoint argument contract changed")
    require("ArmDisableDataCache" in csrc and "ArmInvalidateInstructionCache" in csrc
            and "ArmEnableInstructionCache" in csrc,
            "PrePi cache initialization sequence changed; review transition assumptions")
    require(re.search(r"if\s*\(FixedPcdGet32\s*\(PcdCoreCount\)\s*>\s*1\)\s*\{\s*ArmCallWFE\s*\(\);", csrc),
            "secondary-core WFE is no longer guarded by PcdCoreCount > 1")
    require(re.search(r"MODULE_TYPE\s*=\s*SEC", inf, re.IGNORECASE),
            "PrePi module is no longer typed as SEC")

    # The stack is a NOLOAD linker section, not an assembler .org region.
    require(re.search(r'(?m)^\.section\s+\.stack,"aw",%nobits\s*$', shim) is not None,
            "BootShim private stack is not emitted as a NOLOAD input section")
    require(re.search(r"(?m)^_BootShimStackBottom:\s*$", shim) is not None
            and re.search(r"(?m)^\.space\s+0x2000\s*$", shim) is not None
            and re.search(r"(?m)^_BootShimStackTop:\s*$", shim) is not None,
            "BootShim source no longer reserves exactly 8 KiB between stack symbols")
    require(re.search(r"(?m)^\s*_BootShimStackBottom:\s*$", shim) is not None,
            "BootShim stack bottom symbol is missing")
    require(re.search(r"(?m)^\s*adr\s+x9,\s*_BootShimStackTop\s*$", shim) is not None
            and re.search(r"(?m)^\s*mov\s+sp,\s*x9\s*$", shim) is not None,
            "BootShim does not install its position-independent private stack")
    require(re.search(r"(?m)^\s*and\s+x10,\s*x9,\s*#0xF\s*$", shim) is not None
            and re.search(r"(?m)^\s*cbnz\s+x10,\s*_StackFault\s*$", shim) is not None,
            "BootShim does not fail closed on a misaligned stack")
    require("mov     sp, x9" in shim and shim.index("mov     sp, x9") < shim.index("wfe"),
            "BootShim must install SP before entering its WFE guard")

    require(re.search(r'(?m)^\.section\s+\.mailbox,"aw",%progbits\s*$', shim) is not None
            and re.search(r"(?m)^\.space\s+0x100,\s*0\s*$", shim) is not None,
            "descriptor mailbox must remain a 256-byte input section")
    require(re.search(r'(?m)^\.section\s+\.sec_offset,"a",%progbits\s*$', shim) is not None
            and re.search(r"(?m)^\.quad\s+0\s*$", shim) is not None,
            "SEC entry offset slot must remain an 8-byte input section")

    # Verify fixed output placement through the linker script's section map.
    require(re.search(r"(?m)^\s*\.\s*=\s*0x0000D000;\s*$", linker) is not None
            and re.search(r"(?m)^\s*\.mailbox\s*:\s*\{\s*KEEP\(\*\(\.mailbox\)\)\s*\}", linker) is not None,
            "linker no longer places the descriptor mailbox at 0xD000")
    require(re.search(r"(?m)^\s*\.\s*=\s*0x0000D100;\s*$", linker) is not None
            and re.search(r"(?m)^\s*\.sec_offset\s*:\s*\{\s*KEEP\(\*\(\.sec_offset\)\)\s*\}", linker) is not None,
            "linker no longer places the SEC offset slot at 0xD100")
    require(re.search(r"(?m)^\s*\.\s*=\s*0x0000E000;\s*$", linker) is not None
            and re.search(r"(?m)^\s*\.stack\s*\(NOLOAD\)\s*:\s*\{\s*KEEP\(\*\(\.stack\)\)\s*\}", linker) is not None,
            "linker no longer places the NOLOAD stack at 0xE000")
    require(f"SIZEOF(.mailbox) == 0x{MAILBOX_SIZE:X}" in linker
            and "SIZEOF(.sec_offset) == 8" in linker
            and "ADDR(.stack) + SIZEOF(.stack) == 0x10000" in linker,
            "linker ASSERTs no longer enforce mailbox, SEC slot, and 64 KiB image bounds")
    require(SHIM_SIZE == STACK_BOTTOM + STACK_SIZE,
            "8 KiB stack no longer fills BootShim offsets 0xE000-0x10000")
    require(f"0x{STACK_BOTTOM:X}" in abi_doc and f"0x{STACK_SIZE:X}" in abi_doc,
            "SEC ABI document does not describe the reserved BootShim stack")

    # This revision is intentionally validation-only: never transfer to SEC.
    require(re.search(r"(?m)^\s*bl\s+bootshim_validate_embedded_handoff_descriptor\s*$", shim) is not None,
            "BootShim descriptor validation call is missing")
    require(re.search(r"(?m)^\s*cbnz\s+w0,\s*_DescriptorFault\s*$", shim) is not None,
            "BootShim does not fail closed on an invalid descriptor")
    require("_RuntimeEvidenceUnavailable:" in shim
            and re.search(r"(?m)^\s*wfe\s*$", shim) is not None,
            "BootShim no longer halts when runtime handoff evidence is unavailable")
    require(re.search(r"(?m)^\s*br\s+x16\s*$", shim) is None
            and re.search(r"(?m)^\s*adr\s+x16,\s*_Head\s*$", shim) is None,
            "validation-only BootShim must not branch to SEC")

    require(manifest.get("handoff_enabled") is False,
            "SEC manifest must keep handoff_enabled=false")
    fd = manifest.get("fd", {})
    sec = manifest.get("entry_contract", {}).get("sec_entry", {})
    require(fd.get("runtime_physical_load_address") is None
            and fd.get("runtime_placement_status") == "UNVERIFIED",
            "FD runtime physical placement must remain explicitly unassigned")
    require(sec.get("runtime_address") is None
            and sec.get("runtime_address_status") == "UNVERIFIED",
            "SEC runtime entry address must remain explicitly unassigned")

    print("PASS: PrePi early call, stack ordering, arguments, and cache sequence")
    print("PASS: BootShim private stack is linker-laid-out at 0xE000-0x10000 (NOLOAD)")
    print("PASS: mailbox 0xD000 and SEC slot 0xD100 retain their fixed linker offsets")
    print("PASS: BootShim validates the descriptor and halts without transferring to SEC")
    print("PASS: runtime physical placement remains unassigned")
    print("LIMIT: live EL/MMU/cache/register/DMA state and physical reservation are not proven")


if __name__ == "__main__":
    main()
