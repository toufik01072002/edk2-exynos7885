#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
FD="$ROOT/workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd"
SHIM_DIR="$ROOT/BootShim"
BUNDLE="$ROOT/workspace/UEFI"
UEFI_BASE=0x90000000
UEFI_WINDOW=0x00200000
TEXT_OFFSET=0x00080000

test -s "$FD" || { echo "ERROR: EDK2 FD missing: $FD" >&2; exit 1; }
FD_SIZE=$(stat -c '%s' "$FD")
if (( FD_SIZE > UEFI_WINDOW )); then
  echo "ERROR: FD ($FD_SIZE bytes) exceeds reserved 2 MiB window" >&2
  exit 1
fi
if (( FD_SIZE == 0 || FD_SIZE % 16 != 0 )); then
  echo "ERROR: FD size must be non-zero and divisible by 16" >&2
  exit 1
fi

cd "$SHIM_DIR"
make clean
# First pass obtains the position-independent shim length.
make UEFI_BASE="$UEFI_BASE" UEFI_SIZE="$FD_SIZE" \
  TEXT_OFFSET="$TEXT_OFFSET" IMAGE_SIZE=0
SHIM_SIZE=$(stat -c '%s' BootShim.bin)
IMAGE_SIZE=$((SHIM_SIZE + FD_SIZE))

# The appended FD must begin exactly at the _Payload symbol.
PAYLOAD_OFFSET=$(aarch64-linux-gnu-nm -n BootShim.elf | awk '$3 == "_Payload" { print "0x" $1 }')
if [[ -z "$PAYLOAD_OFFSET" ]] || (( PAYLOAD_OFFSET != SHIM_SIZE )); then
  echo "ERROR: _Payload offset ($PAYLOAD_OFFSET) does not match BootShim size ($SHIM_SIZE)" >&2
  exit 1
fi

# Second pass writes the exact Linux Image image_size field.
make clean
make UEFI_BASE="$UEFI_BASE" UEFI_SIZE="$FD_SIZE" \
  TEXT_OFFSET="$TEXT_OFFSET" IMAGE_SIZE="$IMAGE_SIZE"

mkdir -p "$(dirname "$BUNDLE")"
cat BootShim.bin "$FD" > "$BUNDLE"
test "$(stat -c '%s' "$BUNDLE")" -eq "$IMAGE_SIZE"
echo "BootShim bytes: $SHIM_SIZE"
echo "UEFI FD bytes:  $FD_SIZE"
echo "Image bytes:    $IMAGE_SIZE"
echo "FD destination: $UEFI_BASE (reserved window $UEFI_WINDOW bytes)"
