#!/bin/bash
# Shared EDK2 environment for scripts launched from this repository root.
set -e

PROJECT_ROOT="$PWD"
EDK2_DIR="$PROJECT_ROOT/../edk2"
export PACKAGES_PATH="$EDK2_DIR:$PROJECT_ROOT/../edk2-platforms:$PROJECT_ROOT"
export WORKSPACE="$PROJECT_ROOT/workspace"

# edksetup.sh must be sourced while the current directory is the EDK2 root.
pushd "$EDK2_DIR" >/dev/null
. ./edksetup.sh
popd >/dev/null
