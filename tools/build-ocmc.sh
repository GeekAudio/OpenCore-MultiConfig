#!/bin/bash
#
# Build the OpenCore Multi-Config (OCMC) fork.
#
# Usage: ./build-ocmc.sh [DEBUG|RELEASE]   (default: DEBUG)
#
# Requires: Xcode command line tools, and nasm/mtoc/iasl in ../toolchain/bin
#           (already prepared in this workspace).
#
set -e

TARGET="${1:-DEBUG}"
SRC="$(cd "$(dirname "$0")" && pwd)/OpenCorePkg-1.0.8"
TC="$(cd "$(dirname "$0")" && pwd)/toolchain/bin"
OUT="$(cd "$(dirname "$0")" && pwd)/../OC-多配置/out"

export PATH="${TC}:${PATH}"
export WORKSPACE="${SRC}/UDK"
export EDK_TOOLS_PATH="${SRC}/UDK/BaseTools"
export PACKAGES_PATH="${SRC}/UDK:${SRC}"
export CONF_PATH="${SRC}/UDK/Conf"

CORES="$(sysctl -n hw.ncpu)"

echo "==> Checking tools"
nasm -v >/dev/null
iasl -v >/dev/null
mtoc --version >/dev/null

echo "==> Building EDK2 BaseTools (skipped when already built)"
if [ ! -x "${EDK_TOOLS_PATH}/Source/C/bin/GenFfs" ]; then
  make -C "${EDK_TOOLS_PATH}" -j "${CORES}"
fi

cd "${SRC}/UDK"
if [ ! -e OpenCorePkg ]; then
  ln -s .. OpenCorePkg
fi

echo "==> Building OpenCorePkg ${TARGET} X64 (OpenCore module only)"
. ./edksetup.sh >/dev/null
build -a X64 -b "${TARGET}" -t XCODE5 \
  -p OpenCorePkg/OpenCorePkg.dsc \
  -m OpenCorePkg/Application/OpenCore/OpenCore.inf \
  -n "${CORES}"

BUILD_DIR="${SRC}/UDK/Build/OpenCorePkg/${TARGET}_XCODE5/X64"
if [ ! -f "${BUILD_DIR}/OpenCore.efi" ]; then
  echo "ERROR: OpenCore.efi not found in ${BUILD_DIR}"
  exit 1
fi

mkdir -p "${OUT}/${TARGET}"
cp "${BUILD_DIR}/OpenCore.efi" "${OUT}/${TARGET}/OpenCore.efi"
cp "${BUILD_DIR}/Bootstrap.efi" "${OUT}/${TARGET}/Bootstrap.efi"

echo "==> Done"
ls -l "${OUT}/${TARGET}"
