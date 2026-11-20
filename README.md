# SharedCacheWire

A [Lilu](https://github.com/acidanthera/Lilu) plugin that stops random `SIGBUS` crashes caused by kexts that patch
the dyld shared cache in memory (WhateverGreen, RestrictEvents, FeatureUnlock, RealtekUVCFix,
[CASyncFix](https://github.com/trungkiengrxer/CASyncFix)...).

Developed and tested on macOS 26.6 Tahoe (25G72), Intel. The plugin allows Big Sur through Tahoe; it does not depend
on fixed addresses.

## The problem

Patchers hook `_cs_validate_page` and change code pages of the shared cache as they are paged in. The shared cache is
an APFS-compressed file, read in compression chunks. Once a patched page is resident, macOS cannot page in an evicted
neighbour from the same chunk again: APFS reports `compressed data hash mismatch`, and the process that faulted on that
page dies with `SIGBUS` (`FS pagein error: 94 Bad message`). The crashing app looks random and the crash report points
at a system framework.

## The fix

SharedCacheWire hooks `_cs_validate_page` above every other patcher. When a page changed while the hooks below it ran,
it wires that page's whole compression chunk until shutdown, so none of its pages is ever paged in again. The wires are
released at shutdown stage 1, before volumes unmount.

- Assumes 64 KB compression chunks. At most 64 chunks (4 MB) are wired; the log says `chunk table is full` if a system
  ever needs more.
- If wiring is unavailable, the plugin does not hook anything.

## Install (OpenCore)

1. Download `SharedCacheWire-<version>-RELEASE.zip` from [Releases](../../releases) and copy `SharedCacheWire.kext` to
   `EFI/OC/Kexts`.
2. Add it to `config.plist > Kernel > Add` **last**, after Lilu and after every kext that patches the shared cache.
   A patcher loaded later sits above this hook and its patches go unnoticed.
3. Reboot.

Boot-args: `-scwoff` disables the plugin.

## Verify

```bash
kextstat | grep -i SharedCacheWire
```

```bash
log show --last boot --predicate 'eventMessage CONTAINS "wired chunk"' --style compact
```

## Build

Put a DEBUG `Lilu.kext` and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) in the repository root, then:

```bash
xcodebuild -project SharedCacheWire.xcodeproj -configuration Release -arch x86_64
```

Output: `build/Release/SharedCacheWire-<version>-RELEASE.zip`.

## Tests

```bash
clang++ -std=c++17 -I Tests/stubs -DPRODUCT_NAME=SharedCacheWire -DMODULE_VERSION=0.0.0 Tests/wire_test.cpp -o /tmp/wire_test && /tmp/wire_test
```

## CI and releases

`.github/workflows/build.yml` runs the test and builds on every push and pull request. Pushing a tag `vX.Y.Z` that
matches `MODULE_VERSION` in the Xcode project publishes a GitHub Release with the zip (kext and dSYM).

## Disclaimer

Provided as is, without warranty. Keep a bootable fallback EFI.
