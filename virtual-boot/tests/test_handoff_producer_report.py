#!/usr/bin/env python3
"""Exercise artifact provenance generation and fail-closed input handling."""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

script = Path(sys.argv[1]).resolve()


def run(*args, check=True):
    return subprocess.run(
        [sys.executable, str(script), *map(str, args)],
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    payloads = {
        "kernel": b"kernel-image",
        "initramfs": b"initramfs-image",
        "dtb": b"dtb-image",
        "bootshim": b"bootshim-image",
        "uefi_fd": b"uefi-fd-image",
    }
    inputs = {}
    for name, payload in payloads.items():
        path = root / name
        path.write_bytes(payload)
        inputs[name] = path

    output = root / "report.json"
    run(
        "--kernel", inputs["kernel"],
        "--initramfs", inputs["initramfs"],
        "--dtb", inputs["dtb"],
        "--bootshim", inputs["bootshim"],
        "--uefi-fd", inputs["uefi_fd"],
        "--output", output,
    )
    report = json.loads(output.read_text())
    assert report["format"] == "A20-HANDOFF-PRODUCER-REPORT"
    assert report["state"] == "DISABLED"
    assert report["descriptor_abi"] == "A20HND01"
    assert set(report["segments"]) == set(payloads)
    for name, payload in payloads.items():
        segment = report["segments"][name]
        assert segment["artifact_present"] is True
        assert segment["file"] == inputs[name].name
        assert segment["size"] == len(payload)
        assert segment["sha256"] == hashlib.sha256(payload).hexdigest()
        assert segment["physical_address"] is None
        assert segment["address_source"] == "UNASSIGNED"
    assert len(report["activation_blockers"]) >= 6

    # Optional artifacts omitted by the caller stay explicitly absent.
    absent_output = root / "absent-report.json"
    run(
        "--kernel", inputs["kernel"],
        "--initramfs", inputs["initramfs"],
        "--dtb", inputs["dtb"],
        "--output", absent_output,
    )
    absent = json.loads(absent_output.read_text())
    for name in ("bootshim", "uefi_fd"):
        segment = absent["segments"][name]
        assert segment["artifact_present"] is False
        assert segment["file"] is None
        assert segment["size"] is None
        assert segment["sha256"] is None
        assert segment["physical_address"] is None

    # Missing required input must fail and must not produce a report.
    missing_required = root / "missing-kernel"
    rejected_output = root / "rejected-report.json"
    result = run(
        "--kernel", missing_required,
        "--initramfs", inputs["initramfs"],
        "--dtb", inputs["dtb"],
        "--output", rejected_output,
        check=False,
    )
    assert result.returncode != 0
    assert not rejected_output.exists()

    # An explicitly requested but missing optional artifact is also an error.
    result = run(
        "--kernel", inputs["kernel"],
        "--initramfs", inputs["initramfs"],
        "--dtb", inputs["dtb"],
        "--bootshim", root / "missing-bootshim",
        "--output", rejected_output,
        check=False,
    )
    assert result.returncode != 0
    assert not rejected_output.exists()

print("PASS: provenance hashes every supplied artifact, preserves absent optionals, rejects missing inputs, and leaves all RAM addresses unassigned")
