#!/bin/busybox sh
# Explicit offline staging request. This never writes physical RAM or executes SEC.
set -eu

BOOTSHIM=/boot/edk2/BootShim.bin
DESCRIPTOR=/boot/edk2/handoff-descriptor.bin
OUTPUT=/tmp/BootShim-staged.bin

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

/sbin/handoff-stage "$BOOTSHIM" "$DESCRIPTOR" "$OUTPUT"
echo "Staged image: $OUTPUT"
echo "STATUS: offline file staging only; no physical-memory write, kexec, or SEC transfer."
