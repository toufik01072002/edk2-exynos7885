#!/usr/bin/env python3
"""Cross-check shipped bundle bytes against provenance and manifest metadata."""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED = {
    "kernel": ("Image", ("kernel", "sha256")),
    "initramfs": ("initramfs.cpio.gz", ("initramfs", "sha256")),
    "dtb": ("exynos7885-a205w-virtual.dtb", ("dtb", "sha256")),
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()

    report = json.loads(args.report.read_text())
    manifest = json.loads(args.manifest.read_text())
    if report.get("state") != "DISABLED":
        raise SystemExit("Producer report must keep handoff DISABLED")
    if manifest.get("handoff_protocol", {}).get("state") != "DISABLED":
        raise SystemExit("Hardware manifest must keep handoff DISABLED")

    segments = report.get("segments", {})
    for name, (filename, manifest_path) in EXPECTED.items():
        artifact = args.bundle_dir / filename
        if not artifact.is_file():
            raise SystemExit("Bundle artifact missing: " + str(artifact))
        actual_size = artifact.stat().st_size
        actual_hash = sha256(artifact)
        segment = segments.get(name, {})
        if segment.get("artifact_present") is not True:
            raise SystemExit(name + " is not marked present in provenance report")
        if segment.get("file") != filename:
            raise SystemExit(name + " provenance filename mismatch")
        if segment.get("size") != actual_size or segment.get("sha256") != actual_hash:
            raise SystemExit(name + " provenance does not match bundled bytes")
        if segment.get("physical_address") is not None:
            raise SystemExit(name + " unexpectedly has a physical address")
        manifest_segment = manifest
        for key in manifest_path:
            manifest_segment = manifest_segment.get(key, {})
        if manifest_segment.get("sha256") != actual_hash:
            raise SystemExit(name + " manifest SHA-256 does not match bundled bytes")

    for name in ("bootshim", "uefi_fd"):
        segment = segments.get(name, {})
        if segment.get("physical_address") is not None:
            raise SystemExit(name + " unexpectedly has a physical address")
    print("PASS: kernel, initramfs and DTB hashes/sizes match the bundle and hardware manifest")
    print("PASS: provenance and manifest keep physical handoff disabled")


if __name__ == "__main__":
    main()
