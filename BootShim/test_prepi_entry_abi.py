#!/usr/bin/env python3
"""Check the built PrePi SEC entry and BootShim's fail-closed stack setup.

Static source/build checks only. They do not validate live EL, MMU, cache,
DMA, physical placement, or a runtime UEFI handoff.
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
MANIFEST = ROOT / "workspace/UEFI.sec-manifest.json"
ABI_DOC = ROOT / "virtual-boot/docs/ARM64_SEC_ENTRY_ABI.md"

STACK_BOTTOM = 0xE000
STACK_SIZE = 0x2000
SHIM_SIZE = 0x10000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"ERROR: {message}")


def main() -> None:
    for path in (ENTRY, PREPI, INF, SHIM, MANIFEST, ABI_DOC):
        require(path.is_file(), f"missing required input: {path.relative_to(ROOT)}")

    asm = ENTRY.read_text(encoding="utf-8")
    csrc = PREPI.read_text(encoding="utf-8")
    inf = INF.read_text(encoding="utf-8")
    shim = SHIM.read_text(encoding="utf-8")
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

    require(re.search(r"(?m)^\s*_Head:\s*$", shim) is not None,
            "BootShim entry label is missing")
    require(re.search(r"(?m)^\s*adr\s+x9,\s*_BootShimStackTop\s*$", shim) is not None,
            "BootShim does not derive its private stack position-independently")
    require(re.search(r"(?m)^\s*mov\s+sp,\s*x9\s*$", shim) is not None,
            "BootShim does not install its private stack")
    require(re.search(r"(?m)^\s*and\s+x10,\s*x9,\s*#0xF\s*$", shim) is not None
            and re.search(r"(?m)^\s*cbnz\s+x10,\s*_StackFault\s*$", shim) is not None,
            "BootShim does not fail closed on a misaligned stack")
    require(re.search(r"(?m)^\.org\s+0xE000,\s*0\s*$", shim) is not None
            and re.search(r"(?m)^\.space\s+0x2000,\s*0\s*$", shim) is not None
            and re.search(r"(?m)^_BootShimStackBottom:\s*$", shim) is not None
            and re.search(r"(?m)^_BootShimStackTop:\s*$", shim) is not None,
            "BootShim's reserved 8 KiB stack region or its fixed offsets changed")
    stack_setup = shim.index("mov     sp, x9")
    first_wait = shim.index("wfe")
    require(stack_setup < first_wait, "BootShim must install SP before entering its WFE guard")
    require(not re.search(r"(?im)^\s*br\s+x\d+", shim),
            "active register-indirect branch detected in BootShim")
    require(not re.search(r"(?im)^\s*blr\s+x\d+", shim),
            "active register-indirect call detected in BootShim")
    require(f"0x{STACK_BOTTOM:X}" in abi_doc and f"0x{STACK_SIZE:X}" in abi_doc,
            "SEC ABI document does not describe the reserved BootShim stack")
    require(SHIM_SIZE == STACK_BOTTOM + STACK_SIZE,
            "internal stack layout constants do not fill the 64 KiB BootShim")
    require(manifest.get("handoff_enabled") is False,
            "SEC manifest must keep handoff_enabled=false")

    print("PASS: PrePi calls ArmPlatformPeiBootAction before installing its own stack")
    print("PASS: BootShim installs a position-independent, 16-byte-checked 8 KiB stack")
    print("PASS: private stack occupies BootShim offsets 0xE000-0x10000; FD starts at 0x10000")
    print("PASS: PrePi CEntryPoint argument and cache-init sequence are present")
    print("PASS: secondary-core WFE remains multi-core gated")
    print("PASS: BootShim remains fail-closed; no SEC branch; handoff_enabled=false")
    print("LIMIT: live EL/MMU/cache/register/DMA state and physical reservation are not proven")


if __name__ == "__main__":
    main()
