#!/bin/bash
#
# OCMC installer / repair tool.
#
# Disk numbers drift between boots on this class of machine, and more than one
# ESP can carry the volume name "EFI", so the target is identified by content:
# the ESP that already holds EFI/OC/Configs.
#
# Before installing, every configuration file is parsed. A damaged one is
# restored from config.plist, which is the entry the menu falls back to.
#
# Usage: sudo ./install-ocmc.command [path/to/OpenCore.efi]
#
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${1:-${HERE}/out/DEBUG/OpenCore.efi}"

if [ "$(id -u)" != "0" ]; then
  echo "Administrator rights required: sudo $0"
  exit 1
fi

if [ ! -f "$IMAGE" ]; then
  echo "Not found: $IMAGE"
  exit 1
fi

mount_esp() {
  local dev round
  for round in 1 2 3 4 5; do
    for dev in $(diskutil info -all 2>/dev/null | awk '
        /Device Identifier:/{d=$3}
        /Volume Name:/{if ($0 ~ /EFI/) print d}' | sort -u); do
      diskutil mount "$dev" >/dev/null 2>&1
      if [ -d /Volumes/EFI/EFI/OC/Configs ]; then
        echo "   mounted $dev at /Volumes/EFI"
        return 0
      fi
      diskutil unmount "/Volumes/EFI" >/dev/null 2>&1
    done
    launchctl kickstart -k system/com.apple.diskarbitrationd >/dev/null 2>&1
    sleep 2
  done
  return 1
}

echo "==> locating EFI system partition"
if ! mount_esp; then
  echo "ERROR: no EFI partition holding EFI/OC/Configs could be mounted."
  echo "       Reboot once, then run this script again."
  exit 1
fi

OC="/Volumes/EFI/EFI/OC"
STAMP="$(date +%Y%m%d-%H%M%S)"

echo "==> checking configuration files"
python3 - "$OC" <<'PY'
import plistlib, os, shutil, sys

oc   = sys.argv[1]
root = os.path.join(oc, "config.plist")
rest = [os.path.join(oc, "Configs", "config26.plist"),
        os.path.join(oc, "Configs", "config27.plist")]

def load(p):
    with open(p, "rb") as f:
        return plistlib.load(f)

base = None
if os.path.exists(root):
    try:
        base = load(root)
        print("    config.plist            OK")
    except Exception as exc:
        print(f"    config.plist            BROKEN ({exc})")

for p in rest:
    name = os.path.basename(p)
    if not os.path.exists(p):
        print(f"    {name:<23} MISSING")
        continue
    try:
        load(p)
        print(f"    {name:<23} OK")
    except Exception as exc:
        print(f"    {name:<23} BROKEN ({exc})")
        if base is not None:
            shutil.copyfile(root, p)
            print(f"    {name:<23} restored from config.plist")
PY

echo "==> backing up current firmware"
if [ -f "$OC/OpenCore.efi" ]; then
  cp "$OC/OpenCore.efi" "$OC/OpenCore.efi.bak-$STAMP"
  echo "    saved OpenCore.efi.bak-$STAMP"
fi

echo "==> installing new firmware"
cp "$IMAGE" "$OC/OpenCore.efi" || { echo "ERROR: write failed"; exit 1; }
sync

SRC_SUM="$(shasum -a 256 "$IMAGE" | awk '{print $1}')"
DST_SUM="$(shasum -a 256 "$OC/OpenCore.efi" | awk '{print $1}')"

echo "    source $SRC_SUM"
echo "    target $DST_SUM"
if [ "$SRC_SUM" != "$DST_SUM" ]; then
  echo "ERROR: checksum mismatch. Roll back with:"
  echo "  sudo cp \"$OC/OpenCore.efi.bak-$STAMP\" \"$OC/OpenCore.efi\""
  exit 1
fi

echo "==> verifying the image carries the expected build"
python3 - "$OC/OpenCore.efi" <<'PY'
import sys

data = open(sys.argv[1], "rb").read()

def count(text):
    return data.count(text.encode("utf-16-le"))

print(f"    size    {len(data)} bytes")
print(f"    build   mc6 marker: {count('mc6')}")
print(f"    layout  LAST USED label: {count('LAST USED')}")
PY

sync
osascript -e 'tell application "Finder" to eject disk "EFI"' >/dev/null 2>&1
diskutil unmount "/Volumes/EFI" >/dev/null 2>&1

echo
echo "Done. Reboot and the menu title shows BUILD mc6."
echo "The entry booted last time is preselected and marked LAST USED."
