#!/bin/bash
set -e
cd BootShim
# The packaged BootShim always keeps the WFE safety gate enabled.
make UEFI_BASE=0x90020000 UEFI_SIZE=0x00200000 SEC_ENTRY=0x90028278 BOOTSHIM_BASE=0x80080000 FD_SOURCE=0x80400000 ENABLE_HANDOFF=0
# Build the fixed-segment candidate separately. The runtime loader exposes
# only --load-only; this candidate is not executed by the build or init script.
make handoff-candidate UEFI_BASE=0x90020000 UEFI_SIZE=0x00200000 SEC_ENTRY=0x90028278 BOOTSHIM_BASE=0x80080000 FD_SOURCE=0x80400000
rm -f BootShim.elf BootShim-handoff-candidate.elf
cd ..
