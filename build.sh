#!/usr/bin/env bash
# Build the EDK2 firmware first; BootShim needs the exact resulting FD size.
set -euo pipefail
. build_common.sh

# Current EDK2 has removed the legacy GCC5 toolchain tag.
GCC_AARCH64_PREFIX=aarch64-linux-gnu- build -j"$(nproc)" -s -n 0 -a AARCH64 -t GCC -p EXYNOS7885Pkg/Devices/a205w.dsc

# Build a Linux Image-compatible wrapper and append the real FD payload.
./build_bootshim.sh

# Validate header, embedded FD and final bundle before packaging.
python3 virtual-boot/scripts/validate_bootshim_image.py \
  workspace/UEFI \
  --fd-size "$(stat -c '%s' workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd)" \
  --fd-window 0x00200000 \
  --fd-base 0x90000000

mkbootimg --kernel workspace/UEFI -o workspace/boot.img
cd workspace/
tar -c boot.img -f boot.tar
cd ..
