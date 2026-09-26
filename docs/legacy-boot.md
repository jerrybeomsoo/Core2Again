# Booting from a legacy BIOS

Core2Again is a UEFI driver. A Core 2 machine with only a BIOS needs a UEFI environment before it can load the driver or the UEFI Windows boot manager. The release USB image supplies that environment with OpenDuet.

## Write the USB stick

Download `core2again-legacy-blockio.img.gz` from [Releases](https://github.com/jerrybeomsoo/Core2Again/releases). It is a complete disk image, including partition table and boot code. Writing it erases the selected USB device.

```bash
lsblk -o NAME,SIZE,MODEL,TRAN
gzip -dc core2again-legacy-blockio.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Replace `/dev/sdX` with the whole USB device identified by `lsblk`. Boot from it using the BIOS boot menu. Leave the existing Windows disk connected. The stick provides the boot code and the driver; the Windows disk holds the EFI System Partition and Windows files.

If you build locally, `./scripts/dist.sh` writes the same `.img.gz` files under `dist/`. `./scripts/mkimage.sh` rebuilds only the block I/O image.

## The boot chain

```text
BIOS
  boot0 (MBR code)
    boot1f32 (FAT32 partition boot record)
      boot (OpenDuet UEFI environment)
        EFI/OC/OpenCore.efi (Core2AgainLoader)
          Core2Again.efi (UEFI driver)
          EFI/Microsoft/Boot/bootmgfw.efi (on the Windows disk)
            Windows
```

OpenDuet looks for `EFI/OC/OpenCore.efi`. The name comes from its boot manager; the file in this image is **Core2AgainLoader**, not OpenCore's configuration UI. The loader starts `Core2Again.efi` from the root of the same FAT32 volume, connects disk controllers, then looks for Windows Boot Manager on attached filesystems. If the driver cannot load or start, the loader stops before Windows starts.

The image also places the loader at `EFI/BOOT/BOOTX64.EFI` for a normal UEFI removable-media boot and at `EFI/CLOVER/CLOVERX64.efi` for compatible DUET descendants. The active path in the stock image is `EFI/OC/OpenCore.efi`.

## Which image?

| File | Disk access | Try it when |
| --- | --- | --- |
| `core2again-legacy-blockio.img.gz` | BIOS `INT 13h` through OpenDuet | First choice for older boards |
| `core2again-legacy-native.img.gz` | OpenDuet's native PCI, USB, and storage drivers | The first image cannot see the Windows disk |

The two images contain the same Core2Again EFI files. Both use boot stages from OpenCorePkg 1.0.7. The builder checks their hashes before putting them on the image. The FAT32 volume has a `README.TXT` and copies of the applicable license texts.

## The Windows disk can stay as it is

The BIOS boots the USB stick, not the Windows disk. OpenDuet then supplies UEFI, so Windows starts through `bootmgfw.efi` on its existing GPT EFI System Partition. The image does not write boot code to the Windows disk or modify its ESP.

A BIOS cannot directly start the UEFI Windows boot manager from a GPT installation. That is why the stick must remain available for future boots. If BitLocker is enabled, have the recovery key available before changing the boot chain; a changed boot environment can cause a recovery prompt.

## Switching the driver

Mount the stick's FAT32 partition and copy one of `hv/Core2Again-*.efi` over `Core2Again.efi` at the volume root. The loader always opens that root file. `Core2Again-DEBUG.efi` paints diagnostic numbers on the screen; see [Reading the debug display](diagnostics.md). The normal release driver has those diagnostics compiled out.

## If it stops

| Screen or symptom | What to check |
| --- | --- |
| No OpenDuet screen | Check BIOS USB boot order and that the image was written to the whole device. |
| `LoadImage HV` or `HV StartImage` error | Check that `Core2Again.efi` is at the FAT32 root and has not been corrupted. The loader will not start Windows after this error. |
| No Windows boot volume found | Confirm the Windows disk is attached and has `EFI/Microsoft/Boot/bootmgfw.efi`. Try the native image if block I/O sees no disk. |
| `BOOT MISMATCH!` or `BOOT FAIL!` after a loader return | OpenDuet could not continue to an OS. These messages are expected in a smoke test with no Windows disk; they do not diagnose a running Windows boot. |
| White checkpoint or cyan rows in a DEBUG build | Read [the row map](diagnostics.md) and photograph the whole screen. |

The **current** USB images have reached a two-vCPU Windows desktop under SeaBIOS/KVM. They have not yet been tried on the physical T9900. The older Clover boot path with a diagnostic driver did reach that machine's Windows desktop on two cores. Treat the image as experimental until the new chain is checked on hardware.

## Build a custom image

```bash
./scripts/mkimage.sh --variant blockio
./scripts/mkimage.sh --variant native --hv dist/Core2Again-DEBUG.efi \
  --out dist/core2again-debug-native.img
```

`mkimage.sh --help` lists the available switches. The script builds FAT32 inside a regular file with `mkfs.vfat` and `mtools`, then writes an MBR around it. Creating the image needs no root access. Only writing the finished image to a USB device does.
