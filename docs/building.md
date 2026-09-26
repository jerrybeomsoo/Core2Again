# Building Core2Again

Core2Again is an X64 EDK II package. The tested host is Fedora Linux with GCC, NASM, Python 3, and an EDK II checkout at revision `48c268e7f4ae`. Other toolchains are described in the DSC file but have not been checked for this release.

## Set up EDK II

Install GCC, GNU make, G++, NASM, Python 3, and the development package for `libuuid`. To create USB images, also install `dosfstools`, `mtools`, `util-linux` (for `sfdisk`), `curl`, and `gzip`. Python's `zipfile` module extracts the downloaded archive. `objdump` from binutils is used by the binary check. QEMU and KVM are only needed for the VM boot tests.

```bash
git clone https://github.com/tianocore/edk2.git
git -C edk2 checkout 48c268e7f4ae
make -C edk2/BaseTools
git clone https://github.com/jerrybeomsoo/Core2Again.git
cd Core2Again
```

Put the two repositories side by side, as above. If EDK II is elsewhere, prefix a build command with `EDK2_PATH=/absolute/path/to/edk2`. The build script sets `PACKAGES_PATH` so Core2Again stays outside the EDK II checkout.

## Build the EFI files

```bash
./scripts/build.sh                  # DEBUG and RELEASE
./scripts/build.sh RELEASE          # RELEASE only
./scripts/verify.sh                 # checks the built RELEASE EFI by default
./scripts/dist.sh                   # all EFI variants and both USB images
```

The EFI modules are first written to `Build/Core2AgainPkg/<target>_GCC/X64/`. `dist.sh` collects the files intended for use:

| File | Use |
| --- | --- |
| `Core2Again-RELEASE.efi` | Normal driver, diagnostics compiled out |
| `Core2Again-DEBUG.efi` | Driver with screen diagnostics |
| `Core2Again-singlecore.efi` | Leaves other processors alone; Windows must be limited to one processor |
| `Core2Again-apdebug.efi` | Stops at the AP SIPI exit for bring-up debugging |
| `Core2AgainLoader.efi` | Loads the driver and starts Windows Boot Manager |
| `core2again-legacy-blockio.img[.gz]` | Legacy BIOS boot image using BIOS disk access |
| `core2again-legacy-native.img[.gz]` | Legacy BIOS boot image using OpenDuet's native disk drivers |

`dist.sh` downloads a pinned OpenCorePkg 1.0.7 release on its first USB image build. It checks the archive and the extracted boot stages against fixed SHA-256 values. The image builder needs no root privileges or loop devices.

## Build switches and binary checks

The DSC supports `HV_AP_STARTUP`, `HV_AP_DEBUG`, `HV_FORCE_NO_EPT`, and `HV_BUILD_TESTS`. The target T9900 has no EPT, so `HV_FORCE_NO_EPT=TRUE` can make a development host take the same memory mapping path. The DEBUG target enables framebuffer diagnostics; RELEASE removes them.

The host code is compiled with `-mno-sse4 -mno-popcnt`. A host instruction that the physical CPU lacks would fault outside the guest and cannot be emulated there. Run `scripts/verify.sh` on every EFI file you plan to ship. It checks the disassembly for unsupported host instructions, the VM-exit calling convention, the AP trampoline, and whether diagnostics match the target type.

```bash
./scripts/verify.sh dist/Core2Again-RELEASE.efi
./scripts/verify.sh dist/Core2Again-DEBUG.efi
```

The single-core variant deliberately has no AP trampoline. The verifier reports that as such. For test commands and their limits, continue with [Testing](testing.md).
