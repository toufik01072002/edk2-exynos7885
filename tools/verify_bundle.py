#!/usr/bin/env python3
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT / "workspace"

BUNDLE = WORKSPACE / "UEFI"
MANIFEST = WORKSPACE / "UEFI.manifest.json"

EXPECTED_COPY_SIZE = 0x00200000
EXPECTED_BOOTSHIM_SIZE = 0x10000
EXPECTED_LOAD_ADDRESS = "0x90000000"

def fail(message):
    print("BUNDLE VERIFICATION FAILED:", message, file=sys.stderr)
    sys.exit(1)

def sha256(data):
    return hashlib.sha256(data).hexdigest()

if not BUNDLE.is_file() or not MANIFEST.is_file():
    fail("UEFI bundle or manifest missing")

try:
    metadata = json.loads(MANIFEST.read_text())
except Exception as exc:
    fail("Invalid JSON manifest: " + str(exc))

if metadata.get("format") != "7K-EDK2-BUNDLE":
    fail("Unknown bundle format")

if metadata.get("format_version") != 1:
    fail("Unsupported manifest version")

if metadata.get("architecture") != "AARCH64":
    fail("Unexpected architecture")

bs = metadata.get("bootshim", {})
fd = metadata.get("firmware", {})
bundle_meta = metadata.get("bundle", {})

try:
    bs_offset = bs["offset"]
    bs_size = bs["size"]
    fd_offset = fd["offset"]
    fd_source_size = fd["source_size"]
    fd_padded_size = fd["padded_size"]
    fd_copy_size = fd["copy_size"]
    load_address = fd["load_address"]
    declared_bundle_size = bundle_meta["size"]
except (KeyError, TypeError):
    fail("Missing or invalid manifest fields")

if bs_offset != 0:
    fail("BootShim offset must be zero")

if bs_size != EXPECTED_BOOTSHIM_SIZE:
    fail("BootShim must occupy exactly 64 KiB")

if fd_offset != EXPECTED_BOOTSHIM_SIZE:
    fail("Firmware must begin at bundle offset 0x10000")

if fd_padded_size != EXPECTED_COPY_SIZE:
    fail("Unexpected padded firmware size")

if fd_copy_size != EXPECTED_COPY_SIZE:
    fail("Unexpected BootShim copy size")

if load_address != EXPECTED_LOAD_ADDRESS:
    fail("Unexpected firmware destination address")

if not 0 < fd_source_size <= fd_padded_size:
    fail("Invalid original firmware size")

data = BUNDLE.read_bytes()

if len(data) != declared_bundle_size:
    fail("Bundle size differs from manifest")

expected_size = bs_size + fd_padded_size
if len(data) != expected_size:
    fail("Bundle layout or total size is inconsistent")

bootshim_data = data[bs_offset:bs_offset + bs_size]
firmware_area = data[fd_offset:fd_offset + fd_padded_size]
firmware_data = firmware_area[:fd_source_size]
padding = firmware_area[fd_source_size:]

if sha256(bootshim_data) != bs.get("sha256"):
    fail("BootShim SHA-256 mismatch")

if sha256(firmware_data) != fd.get("sha256"):
    fail("Original firmware SHA-256 mismatch")

if sha256(firmware_area) != fd.get("padded_sha256"):
    fail("Padded firmware SHA-256 mismatch")

if any(padding):
    fail("Firmware padding contains non-zero bytes")

if sha256(data) != bundle_meta.get("sha256"):
    fail("Complete bundle SHA-256 mismatch")

print("BUNDLE VERIFICATION OK")
print("BootShim:", bs_size, "bytes")
print("Firmware original:", fd_source_size, "bytes")
print("Firmware padded:", fd_padded_size, "bytes")
print("Bundle total:", len(data), "bytes")
print("Firmware destination:", load_address)
print("All SHA-256 and layout checks passed")
