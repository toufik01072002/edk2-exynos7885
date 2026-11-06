#!/bin/bash
set -e
sudo apt update
sudo apt install -y build-essential uuid-dev iasl git nasm gcc-aarch64-linux-gnu bc mkbootimg
# Ubuntu 24.04 ships mkbootimg with a missing optional GKI signing module.
# This build does not sign GKI images, so remove the broken optional import.
sudo sed -i '/^from gki\.generate_gki_certificate import generate_gki_certificate$/d' /usr/bin/mkbootimg
curdir="$PWD"
cd ..
# edk2-stable202302 does not contain MdePkg/Library/ArmSmcLib/ArmSmcLib.inf.
# Pin a release containing both ArmSmcLib and DefaultExceptionHandlerLib used by this platform.
git clone https://github.com/tianocore/edk2 --recursive -b edk2-stable202602
git clone https://github.com/tianocore/edk2-platforms.git
cd "$curdir"
./firstrun.sh
./build.sh
