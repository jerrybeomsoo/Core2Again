/** @file
  Recognize the Windows EFI loaders before Core2Again enters VMX.

  This runs between the loader's GetMemoryMap and ExitBootServices calls.
  Keep it bounded and allocation-free: allocating here invalidates MapKey.
**/

#include "Hypervisor.h"

#define HV_BOOT_PATH_LIMIT  4096

STATIC
CHAR16
FoldPathChar (
  IN CHAR16  Ch
  )
{
  if ((Ch >= 'A') && (Ch <= 'Z')) {
    return (CHAR16)(Ch + ('a' - 'A'));
  }
  if (Ch == '/') {
    return '\\';
  }
  return Ch;
}

STATIC
BOOLEAN
PathEndsWith (
  IN CONST CHAR16  *Path,
  IN UINTN         PathChars,
  IN CONST CHAR8   *Suffix
  )
{
  UINTN  SuffixChars;
  UINTN  Index;

  for (SuffixChars = 0; Suffix[SuffixChars] != 0; SuffixChars++) {
  }
  if (PathChars < SuffixChars) {
    return FALSE;
  }

  for (Index = 0; Index < SuffixChars; Index++) {
    if (FoldPathChar (Path[PathChars - SuffixChars + Index]) !=
        FoldPathChar ((CHAR16)(UINT8)Suffix[Index]))
    {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC
BOOLEAN
IsWindowsFilePath (
  IN CONST CHAR16  *Path,
  IN UINTN         MaxChars
  )
{
  UINTN  Chars;

  // A FilePath node must contain a NUL-terminated UTF-16 path.
  for (Chars = 0; Chars < MaxChars; Chars++) {
    if (Path[Chars] == 0) {
      break;
    }
  }
  if (Chars == MaxChars) {
    return FALSE;
  }

  // Match Microsoft's actual boot paths, not a generic BOOTX64.EFI. The latter
  // is also used by Linux and other operating systems. A bare filename allows
  // firmware that splits a path into separate FilePath nodes.
  if (PathEndsWith (Path, Chars, "\\efi\\microsoft\\boot\\bootmgfw.efi") ||
      PathEndsWith (Path, Chars, "\\windows\\system32\\winload.efi") ||
      PathEndsWith (Path, Chars, "\\windows\\system32\\winresume.efi"))
  {
    return TRUE;
  }
  return (BOOLEAN)(((Chars == sizeof ("bootmgfw.efi") - 1) &&
                    PathEndsWith (Path, Chars, "bootmgfw.efi")) ||
                   ((Chars == sizeof ("winload.efi") - 1) &&
                    PathEndsWith (Path, Chars, "winload.efi")) ||
                   ((Chars == sizeof ("winresume.efi") - 1) &&
                    PathEndsWith (Path, Chars, "winresume.efi")));
}

BOOLEAN
HvIsWindowsBootPath (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINTN                           Offset;
  UINTN                           NodeBytes;
  UINTN                           PathBytes;
  BOOLEAN                         Windows;

  if (DevicePath == NULL) {
    return FALSE;
  }

  Node    = DevicePath;
  Windows = FALSE;
  for (Offset = 0; Offset < HV_BOOT_PATH_LIMIT; Offset += NodeBytes) {
    if (HV_BOOT_PATH_LIMIT - Offset < sizeof (*Node)) {
      return FALSE;
    }
    NodeBytes = (UINTN)Node->Length[0] | ((UINTN)Node->Length[1] << 8);
    if ((NodeBytes < sizeof (*Node)) ||
        (NodeBytes > HV_BOOT_PATH_LIMIT - Offset))
    {
      return FALSE;
    }

    if (Node->Type == END_DEVICE_PATH_TYPE) {
      return (BOOLEAN)(Node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE &&
                       NodeBytes == sizeof (*Node) && Windows);
    }

    if ((Node->Type == MEDIA_DEVICE_PATH) &&
        (Node->SubType == MEDIA_FILEPATH_DP))
    {
      PathBytes = NodeBytes - sizeof (*Node);
      if ((PathBytes < sizeof (CHAR16)) || ((PathBytes & 1) != 0)) {
        return FALSE;
      }
      Windows = IsWindowsFilePath ((CONST CHAR16 *)(CONST VOID *)(Node + 1),
                                   PathBytes / sizeof (CHAR16));
    }

    Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)(CONST VOID *)((CONST UINT8 *)Node + NodeBytes);
  }

  return FALSE;
}
