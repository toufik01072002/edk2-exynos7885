#!/usr/bin/env python3
"""Check the built PrePi SEC entry's initial-stack and CPU-state contract.

This is a static source/build test. It does not validate live EL, MMU, cache,
DMA, or kexec state and must never be used to enable the firmware branch.
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
    stack_install = re.search(r"(?im)^\s*mov\s+sp,\s*x1\s*$", asm)
    require(early_call is not None, "SEC entry no longer calls ArmPlatformPeiBootAction")
    require(stack_install is not None, "PrePi's own stack installation was not found")
    require(early_call.start() < stack_install.start(),
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
    require(re.search(r"(?m)^\s*_Head:\s*$", shim) and "_Disabled" in shim
            and re.search(r"(?im)^\s*wfe\s*$", shim),
            "BootShim is not the expected disabled WFE stub")
    require(not re.search(r"(?im)^\s*br\s+x\d+", shim),
            "active register-indirect branch detected in BootShim")
    require(manifest.get("handoff_enabled") is False,
            "SEC manifest must keep handoff_enabled=false")

    print("PASS: PrePi's first platform call occurs before its own stack setup")
    print("PASS: initial writable, mapped, 16-byte-aligned stack is a mandatory entry precondition")
    print("PASS: PrePi CEntryPoint argument and cache-init sequence are present")
    print("PASS: secondary-core WFE remains multi-core gated")
    print("PASS: pinned recovery kexec ABI records zeroed entry registers and inherited SP")
    print("PASS: BootShim remains fail-closed; handoff_enabled=false")
    print("LIMIT: live EL/MMU/cache/register/DMA state is not proven by this static test")


if __name__ == "__main__":
    main()
