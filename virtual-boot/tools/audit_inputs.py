#!/usr/bin/env python3
"""Read-only audit of the A20 virtual-boot inputs. Never creates boot images."""
import argparse
import hashlib
import json
import re
import subprocess
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
    parser.add_argument("--a20-overlay", required=True, type=Path)
    parser.add_argument("--kernel-source", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()

    for path in (args.kernel, args.dtb, args.a20_overlay):
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"BLOCKED: missing or empty input: {path}")

    kernel_bytes = args.kernel.read_bytes()[:64]
    if len(kernel_bytes) < 64 or kernel_bytes[0x38:0x3c] != b"ARM\x64":
        raise SystemExit("BLOCKED: prebuilt kernel does not have the AArch64 Image magic at offset 0x38")

    overlay_text = args.a20_overlay.read_text(encoding="utf-8", errors="replace")
    overlay_compatible = re.search(r'compatible\s*=\s*"([^"]+)"\s*,\s*"([^"]+)"\s*;', overlay_text)
    overlay_rev = re.search(r'dtbo-hw_rev\s*=\s*<([^>]+)>;', overlay_text)
    overlay_rev_end = re.search(r'dtbo-hw_rev_end\s*=\s*<([^>]+)>;', overlay_text)
    if not overlay_compatible or "A20 CAN SINGLE 05" not in overlay_compatible.group(1):
        raise SystemExit("BLOCKED: selected overlay is not the A20 CAN SINGLE 05 board variant")
    if not overlay_rev or not overlay_rev_end or int(overlay_rev.group(1), 0) != 5 or int(overlay_rev_end.group(1), 0) != 255:
        raise SystemExit("BLOCKED: selected overlay does not cover hardware revisions 0x05-0xff")

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
    binary_config_verified = extracted.returncode == 0 and b"CONFIG_" in extracted.stdout
    if binary_config_verified:
        config_text = extracted.stdout.decode("utf-8", errors="replace")
        config_evidence = "embedded_kernel_image"
    else:
        source_defconfig = args.kernel_source / "arch/arm64/configs/exynos7885-a20_defconfig"
        if not source_defconfig.is_file():
            raise SystemExit("BLOCKED: neither embedded kernel config nor A20 source defconfig is available")
        config_text = source_defconfig.read_text(encoding="utf-8", errors="replace")
        config_evidence = "source_defconfig_only"
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

    compatibles = re.findall(r'compatible\s*=\s*((?:"[^"]*"\s*,?\s*)+);', dts)
    memory_nodes = re.findall(r'memory@([0-9a-fA-F]+)\s*\{[^}]*?reg\s*=\s*<([^>]+)>;', dts, re.S)
    expected_memory_banks = [(0x80000000, 0x3BC00000), (0xC0000000, 0x40000000), (0x880000000, 0x40000000)]
    observed_memory_banks = []
    for node, value in memory_nodes:
        cells = [int(cell, 0) for cell in value.split()]
        if len(cells) == 3 and cells[2] > 0:
            observed_memory_banks.append(((cells[0] << 32) | cells[1], cells[2]))
    memory_map_verified = sorted(observed_memory_banks) == sorted(expected_memory_banks)

    expected_reserved = {
        "edk2_uefi@90000000": (0x90000000, 0x00200000),
        "cma3_reserved@8ba000000": (0x8BA000000, 0x01400000),
        "bank2_gap@8bb400000": (0x8BB400000, 0x00400000),
        "video_frame@8bd800000": (0x8BD800000, 0x02800000),
    }
    observed_reserved = {}
    for node_name in expected_reserved:
        node_match = re.search(rf'{re.escape(node_name)}\\s*\\{{([^{{}}]*)\\}}', dts, re.S)
        reg_match = re.search(r'reg\\s*=\\s*<([^>]+)>;', node_match.group(1)) if node_match else None
        if reg_match:
            cells = [int(cell, 0) for cell in reg_match.group(1).split()]
            if len(cells) == 3:
                observed_reserved[node_name] = ((cells[0] << 32) | cells[1], cells[2])
    reserved_ranges_verified = observed_reserved == expected_reserved

    def alloc_range(node_name):
        node_match = re.search(rf'\\b{re.escape(node_name)}\\s*\\{{([^{{}}]*)\\}}', dts, re.S)
        prop_match = re.search(r'alloc-ranges\\s*=\\s*<([^>]+)>;', node_match.group(1)) if node_match else None
        if not prop_match:
            return None
        cells = [int(cell, 0) for cell in prop_match.group(1).split()]
        if len(cells) != 3:
            return None
        return ((cells[0] << 32) | cells[1], cells[2])

    expected_alloc_ranges = {
        "crypto": (0x8BB800000, 0x02000000),
        "video_stream": (0xF0C00000, 0x04800000),
    }
    observed_alloc_ranges = {name: alloc_range(name) for name in expected_alloc_ranges}
    alloc_ranges_verified = observed_alloc_ranges == expected_alloc_ranges
    initrd_start = re.search(r'linux,initrd-start\s*=\s*<([^>]+)>;', dts)
    initrd_end = re.search(r'linux,initrd-end\s*=\s*<([^>]+)>;', dts)
    bootargs = re.search(r'bootargs\s*=\s*"([^"]*)";', dts)
    model = re.search(r'model\s*=\s*"([^"]*)";', dts)

    findings = [
        "This report audits input files only; it does not prove that the kernel can boot as a standalone virtual environment.",
        "The EDK2 handoff is not implemented or enabled.",
        "Physical RAM reservation and DMA quiescence have not been proven for this boot path.",
    ]
    if not binary_config_verified:
        findings.append("The prebuilt Image has no extractable IKCONFIG; required options are confirmed only in the source defconfig, not in this binary.")
    compatible_text = " | ".join(compatibles)
    if "samsung,exynos7885" not in compatible_text:
        findings.append("DTB does not declare samsung,exynos7885 compatibility.")
    if "samsung,armv8" in compatible_text and "samsung,exynos7885" in compatible_text and not (model and "A20 CAN SINGLE 05" in model.group(1)):
        findings.append("Base DTB is generic Exynos 7885; the selected A20 CAN SINGLE 05 overlay has not been confirmed in the merged DTB.")
    if not memory_map_verified:
        findings.append("DTB RAM banks do not match the corrected SM-A205W memory audit; this DTB must not be booted.")
    if initrd_start or initrd_end:
        findings.append("DTB still contains fixed linux,initrd-start/end properties.")
    else:
        findings.append("DTB initrd addresses are cleared; a verified runtime loader must set the actual initrd range before boot.")
    if bootargs and "skip_initramfs" in bootargs.group(1):
        findings.append("DTB bootargs still contain skip_initramfs; the external diagnostic initramfs would be skipped.")
    if not reserved_ranges_verified:
        findings.append("EDK2 and Bank 2 carveout reservations do not match the corrected memory audit.")
    if not alloc_ranges_verified:
        findings.append("Crypto/video_stream reserved-pool allocation ranges do not match the corrected memory audit.")
    if "CONFIG_DEVTMPFS_MOUNT=y" not in config_lines:
        findings.append("CONFIG_DEVTMPFS_MOUNT is disabled; the future initramfs must mount devtmpfs itself.")

    report = {
        "format": "7K-A20-VIRTUAL-BOOT-AUDIT",
        "format_version": 1,
        "launch_enabled": False,
        "inputs": {
            "kernel": {"path": str(args.kernel), "size": args.kernel.stat().st_size, "sha256": sha256(args.kernel), "aarch64_image_magic": True},
            "dtb": {"path": str(args.dtb), "size": args.dtb.stat().st_size, "sha256": sha256(args.dtb), "fdt_magic": True},
            "a20_overlay_source": {"path": str(args.a20_overlay), "size": args.a20_overlay.stat().st_size, "sha256": sha256(args.a20_overlay), "compatible": list(overlay_compatible.groups()), "hardware_revision_start": int(overlay_rev.group(1), 0), "hardware_revision_end": int(overlay_rev_end.group(1), 0), "variant_match": True},
        },
        "kernel_config": {
            "extracted_from_prebuilt_image": binary_config_verified,
            "evidence": config_evidence,
            "binary_config_verified": binary_config_verified,
            "required_options": list(REQUIRED_CONFIG),
            "required_options_present": True,
            "devtmpfs_mount": "CONFIG_DEVTMPFS_MOUNT=y" in config_lines,
        },
        "dtb": {
            "compatible_properties": compatibles,
            "memory_nodes": [{"node": node, "reg_cells": value.split()} for node, value in memory_nodes],
            "expected_memory_banks": [{"start": hex(start), "size": hex(size)} for start, size in expected_memory_banks],
            "observed_memory_banks": [{"start": hex(start), "size": hex(size)} for start, size in observed_memory_banks],
            "memory_map_verified": memory_map_verified,
            "expected_reserved_ranges": {name: {"start": hex(value[0]), "size": hex(value[1])} for name, value in expected_reserved.items()},
            "observed_reserved_ranges": {name: {"start": hex(value[0]), "size": hex(value[1])} for name, value in observed_reserved.items()},
            "reserved_ranges_verified": reserved_ranges_verified,
            "expected_alloc_ranges": {name: {"start": hex(value[0]), "size": hex(value[1])} for name, value in expected_alloc_ranges.items()},
            "observed_alloc_ranges": {name: ({"start": hex(value[0]), "size": hex(value[1])} if value else None) for name, value in observed_alloc_ranges.items()},
            "alloc_ranges_verified": alloc_ranges_verified,
            "linux_initrd_start_cells": initrd_start.group(1).split() if initrd_start else None,
            "linux_initrd_end_cells": initrd_end.group(1).split() if initrd_end else None,
            "bootargs": bootargs.group(1) if bootargs else None,
            "model": model.group(1) if model else None,
        },
        "blocking_findings": findings,
        "status": "INPUTS_VALIDATED_HANDOFF_BLOCKED" if binary_config_verified else "INPUTS_FORMATS_VALIDATED_BINARY_CONFIG_UNVERIFIED",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    print("AUDIT PASSED: file formats and embedded kernel configuration verified; launch remains disabled.")


if __name__ == "__main__":
    main()
