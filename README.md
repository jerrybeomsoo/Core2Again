# Core2Again

**Windows 11 on a Core 2 Duo, with both cores.**

Core2Again is a small UEFI VMX hypervisor for Intel Core 2 processors. It lets an operating system use a CPU that predates some of its required instructions. On a Core 2 Duo T9900, it reports SSE4.2 and POPCNT through `CPUID`, then handles the `#UD` faults when Windows actually tries to use them. SSE4.1 emulation is included for older Conroe and Kentsfield chips.

This is source code for an experimental boot path, not an installer or a Windows image. You need an existing Windows installation and a processor with Intel VMX.

## Where it stands

| Configuration | Result |
| --- | --- |
| T9900, older Clover-based diagnostic build | Windows 11 reached the desktop. Task Manager showed two processors. |
| T9300, Dell Inspiron 1520 and Latitude D830, user report | Windows 11 24H2 Enterprise reached the desktop through OpenCore 1.0.4 Duet. Shutdown worked; restart froze. A candidate fix awaits a hardware test. |
| Current `Core2Again-RELEASE.efi` and OpenDuet USB image | Built and booted to a two-vCPU Windows desktop under SeaBIOS/KVM. They have **not yet been booted on the T9900**. |
| Conroe/Kentsfield SSE4.1 path | Implemented and compared against real instructions locally. Hardware boot remains untested. |

The T9900 result is our hardware test; the Dell results are user reports. Neither confirms the new release binary or USB boot chain on the T9900. The restart change still needs a run on the Dell laptops.

## Get started

The [latest release](https://github.com/jerrybeomsoo/Core2Again/releases) contains the EFI files and compressed USB images. For a legacy BIOS machine, start with `core2again-legacy-blockio.img.gz`. The block I/O build asks the BIOS to access disks through `INT 13h`, which suits older hardware.

1. Back up anything you need from the USB stick. Writing the image erases it.
2. Check the device name with `lsblk`. Use the whole USB device, such as `/dev/sdX`, not a partition like `/dev/sdX1`.
3. Write and flush the image:

   ```bash
   gzip -dc core2again-legacy-blockio.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
   sync
   ```

4. Select the stick in the BIOS boot menu. Core2Again loads the Windows boot manager from an attached drive. The Windows drive is not rewritten by this boot image.

Keep the USB stick inserted when rebooting. For the other image, firmware paths, and troubleshooting, see [Legacy BIOS boot](docs/legacy-boot.md). On a UEFI machine, the driver and loader can be used without the USB image; see the same guide.

Some Dell T9300 users have reported a freeze on Windows restart. A candidate fix and the information needed to check it on hardware are in [Restart troubleshooting](docs/reboot.md).

## What happens at boot

```text
BIOS -> OpenDuet -> Core2AgainLoader.efi -> Core2Again.efi
                                             |
                                      ExitBootServices hook
                                             |
                                      Windows boot manager
```

Core2Again prepares its VMX state while UEFI boot services are available. It switches the boot processor into VMX just before the operating system leaves UEFI, then starts the other processor after firmware has finished `ExitBootServices`. That timing matters on BIOS machines whose UEFI layer still calls real-mode BIOS services during boot.

Inside the guest, the hypervisor adjusts `CPUID` and routes unsupported-instruction faults to its decoder. The decoder fetches the faulting bytes and operands, emulates the instruction, updates flags and registers, and advances the guest instruction pointer. The CPU is still a T9900; this changes the instruction interface Windows sees, not its physical capabilities.

Read [Architecture](docs/architecture.md) for the VMX and multi-core path.

## Build it

The repository is an EDK II package. Keep an EDK II checkout beside it, or set `EDK2_PATH` to another location. Nothing is copied into EDK II.

```bash
git clone https://github.com/tianocore/edk2.git
git clone https://github.com/jerrybeomsoo/Core2Again.git
make -C edk2/BaseTools
cd Core2Again
./scripts/build.sh RELEASE
./scripts/dist.sh
./scripts/verify.sh dist/Core2Again-RELEASE.efi
```

The tested EDK II revision and host packages are in [Building](docs/building.md). `dist.sh` builds the release, debug, single-core and AP debug EFI variants, the loader, and both BIOS USB images. Generated files stay in `Build/` and `dist/`; neither belongs in a source commit.

## Test it

```bash
./tests/run.sh
./tests/ap/run.sh --kvm
./tests/legacy/run.sh --image /path/to/preinstalled-windows.qcow2 --smp 2
```

The first command compares the emulator with real CPU instructions across roughly 41 million cases. The QEMU harnesses check VMX startup, boot, and processor scheduling. QEMU on a modern KVM host executes SSE4.2 itself, so a Windows boot there does **not** exercise the unsupported-instruction path. That path needs a real older CPU; see [Testing](docs/testing.md). A Windows installer boot is not a useful test for this project because the installer can start without the instructions Core2Again provides.

## Files and notes

| Path | Purpose |
| --- | --- |
| `Core2AgainPkg/` | UEFI driver, loader, VMX entry and exit code, AP startup, and instruction emulators |
| `scripts/` | Build, package, image creation, and binary checks |
| `tests/` | Differential and boot harnesses |
| `docs/` | [Boot guide](docs/legacy-boot.md), [build guide](docs/building.md), [architecture](docs/architecture.md), [testing](docs/testing.md), [debug display](docs/diagnostics.md) |

The source is licensed under [BSD-2-Clause-Patent](LICENSE). The BIOS image includes OpenDuet binaries from OpenCorePkg, and the EFI files link EDK II code. Their licenses and attribution are in [Third-party notices](THIRD-PARTY-NOTICES.md).
