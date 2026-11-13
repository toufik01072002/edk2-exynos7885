#!/usr/bin/env python3
"""Guard the single-core PrePi path against an unconditional WFE wait."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "EXYNOS7885Pkg/PrePi/PrePi.c"
DSC = ROOT / "EXYNOS7885Pkg/EXYNOS7885Pkg.dsc"

source = SOURCE.read_text(encoding="utf-8")
dsc = DSC.read_text(encoding="utf-8")

if not re.search(
    r"if\s*\(\s*FixedPcdGet32\s*\(\s*PcdCoreCount\s*\)\s*>\s*1\s*\)"
    r"\s*\{\s*//[^\n]*\n(?:\s*//[^\n]*\n)*\s*ArmCallWFE\s*\(\s*\)\s*;",
    source,
):
    raise SystemExit("FAIL: ArmCallWFE is not guarded by PcdCoreCount > 1")

if len(re.findall(r"\bArmCallWFE\s*\(", source)) != 1:
    raise SystemExit("FAIL: unexpected additional ArmCallWFE call site")

if not re.search(
    r"gArmPlatformTokenSpaceGuid\.PcdCoreCount\s*\|\s*1\b", dsc
):
    raise SystemExit("FAIL: platform DSC no longer declares PcdCoreCount = 1")

print("PASS: single-core build bypasses WFE; SMP builds retain the wait.")
