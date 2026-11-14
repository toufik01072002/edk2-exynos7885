#!/usr/bin/env python3
"""Regression tests for the fail-closed bundle provenance validator."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

VALIDATOR = Path(__file__).resolve().parents[1] / "scripts" / "validate_handoff_bundle_provenance.py"
FILES = {
    "kernel": "Image",
    "initramfs": "initramfs.cpio.gz",
    "dtb": "exynos7885-a205w-virtual.dtb",
}


class BundleProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        segments = {}
        manifest = {}
        for name, filename in FILES.items():
            data = (name + "-fixture-bytes").encode()
            (self.bundle / filename).write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            segments[name] = {
                "artifact_present": True,
                "file": filename,
                "size": len(data),
                "sha256": digest,
                "physical_address": None,
            }
            manifest[name if name != "kernel" else "kernel"] = {"sha256": digest}
        self.report = self.root / "report.json"
        self.manifest = self.root / "manifest.json"
        self.report.write_text(json.dumps({"state": "DISABLED", "segments": segments}))
        self.manifest.write_text(json.dumps({
            "kernel": manifest["kernel"],
            "initramfs": manifest["initramfs"],
            "dtb": manifest["dtb"],
            "handoff_protocol": {"state": "DISABLED"},
        }))

    def tearDown(self):
        self.tmp.cleanup()

    def run_validator(self):
        return subprocess.run(
            [sys.executable, str(VALIDATOR), "--bundle-dir", str(self.bundle),
             "--report", str(self.report), "--manifest", str(self.manifest)],
            text=True, capture_output=True,
        )

    def update_json(self, path, change):
        value = json.loads(path.read_text())
        change(value)
        path.write_text(json.dumps(value))

    def test_valid_bundle_passes(self):
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_modified_artifact_is_rejected(self):
        with (self.bundle / "Image").open("ab") as stream:
            stream.write(b"tamper")
        self.assertNotEqual(self.run_validator().returncode, 0)

    def test_manifest_hash_mismatch_is_rejected(self):
        self.update_json(self.manifest, lambda value: value["kernel"].update(sha256="0" * 64))
        self.assertNotEqual(self.run_validator().returncode, 0)

    def test_enabled_handoff_is_rejected(self):
        self.update_json(self.report, lambda value: value.update(state="ENABLED"))
        self.assertNotEqual(self.run_validator().returncode, 0)

    def test_physical_address_claim_is_rejected(self):
        self.update_json(self.report, lambda value: value["segments"]["kernel"].update(physical_address="0x80080000"))
        self.assertNotEqual(self.run_validator().returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
