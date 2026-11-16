#!/bin/bash
# based on the instructions from edk2-platform
set -e
. build_common.sh
./build_bootshim.sh
# Current EDK2 has removed the legacy GCC5 toolchain tag.
GCC_AARCH64_PREFIX=aarch64-linux-gnu- build -j$(nproc) -s -n 0 -a AARCH64 -t GCC -p EXYNOS7885Pkg/Devices/a205w.dsc
python3 tools/create_bundle.py
# Bundle experimental uniquement : aucune image Android flashable.
(cd workspace && sha256sum UEFI UEFI.manifest.json > SHA256SUMS)
