# Offline Darwin 27 XhciPortLimit regression

Build with the normal OpenCore userspace toolchain and matching AUDK headers:

```sh
make UDK_PATH=/path/to/audk
./UsbPortLimit /path/to/BootKernelExtensions.kc
```

This test reads a private copy of the supplied collection and applies the
actual OpenCore patcher. It never writes a kernel collection or EFI file.
The fixture is supplied locally; Apple binaries are not included here.

For the examined Darwin 27.2.0 x86_64 collection, require:

- both the IOUSBHostFamily and AppleUSBXHCI quirks succeed;
- exactly two bytes change, each from `0F` to `40`;
- a missing/already patched host limit returns `EFI_NOT_FOUND`;
- forcing Darwin 25, 26, or 28 on this same modern-only fixture still runs
  the existing second patch and returns `EFI_NOT_FOUND`.

The forced-version checks test dispatch boundaries, not hardware support
for those systems. A passing offline test does not prove USB enumeration,
unique locationIDs, hotplug, speed, or sleep/wake after boot.
