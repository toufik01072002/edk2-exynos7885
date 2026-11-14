#!/usr/bin/env python3
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

script = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    inputs = {}
    for name, payload in (("kernel", b"kernel-image"), ("initramfs", b"initramfs-image"), ("dtb", b"dtb-image")):
        path = root / name
        path.write_bytes(payload)
        inputs[name] = path
    output = root / "report.json"
    subprocess.run([
        sys.executable, str(script),
        "--kernel", str(inputs["kernel"]),
        "--initramfs", str(inputs["initramfs"]),
        "--dtb", str(inputs["dtb"]),
        "--output", str(output),
    ], check=True)
    report = json.loads(output.read_text())
    assert report["state"] == "DISABLED"
    assert report["descriptor_abi"] == "A20HND01"
    for name, payload in (("kernel", b"kernel-image"), ("initramfs", b"initramfs-image"), ("dtb", b"dtb-image")):
        segment = report["segments"][name]
        assert segment["artifact_present"] is True
        assert segment["size"] == len(payload)
        assert segment["sha256"] == hashlib.sha256(payload).hexdigest()
        assert segment["physical_address"] is None
        assert segment["address_source"] == "UNASSIGNED"
    for name in ("bootshim", "uefi_fd"):
        segment = report["segments"][name]
        assert segment["artifact_present"] is False
        assert segment["size"] is None
        assert segment["sha256"] is None
        assert segment["physical_address"] is None
    assert len(report["activation_blockers"]) >= 6
print("PASS: producer report hashes only present artifacts and leaves all RAM addresses unassigned")
