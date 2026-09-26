# Third-party notices

Core2Again's own source is covered by [LICENSE](LICENSE). The build and BIOS image also use these projects:

| Component | Use | License |
| --- | --- | --- |
| [EDK II](https://github.com/tianocore/edk2) | Headers, libraries, build tools, and code linked into the EFI files | [BSD-2-Clause-Patent](licenses/edk2.txt) |
| [OpenCorePkg 1.0.7](https://github.com/acidanthera/OpenCorePkg/tree/1.0.7) | OpenDuet legacy BIOS boot stages bundled in the USB images | [BSD-3-Clause](licenses/opencore.txt) |

The build scripts download OpenCore's 1.0.7 release archive and check its SHA-256 before extracting the boot stages. EDK II is a separate checkout supplied by the builder. The USB image carries copies of these license texts on its FAT32 volume.
