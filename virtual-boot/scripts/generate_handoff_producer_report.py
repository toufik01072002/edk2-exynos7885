#!/usr/bin/env python3
"""Generate provenance for available Virtual Boot artifacts without assigning RAM addresses."""
import argparse
import hashlib
import json
from pathlib import Path

SEGMENTS = ("kernel", "initramfs", "dtb", "bootshim", "uefi_fd")


def describe(path):
    if path is None:
        return {
            "artifact_present": False,
            "file": None,
            "size": None,
            "sha256": None,
            "physical_address": None,
            "address_source": "UNASSIGNED",
        }
    p = Path(path)
    if not p.is_file():
        raise SystemExit("Required artifact is missing: " + str(p))
    digest = hashlib.sha256()
    size = 0
    with p.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(chunk)
            digest.update(chunk)
    return {
        "artifact_present": True,
        "file": p.name,
        "size": size,
        "sha256": digest.hexdigest(),
        "physical_address": None,
        "address_source": "UNASSIGNED",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initramfs", required=True)
    parser.add_argument("--dtb", required=True)
    parser.add_argument("--bootshim")
    parser.add_argument("--uefi-fd")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    artifacts = {
        "kernel": describe(args.kernel),
        "initramfs": describe(args.initramfs),
        "dtb": describe(args.dtb),
        "bootshim": describe(args.bootshim),
        "uefi_fd": describe(args.uefi_fd),
    }
    report = {
        "format": "A20-HANDOFF-PRODUCER-REPORT",
        "format_version": 1,
        "descriptor_abi": "A20HND01",
        "state": "DISABLED",
        "purpose": "Artifact provenance only; not a physical handoff descriptor",
        "segments": artifacts,
        "activation_blockers": [
            "physical reservations are not verified",
            "runtime physical addresses are unassigned",
            "kernel register contract is not verified",
            "EL/MMU/cache state is not verified",
            "DMA quiescence is not verified",
            "secondary CPU state is not verified",
            "runtime hardware validation is pending",
        ],
    }
    Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\\n")


if __name__ == "__main__":
    main()
