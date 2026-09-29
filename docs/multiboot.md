# Multiboot with OpenCore

Core2Again v0.1.0 and v0.1.1-rc1 entered VMX whenever any operating system called `ExitBootServices`. That caused macOS and Linux Mint to stop booting when OpenCore loaded the driver globally. A user worked around it by keeping Core2Again in OpenCore's Tools menu and launching it manually before Windows. That is still a useful fallback.

The new driver checks the EFI image that calls `ExitBootServices`. It activates for the normal Windows boot manager (`bootmgfw.efi`) and the Windows loader or resume loader (`winload.efi` or `winresume.efi`). Other images, including the usual macOS and Linux loaders, leave boot services without Core2Again entering VMX. An unknown or malformed image path is treated as non-Windows.

This checks the **boot loader**, not the installed Windows version. UEFI does not give the driver a dependable "Windows 11 24H2 or later" flag at this point, and a Windows file version check during `ExitBootServices` could reject an updated or customized loader. The driver will also activate for older Windows versions that use these paths. For exact per-entry control, continue using the Tools workflow.

## Try it on a multiboot machine

1. Keep a copy of your working Core2Again EFI file and OpenCore configuration.
2. Replace the configured Core2Again driver with the new `Core2Again-RELEASE.efi`, using the filename your OpenCore configuration expects. Load one copy only; do not also launch it from Tools on the same boot.
3. Boot Linux Mint and macOS normally, then boot Windows 11. In Windows, confirm the processor count and try Restart.

If Windows stops booting, the boot loader may be using an unusual path that this build does not recognize. Restore the working EFI file and use the Tools method. Please send the exact OpenCore boot entry path and a photo of the last screen. If macOS or Linux still stops, restore the old setup and report which OS and version, where it stops, and whether the same entry boots when Core2Again is removed from OpenCore's Drivers list.

The [boot gate test](../tests/README.md) exercises both branches under KVM. It does not replace a test on the affected OpenCore machine.
