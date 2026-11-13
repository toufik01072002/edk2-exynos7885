#!/bin/bash
set -e
cd BootShim
# The packaged BootShim always keeps the WFE safety gate enabled.
make UEFI_BASE=0x90000000 UEFI_SIZE=0x00200000 SEC_ENTRY=0x90008280 ENABLE_HANDOFF=0
# Compile (but do not package) the candidate FD-copy/SEC-entry path.
make handoff-candidate UEFI_BASE=0x90000000 UEFI_SIZE=0x00200000 SEC_ENTRY=0x90008280
rm -f BootShim.elf BootShim-handoff-candidate.elf
cd ..
