#!/bin/bash
#
# Install OCMC's OpenCore.efi onto the boot ESP.
#
# Usage: sudo ./install-ocmc.command [path/to/OpenCore.efi]
#
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-${HERE}/out/DEBUG/OpenCore.efi}"
STAMP="$(date +%Y%m%d-%H%M%S)"

if [ "$(id -u)" != "0" ]; then
  echo "Administrator rights required: sudo $0"
  exit 1
fi

if [ ! -f "${SRC}" ]; then
  echo "Not found: ${SRC}"
  exit 1
fi

# Derive the ESP from the running root volume, disk numbers drift across boots.
STORE="$(diskutil info / | awk '/APFS Physical Store/{print $NF}')"
DISK="$(echo "${STORE}" | sed 's/s[0-9]*$//')"
ESP="${DISK}s1"

echo "Boot store: ${STORE}  ->  ESP: ${ESP}"

if [ ! -d "/Volumes/EFI/EFI/OC" ]; then
  echo "Mounting ${ESP} ..."
  diskutil mount "${ESP}" >/dev/null || { echo "Mount failed, reboot and retry"; exit 1; }
fi

OC="/Volumes/EFI/EFI/OC"
if [ ! -f "${OC}/OpenCore.efi" ]; then
  echo "${OC}/OpenCore.efi missing, wrong mount point?"
  exit 1
fi

echo "Current: $(ls -l "${OC}/OpenCore.efi" | awk '{print $5}') bytes"
echo "New:     $(ls -l "${SRC}" | awk '{print $5}') bytes"

cp "${OC}/OpenCore.efi" "${OC}/OpenCore.efi.bak-${STAMP}"
cp "${SRC}" "${OC}/OpenCore.efi"
sync

A="$(shasum -a 256 "${SRC}" | awk '{print $1}')"
B="$(shasum -a 256 "${OC}/OpenCore.efi" | awk '{print $1}')"

if [ "${A}" != "${B}" ]; then
  echo "Checksum mismatch, roll back with:"
  echo "  cp ${OC}/OpenCore.efi.bak-${STAMP} ${OC}/OpenCore.efi"
  exit 1
fi

echo "Installed, SHA-256 ${A}"
echo "Backup: EFI/OC/OpenCore.efi.bak-${STAMP}"
echo
echo "Put alternative configurations into EFI/OC/Configs/*.plist."
echo "Hold ESC at boot for the menu, SPACE edits boot-args."
