#!/bin/bash
set -e
sudo apt update
sudo apt install -y build-essential uuid-dev iasl git nasm gcc-aarch64-linux-gnu bc mkbootimg
curdir="$PWD"
cd ..
# Follow the upstream project's tested setup: clone the default EDK2 branch.
# edk2-stable202302 does not contain MdePkg/Library/ArmSmcLib/ArmSmcLib.inf.
# Pin a release containing both ArmSmcLib and DefaultExceptionHandlerLib used by this platform.
git clone https://github.com/tianocore/edk2 --recursive -b edk2-stable202602
git clone https://github.com/tianocore/edk2-platforms.git
cd "$curdir"
./firstrun.sh
./build.sh
