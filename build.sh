#!/bin/bash
# based on the instructions from edk2-platform
set -e
. build_common.sh
./build_bootshim.sh
# Current EDK2 has removed the legacy GCC5 toolchain tag.
GCC_AARCH64_PREFIX=aarch64-linux-gnu- build -j$(nproc) -s -n 0 -a AARCH64 -t GCC -p EXYNOS7885Pkg/Devices/a205w.dsc
python3 - <<'BUNDLE'
import hashlib
import json
from pathlib import Path

bootshim = Path("BootShim/BootShim.bin")
firmware = Path("workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd")
bundle = Path("workspace/UEFI")
manifest = Path("workspace/UEFI.manifest.json")

COPY_SIZE = 0x00200000
LOAD_ADDRESS = 0x90000000

if not bootshim.is_file() or not firmware.is_file():
    raise SystemExit("ERROR: BootShim or firmware missing")

bs = bootshim.read_bytes()
fd = firmware.read_bytes()

if not 256 <= len(bs) <= 65536:
    raise SystemExit("ERROR: BootShim size outside expected limits")

if not 0 < len(fd) <= COPY_SIZE:
    raise SystemExit("ERROR: Firmware size invalid or exceeds 2 MiB")

padded_fd = fd + bytes(COPY_SIZE - len(fd))
bundle_data = bs + padded_fd
bundle.write_bytes(bundle_data)

sha = lambda data: hashlib.sha256(data).hexdigest()

metadata = {
    "format": "7K-EDK2-BUNDLE",
    "format_version": 1,
    "architecture": "AARCH64",
    "platform": "Samsung SM-A205W / Exynos 7885",
    "bootshim": {
        "offset": 0,
        "size": len(bs),
        "sha256": sha(bs)
    },
    "firmware": {
        "source_size": len(fd),
        "offset": len(bs),
        "padded_size": COPY_SIZE,
        "load_address": hex(LOAD_ADDRESS),
        "copy_size": COPY_SIZE,
        "sha256": sha(fd),
        "padded_sha256": sha(padded_fd)
    },
    "bundle": {
        "size": len(bundle_data),
        "sha256": sha(bundle_data)
    }
}

manifest.write_text(json.dumps(metadata, indent=2) + "\n")

print("Bundle generated:", bundle)
print("BootShim:", len(bs), "bytes")
print("Firmware:", len(fd), "bytes")
print("Firmware padded:", COPY_SIZE, "bytes")
print("Bundle:", len(bundle_data), "bytes")
print("Manifest:", manifest)
BUNDLE
# Bundle experimental uniquement : aucune image Android flashable.
(cd workspace && sha256sum UEFI UEFI.manifest.json > SHA256SUMS)
