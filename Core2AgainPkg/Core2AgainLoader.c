/** @file
  Core2AgainLoader.efi - chainloader for the legacy USB image and QEMU harnesses.

  1. Loads and starts the Core2Again.efi driver from its own volume
     (the driver hooks gBS->ExitBootServices and stays resident).
  2. Connects all controllers so attached disks are enumerated.
  3. Finds a Windows boot volume and chainloads its boot manager with the
     correct device-path context (what BDS would do), so bootmgfw can locate
     its BCD - unlike a bare shell 'launch' which returns EFI_INVALID_PARAMETER.

  This is the boot target in the shipping legacy-BIOS USB image and also lets
  the QEMU harnesses boot deterministically without interaction.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Protocol/BlockIo.h>

STATIC
BOOLEAN
FileExists (
  IN EFI_HANDLE  Handle,
  IN CHAR16      *Path
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_STATUS                       Status;

  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs))) {
    return FALSE;
  }
  if (EFI_ERROR (Sfs->OpenVolume (Sfs, &Root))) {
    return FALSE;
  }
  Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    File->Close (File);
    Root->Close (Root);
    return TRUE;
  }
  Root->Close (Root);
  return FALSE;
}

STATIC
EFI_STATUS
ChainLoad (
  IN EFI_HANDLE  ParentImage,
  IN EFI_HANDLE  VolHandle,
  IN CHAR16      *Path,
  IN BOOLEAN     BootPolicy
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_HANDLE                Img;
  EFI_STATUS                Status;

  Dp = FileDevicePath (VolHandle, Path);
  if (Dp == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Img    = NULL;
  //
  // BootPolicy=TRUE gives boot-manager load semantics (El-Torito / removable
  // media resolution), which is what Windows' bootmgfw needs to locate its BCD;
  // a plain FALSE load returns EFI_INVALID_PARAMETER from bootmgfw.
  //
  Status = gBS->LoadImage (BootPolicy, ParentImage, Dp, NULL, 0, &Img);
  FreePool (Dp);
  if (EFI_ERROR (Status)) {
    Print (L"[CORE2AGAIN] LoadImage %s: %r\n", Path, Status);
    return Status;
  }
  Print (L"[CORE2AGAIN] starting %s ...\n", Path);
  Status = gBS->StartImage (Img, NULL, NULL);
  Print (L"[CORE2AGAIN] %s returned: %r\n", Path, Status);
  return Status;
}

EFI_STATUS
EFIAPI
Core2AgainLoaderMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;
  EFI_HANDLE                 HvHandle;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;

  Print (L"[CORE2AGAIN] start\n");

  //
  // 1. Load + start the hypervisor driver from our own volume.
  //
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    Print (L"[CORE2AGAIN] no LoadedImage: %r\n", Status);
    return Status;
  }
  Dp = FileDevicePath (Li->DeviceHandle, L"\\Core2Again.efi");
  if (Dp == NULL) {
    Print (L"[CORE2AGAIN] no device path for hypervisor\n");
    return EFI_OUT_OF_RESOURCES;
  }
  HvHandle = NULL;
  Status   = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &HvHandle);
  FreePool (Dp);
  if (EFI_ERROR (Status)) {
    Print (L"[CORE2AGAIN] LoadImage HV: %r\n", Status);
    return Status;
  }
  Status = gBS->StartImage (HvHandle, NULL, NULL);
  Print (L"[CORE2AGAIN] HV StartImage: %r\n", Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 2. Connect every controller so attached filesystems are enumerated.
  //
  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    Print (L"[CORE2AGAIN] connecting %d handle(s)\r\n", Count);
    for (Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }
    FreePool (Handles);
  } else {
    Print (L"[CORE2AGAIN] LocateHandleBuffer(AllHandles): %r\r\n", Status);
  }

  //
  // Count block devices separately from filesystems.  If a disk shows up here
  // but not as a filesystem, the partition or FAT driver did not attach; if it
  // shows up in neither, the firmware never enumerated the disk at all.  Those
  // are different problems and the distinction is not otherwise visible.
  //
  {
    EFI_HANDLE  *Blk;
    UINTN       BlkCount = 0;

    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                             NULL, &BlkCount, &Blk)))
    {
      Print (L"[CORE2AGAIN] %d BlockIo handle(s)\r\n", BlkCount);
      for (Index = 0; Index < BlkCount; Index++) {
        EFI_DEVICE_PATH_PROTOCOL  *Bdp;
        CHAR16                    *Txt;

        if (!EFI_ERROR (gBS->HandleProtocol (Blk[Index], &gEfiDevicePathProtocolGuid,
                                             (VOID **)&Bdp)))
        {
          EFI_BLOCK_IO_PROTOCOL  *Bio;
          UINT8                  Sector[512];
          EFI_STATUS             RdStatus = EFI_UNSUPPORTED;
          UINT32                 Sig0 = 0, Sig1 = 0;

          Txt = ConvertDevicePathToText (Bdp, TRUE, FALSE);

          //
          // Read LBA 1 ourselves.  If the firmware can reach the disk, this is
          // "EFI PART"; if it cannot, the partition driver saw the same failure
          // and that is why the disk has no children.
          //
          if (!EFI_ERROR (gBS->HandleProtocol (Blk[Index], &gEfiBlockIoProtocolGuid,
                                               (VOID **)&Bio)))
          {
            RdStatus = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 1,
                                        sizeof (Sector), Sector);
            if (!EFI_ERROR (RdStatus)) {
              Sig0 = *(UINT32 *)&Sector[0];
              Sig1 = *(UINT32 *)&Sector[4];
            }
          }
          Print (L"[CORE2AGAIN]   blk%d: lba1 %r %08x%08x blk=%d  %s\r\n",
                 Index, RdStatus, Sig1, Sig0,
                 (Bio != NULL) ? Bio->Media->BlockSize : 0,
                 (Txt != NULL) ? Txt : L"?");
          if (Txt != NULL) {
            FreePool (Txt);
          }
        }
      }
      FreePool (Blk);
    } else {
      Print (L"[CORE2AGAIN] no BlockIo handles\r\n");
    }
  }

  //
  // 3. Find a Windows boot volume and chainload it.
  //
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"[CORE2AGAIN] no filesystems: %r\n", Status);
    return Status;
  }
  Print (L"[CORE2AGAIN] scanning %d filesystem(s)\n", Count);

  // Pass 1: an installed Windows (bootmgfw on a real ESP).
  for (Index = 0; Index < Count; Index++) {
    if (FileExists (Handles[Index], L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi")) {
      Print (L"[CORE2AGAIN] installed Windows on fs#%d\n", Index);
      ChainLoad (ImageHandle, Handles[Index], L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", TRUE);
    }
  }

  // Pass 2: bootable media (install DVD).  Try \EFI\BOOT\BOOTX64.EFI on every
  // volume EXCEPT our own, which carries this chainloader at that same path -
  // loading it would recurse.  BDS boots the El-Torito stub via a CDROM()
  // handle; that is the volume that loads cleanly, unlike the VenMedia handle.
  //
  // Identify our own volume by handle.  This used to test for the presence of
  // \Core2Again.efi, which is a guess about what our volume contains
  // rather than a fact about which volume we came from: on any boot where that
  // file is absent - a control run, a stripped ESP, a rename - the check failed
  // and this loop launched itself, forever.  Observed recursing 3275 deep
  // before the machine gave up.  Li->DeviceHandle is the actual answer and the
  // firmware handed it to us at entry.
  for (Index = 0; Index < Count; Index++) {
    if (Handles[Index] == Li->DeviceHandle) {
      continue;  // our own volume - skip
    }
    if (FileExists (Handles[Index], L"\\EFI\\BOOT\\BOOTX64.EFI")) {
      Print (L"[CORE2AGAIN] media boot candidate fs#%d\n", Index);
      ChainLoad (ImageHandle, Handles[Index], L"\\EFI\\BOOT\\BOOTX64.EFI", TRUE);
    }
  }
  FreePool (Handles);

  Print (L"[CORE2AGAIN] no bootable volume booted; returning to BDS\n");
  return EFI_SUCCESS;
}
