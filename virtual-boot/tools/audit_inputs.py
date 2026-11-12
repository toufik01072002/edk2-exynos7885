#!/usr/bin/env python3
"""Read-only audit of the A20 virtual-boot inputs. Never creates boot images."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REQUIRED_CONFIG = (
    "CONFIG_ARM64=y",
    "CONFIG_BLK_DEV_INITRD=y",
    "CONFIG_KEXEC=y",
    "CONFIG_DEVTMPFS=y",
    "CONFIG_OF=y",
    "CONFIG_PROC_FS=y",
    "CONFIG_SYSFS=y",
)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--dtb", required=True, type=Path)
    parser.add_argument("--kernel-source", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    for path in (args.kernel, args.dtb):
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"BLOCKED: missing or empty input: {path}")

    kernel_bytes = args.kernel.read_bytes()[:64]
    if len(kernel_bytes) < 64 or kernel_bytes[0x38:0x3c] != b"ARM\\x64":
        raise SystemExit("BLOCKED: prebuilt kernel does not have the AArch64 Image magic at offset 0x38")

    dtb_bytes = args.dtb.read_bytes()[:4]
    if dtb_bytes != bytes.fromhex("d00dfeed"):
        raise SystemExit("BLOCKED: DTB does not begin with the flattened-device-tree magic")

    extractor = args.kernel_source / "scripts/extract-ikconfig"
    if not extractor.is_file():
        raise SystemExit(f"BLOCKED: missing config extractor: {extractor}")
    extracted = subprocess.run(
        ["sh", str(extractor), str(args.kernel)],
        check=False, capture_output=True
    )
    if extracted.returncode != 0 or not extracted.stdout.startswith(b"CONFIG_"):
        raise SystemExit("BLOCKED: cannot extract the actual configuration embedded in the prebuilt kernel")
    config_text = extracted.stdout.decode("utf-8", errors="replace")
    config_lines = set(config_text.splitlines())
    missing = [item for item in REQUIRED_CONFIG if item not in config_lines]
    if missing:
        raise SystemExit("BLOCKED: prebuilt kernel lacks required options: " + ", ".join(missing))

    dtc = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dts", str(args.dtb)],
        check=False, capture_output=True, text=True
    )
    if dtc.returncode != 0:
        raise SystemExit("BLOCKED: dtc could not decode DTB: " + dtc.stderr[-2000:])
    dts = dtc.stdout

    compatibles = re.findall(r'compatible\\s*=\\s*((?:"[^"]*"\\s*,?\\s*)+);', dts)
    memory_nodes = re.findall(r'memory@([0-9a-fA-F]+)\\s*\\{[^}]*?reg\\s*=\\s*<([^>]+)>;', dts, re.S)
    initrd_start = re.search(r'linux,initrd-start\\s*=\\s*<([^>]+)>;', dts)
    initrd_end = re.search(r'linux,initrd-end\\s*=\\s*<([^>]+)>;', dts)
    bootargs = re.search(r'bootargs\\s*=\\s*"([^"]*)";', dts)

    findings = [
        "This report audits input files only; it does not prove that the kernel can boot as a standalone virtual environment.",
        "The EDK2 handoff is not implemented or enabled.",
        "Physical RAM reservation and DMA quiescence have not been proven for this boot path.",
    ]
    compatible_text = " | ".join(compatibles)
    if "samsung,exynos7885" not in compatible_text:
        findings.append("DTB does not declare samsung,exynos7885 compatibility.")
    if "samsung,armv8" in compatible_text and "samsung,exynos7885" in compatible_text:
        findings.append("DTB declares generic Exynos 7885 compatibility; A20 board-specific overlay matching is still unverified.")
    if initrd_start or initrd_end:
        findings.append("DTB contains fixed linux,initrd-start/end properties; these must be removed or rebuilt for a dynamically placed initramfs.")
    if "CONFIG_DEVTMPFS_MOUNT=y" not in config_lines:
        findings.append("CONFIG_DEVTMPFS_MOUNT is disabled; the future initramfs must mount devtmpfs itself.")

    report = {
        "format": "7K-A20-VIRTUAL-BOOT-AUDIT",
        "format_version": 1,
        "launch_enabled": False,
        "inputs": {
            "kernel": {"path": str(args.kernel), "size": args.kernel.stat().st_size, "sha256": sha256(args.kernel), "aarch64_image_magic": True},
            "dtb": {"path": str(args.dtb), "size": args.dtb.stat().st_size, "sha256": sha256(args.dtb), "fdt_magic": True},
        },
        "kernel_config": {
            "extracted_from_prebuilt_image": True,
            "required_options": list(REQUIRED_CONFIG),
            "required_options_present": True,
            "devtmpfs_mount": "CONFIG_DEVTMPFS_MOUNT=y" in config_lines,
        },
        "dtb": {
            "compatible_properties": compatibles,
            "memory_nodes": [{"node": node, "reg_cells": value.split()} for node, value in memory_nodes],
            "linux_initrd_start_cells": initrd_start.group(1).split() if initrd_start else None,
            "linux_initrd_end_cells": initrd_end.group(1).split() if initrd_end else None,
            "bootargs": bootargs.group(1) if bootargs else None,
        },
        "blocking_findings": findings,
        "status": "INPUTS_VALIDATED_HANDOFF_BLOCKED",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    print("AUDIT PASSED: file formats and embedded kernel configuration verified; launch remains disabled.")


if __name__ == "__main__":
    main()
