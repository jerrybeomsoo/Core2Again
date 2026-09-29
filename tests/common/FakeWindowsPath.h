/** @file
  Give a test EFI application the path of the Windows boot manager.

  These applications live at EFI/BOOT/BOOTX64.EFI under QEMU. The production
  driver must skip that generic path, so VMX tests set their LoadedImage path
  to the bootmgfw.efi path observed on a real Windows boot. This is test-only.
**/
#ifndef CORE2AGAIN_FAKE_WINDOWS_PATH_H_
#define CORE2AGAIN_FAKE_WINDOWS_PATH_H_

#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>

STATIC
VOID
TestUseWindowsBootPath (
  IN EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage
  )
{
  STATIC union {
    UINTN Align;
    UINT8 Data[128];
  } mPath;
  STATIC CONST CHAR8            mName[] = "\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
  EFI_DEVICE_PATH_PROTOCOL      *File;
  EFI_DEVICE_PATH_PROTOCOL      *End;
  CHAR16                        *Wide;
  UINTN                         FileBytes;
  UINTN                         Index;

  FileBytes = sizeof (*File) + sizeof (mName) * sizeof (CHAR16);
  if (FileBytes + sizeof (*End) > sizeof (mPath.Data)) {
    return;
  }
  File            = (EFI_DEVICE_PATH_PROTOCOL *)(VOID *)mPath.Data;
  File->Type      = MEDIA_DEVICE_PATH;
  File->SubType   = MEDIA_FILEPATH_DP;
  File->Length[0] = (UINT8)FileBytes;
  File->Length[1] = (UINT8)(FileBytes >> 8);
  Wide = (CHAR16 *)(VOID *)(mPath.Data + sizeof (*File));
  for (Index = 0; Index < sizeof (mName); Index++) {
    Wide[Index] = (CHAR16)(UINT8)mName[Index];
  }
  End            = (EFI_DEVICE_PATH_PROTOCOL *)(VOID *)(mPath.Data + FileBytes);
  End->Type      = END_DEVICE_PATH_TYPE;
  End->SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0] = sizeof (*End);
  End->Length[1] = 0;
  LoadedImage->FilePath = File;
}

#endif
