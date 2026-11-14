#!/bin/bash
set -eu

# BootShim is position-independent. Its build must not assign an FD or
# runtime physical load address; placement belongs to a future validated
# runtime handoff contract.
cd BootShim
make
rm -f BootShim.elf
cd ..
