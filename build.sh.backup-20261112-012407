#!/bin/bash
# based on the instructions from edk2-platform
set -e
. build_common.sh
./build_bootshim.sh
# Current EDK2 has removed the legacy GCC5 toolchain tag.
GCC_AARCH64_PREFIX=aarch64-linux-gnu- build -j$(nproc) -s -n 0 -a AARCH64 -t GCC -p EXYNOS7885Pkg/Devices/a205w.dsc
cat BootShim/BootShim.bin workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd > workspace/UEFI
mkbootimg --kernel workspace/UEFI -o workspace/boot.img
cd workspace/
tar -c boot.img -f boot.tar
cd ..
