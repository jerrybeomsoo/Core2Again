# Restart freezes on some legacy BIOS machines

Two Dell laptops with a Core 2 Duo T9300 have booted Windows 11 24H2 through OpenCore 1.0.4 Duet and Core2Again, but stopped on a black screen whenever Windows restarted. Shutdown worked. The exact cause on those laptops still needs a hardware check.

We found a reboot bug in Core2Again that fits this symptom. When the boot processor received an INIT signal, the driver put it in *wait for SIPI*. That state is for application processors during startup. The boot processor never gets the startup IPI that would release it, so the machine stays frozen. The [ICH8 chipset documentation](https://www.intel.vn/content/dam/www/public/us/en/documents/datasheets/io-controller-hub-8-datasheet.pdf) says a keyboard-controller reset can generate INIT, and [OpenDuet's reset code](https://github.com/acidanthera/OpenCorePkg/blob/1.0.4/Legacy/BootPlatform/AcpiResetDxe/Reset.c) uses that command when it has no ACPI reset register.

The experimental fix asks the chipset for a full reset when INIT reaches the boot processor. INIT sent to an application processor still follows the usual INIT-SIPI path. A two-processor QEMU test deliberately triggers boot-processor INIT: the previous driver freezes, and the patched driver resets the VM. A separate QEMU Windows restart succeeded even with the previous driver, so that test cannot confirm the Dell fix.

## Try the patched driver

Keep a copy of the working EFI file. Replace the Core2Again driver loaded by your OpenCore setup with `Core2Again-RELEASE.efi` from the experimental release, using the filename your configuration expects. You do not need to replace OpenCore or reinstall Windows. Then boot Windows and use **Restart** from the Start menu. If it works, try one more restart after signing in.

If the machine still stops, please report:

- Which laptop and BIOS version you tested, and which Core2Again build was loaded.
- Whether the Dell logo returns, the power light changes, or the fans cycle. Note roughly how long you waited at the black screen.
- A short video of the restart, if possible. If you can use `Core2Again-DEBUG.efi`, also photograph any white checkpoint number that remains on screen. Checkpoint **31** means the boot processor received INIT and the chipset did not complete the requested reset. A blank screen is useful to report too.
- Whether an EFI Shell `reset -c` command also hangs before starting Windows, if your OpenCore setup offers a shell. This tests the firmware reset path without the Windows shutdown sequence.

If the patched driver does not help, restore the previous EFI file. The next investigation is the reset register advertised by the machine's ACPI FADT and the last reset method that Windows reaches.
