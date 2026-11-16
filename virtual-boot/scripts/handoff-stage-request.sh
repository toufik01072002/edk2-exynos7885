#!/bin/busybox sh
# Explicit offline staging request. This never writes physical RAM or executes SEC.
set -eu

BOOTSHIM=${BOOTSHIM_PATH:-/boot/edk2/BootShim.bin}
DESCRIPTOR=${DESCRIPTOR_PATH:-/boot/edk2/handoff-descriptor.bin}
OUTPUT=${OUTPUT_PATH:-/tmp/BootShim-staged.bin}
STAGER=${HANDOFF_STAGE_BIN:-/sbin/handoff-stage}

if [ ! -f "$BOOTSHIM" ] || [ ! -f "$DESCRIPTOR" ]; then
    echo "HANDOFF STAGING BLOCKED"
    echo "Required files:"
    echo "  $BOOTSHIM"
    echo "  $DESCRIPTOR"
    echo "No descriptor is generated and no addresses are inferred."
    exit 1
fi

if [ -e "$OUTPUT" ]; then
    echo "HANDOFF STAGING BLOCKED: output already exists: $OUTPUT"
    echo "Remove or rename it explicitly before retrying."
    exit 1
fi

if [ ! -x "$STAGER" ]; then
    echo "HANDOFF STAGING BLOCKED: stager is not executable: $STAGER"
    exit 1
fi

"$STAGER" "$BOOTSHIM" "$DESCRIPTOR" "$OUTPUT"
echo "Staged image: $OUTPUT"
echo "STATUS: offline file staging only; no physical-memory write, kexec, or SEC transfer."
