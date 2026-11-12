#!/bin/bash
# Build a bundle for analysis only. Never create a flashable boot/recovery image.
set -e

. build_common.sh
./build_bootshim.sh

# Current EDK2 has removed the legacy GCC5 toolchain tag.
GCC_AARCH64_PREFIX=aarch64-linux-gnu- build -j"$(nproc)" -s -n 0 \
    -a AARCH64 -t GCC -p EXYNOS7885Pkg/Devices/a205w.dsc

python3 - <<'PY'
import hashlib
import json
from pathlib import Path

bootshim = Path("BootShim/BootShim.bin")
firmware = Path("workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd")
workspace = Path("workspace")
bundle = workspace / "UEFI"
manifest = workspace / "UEFI.manifest.json"
checksums = workspace / "SHA256SUMS"

COPY_SIZE = 0x00200000
INTENDED_LOAD_ADDRESS = 0x90000000

if not bootshim.is_file() or not firmware.is_file():
    raise SystemExit("ERROR: BootShim or EDK2 firmware is missing")

bs = bootshim.read_bytes()
fd = firmware.read_bytes()

if len(bs) != 256:
    raise SystemExit(f"ERROR: SAFE MODE BootShim must be exactly 256 bytes, got {len(bs)}")
if not 0 < len(fd) <= COPY_SIZE:
    raise SystemExit(f"ERROR: EDK2 FD size {len(fd)} is invalid or exceeds 2 MiB")

padded_fd = fd + bytes(COPY_SIZE - len(fd))
bundle_data = bs + padded_fd
bundle.write_bytes(bundle_data)

sha = lambda data: hashlib.sha256(data).hexdigest()
metadata = {
    "format": "7K-EDK2-BUNDLE",
    "format_version": 1,
    "architecture": "AARCH64",
    "platform": "Samsung SM-A205W / Exynos 7885",
    "status": "BUNDLE_ONLY_BOOTSHIM_INERT",
    "launch_enabled": False,
    "bootable": False,
    "bootshim": {
        "offset": 0,
        "size": len(bs),
        "sha256": sha(bs),
        "mode": "SAFE_MODE_WFE_LOOP"
    },
    "firmware": {
        "source_size": len(fd),
        "offset": len(bs),
        "padded_size": COPY_SIZE,
        "intended_load_address": hex(INTENDED_LOAD_ADDRESS),
        "load_address_runtime_validated": False,
        "sha256": sha(fd),
        "padded_sha256": sha(padded_fd)
    },
    "bundle": {
        "size": len(bundle_data),
        "sha256": sha(bundle_data)
    },
    "safety": {
        "flashable_images_created": False,
        "boot_recovery_vbmeta_partitions_modified": False,
        "edk2_handoff_implemented": False
    }
}
manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
checksums.write_text(
    sha(bundle_data) + "  UEFI\n" +
    sha(manifest.read_bytes()) + "  UEFI.manifest.json\n",
    encoding="utf-8"
)
print(json.dumps(metadata, indent=2))
PY

(
    cd workspace
    sha256sum -c SHA256SUMS
)

echo "Bundle-only build complete. No boot.img, recovery.img, or tar flash package was created."
