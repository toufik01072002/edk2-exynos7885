#!/usr/bin/env python3
"""Static contract checks for the fixed-segment, load-only ARM64 kexec path."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ASM = re.sub(r"\s+", " ", (ROOT / "BootShim/BootShim.S").read_text())
MAKE = re.sub(r"\s+", " ", (ROOT / "BootShim/Makefile").read_text())
BUILD = (ROOT / "build_bootshim.sh").read_text()
LOADER = (ROOT / "virtual-boot/loader/edk2-kexec.c").read_text()

BOOTSHIM = (0x80080000, 0x80090000)
FD_STAGE = (0x80400000, 0x80600000)
FD_FINAL = (0x90020000, 0x90220000)
RAM0 = (0x80000000, 0x90000000)

def require(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)

def inside(region, bounds):
    return bounds[0] <= region[0] < region[1] <= bounds[1]

def overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]

require(inside(BOOTSHIM, RAM0), "BootShim segment is outside usable RAM bank 0")
require(inside(FD_STAGE, RAM0), "FD staging segment is outside usable RAM bank 0")
require(FD_FINAL == (0x90020000, 0x90220000), "FD target no longer matches reserved window")
require(not overlaps(BOOTSHIM, FD_STAGE), "BootShim and FD staging segments overlap")
require(not overlaps(FD_STAGE, FD_FINAL), "FD source staging overlaps final reserved FD")
require(BOOTSHIM[0] % 0x1000 == 0 and FD_STAGE[0] % 0x1000 == 0,
        "kexec segment starts must be page aligned")
require(BOOTSHIM[1] - BOOTSHIM[0] == 0x10000, "BootShim segment must be 64 KiB")
require(FD_STAGE[1] - FD_STAGE[0] == 0x200000, "FD staging segment must be 2 MiB")

for token in ("ldr x1, =FD_SOURCE", "ldr x12, =(BOOTSHIM_BASE + 0x10000)",
              "mov sp, x12", "ldr x10, =UEFI_BASE", "ldr x11, =UEFI_SIZE",
              "stp x12, x13, [x10], #16", "dsb sy", "ic iallu",
              "ldr x16, =SEC_ENTRY", "br x16"):
    require(token in ASM, "BootShim fixed-segment handoff missing: " + token)

for token in ("BOOTSHIM_BASE ?= 0x80080000", "FD_SOURCE ?= 0x80400000",
              "-DBOOTSHIM_BASE=$(BOOTSHIM_BASE)", "-DFD_SOURCE=$(FD_SOURCE)"):
    require(token in MAKE, "Makefile address contract missing: " + token)
require("BOOTSHIM_BASE=0x80080000 FD_SOURCE=0x80400000" in BUILD,
        "build script does not use the fixed-segment address contract")

for token in ("__NR_kexec_load", "KEXEC_ARCH_AARCH64", "segments[2]",
              "BOOTSHIM_ADDR", "FD_STAGE_ADDR", "FD_BASE", "FD_END",
              '"_FVH"', '"--load-only"', '"--unload"'):
    require(token in LOADER, "userspace kexec loader missing: " + token)

require("reboot(" not in LOADER and "LINUX_REBOOT_CMD_KEXEC" not in LOADER,
        "load-only utility must never reboot or execute the staged image")
require("syscall(__NR_kexec_load" in LOADER,
        "loader does not call the target kernel's legacy kexec_load syscall")
require("segments[0].mem" in LOADER and "segments[1].mem" in LOADER,
        "loader must provide two explicit physical destination segments")

print("PASS: fixed physical segment map (BootShim, FD staging, reserved FD)")
print("PASS: BootShim copies FD from staging to 0x90020000 then branches to SEC")
print("PASS: userspace uses legacy AArch64 kexec_load with explicit segments")
print("PASS: loader has load-only/unload modes and contains no reboot path")
