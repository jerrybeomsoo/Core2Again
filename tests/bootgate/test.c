#include "Hypervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union {
  UINTN Align;
  UINT8 Bytes[512];
} TEST_PATH;

static unsigned
MakePath (TEST_PATH *Out, const char *Text)
{
  EFI_DEVICE_PATH_PROTOCOL *File, *End;
  CHAR16 *Name;
  size_t Chars, Index;
  unsigned Length;

  Chars  = strlen (Text);
  Length = (unsigned)(sizeof (*File) + (Chars + 1) * sizeof (CHAR16));
  if (Length + sizeof (*End) > sizeof (Out->Bytes)) {
    abort ();
  }
  memset (Out, 0, sizeof (*Out));
  File = (EFI_DEVICE_PATH_PROTOCOL *)Out->Bytes;
  File->Type      = MEDIA_DEVICE_PATH;
  File->SubType   = MEDIA_FILEPATH_DP;
  File->Length[0] = (UINT8)Length;
  File->Length[1] = (UINT8)(Length >> 8);
  Name = (CHAR16 *)(Out->Bytes + sizeof (*File));
  for (Index = 0; Index < Chars; Index++) {
    Name[Index] = (CHAR16)(UINT8)Text[Index];
  }
  End = (EFI_DEVICE_PATH_PROTOCOL *)(Out->Bytes + Length);
  End->Type      = END_DEVICE_PATH_TYPE;
  End->SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0] = sizeof (*End);
  return Length;
}

static void
Expect (const char *Label, const TEST_PATH *Path, BOOLEAN Wanted)
{
  BOOLEAN Got = HvIsWindowsBootPath ((const EFI_DEVICE_PATH_PROTOCOL *)Path->Bytes);
  if (Got != Wanted) {
    fprintf (stderr, "FAIL %s: got %u, wanted %u\n", Label, Got, Wanted);
    exit (1);
  }
}

int
main (void)
{
  static const struct {
    const char *Name;
    const char *Path;
    BOOLEAN Windows;
  } Cases[] = {
    {"bootmgfw", "\\EFI\\Microsoft\\Boot\\bootmgfw.efi", TRUE},
    {"mixed case", "/efi/MICROSOFT/BOOT/BOOTMGFW.EFI", TRUE},
    {"winload", "\\Windows\\System32\\winload.efi", TRUE},
    {"winresume", "\\Windows\\System32\\winresume.efi", TRUE},
    {"split node filename", "BOOTMGFW.EFI", TRUE},
    {"Linux GRUB", "\\EFI\\ubuntu\\grubx64.efi", FALSE},
    {"Linux fallback", "\\EFI\\BOOT\\BOOTX64.EFI", FALSE},
    {"macOS", "\\System\\Library\\CoreServices\\boot.efi", FALSE},
    {"unrelated bootmgfw name", "\\EFI\\Other\\bootmgfw.efi", FALSE},
    {"backup suffix", "\\EFI\\Microsoft\\Boot\\bootmgfw.efi.bak", FALSE},
    {"empty", "", FALSE}
  };
  TEST_PATH Path;
  TEST_PATH Other;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  unsigned Length;
  size_t Index;

  if (HvIsWindowsBootPath (NULL)) {
    fprintf (stderr, "FAIL null path accepted\n");
    return 1;
  }
  for (Index = 0; Index < sizeof (Cases) / sizeof (Cases[0]); Index++) {
    MakePath (&Path, Cases[Index].Path);
    Expect (Cases[Index].Name, &Path, Cases[Index].Windows);
  }

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  Node = (EFI_DEVICE_PATH_PROTOCOL *)Path.Bytes;
  Node->Length[0] = 2;
  Node->Length[1] = 0;
  Expect ("short node", &Path, FALSE);

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  Node = (EFI_DEVICE_PATH_PROTOCOL *)Path.Bytes;
  Node->Length[0] = 0xff;
  Node->Length[1] = 0xff;
  Expect ("oversized node", &Path, FALSE);

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  Path.Bytes[Length - 2] = 'X';
  Expect ("missing string terminator", &Path, FALSE);

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  Node = (EFI_DEVICE_PATH_PROTOCOL *)(Path.Bytes + Length);
  Node->Length[0] = 2;
  Expect ("bad end node after match", &Path, FALSE);

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  {
    unsigned OtherLength = MakePath (&Other, "\\EFI\\ubuntu\\grubx64.efi");
    memcpy (Path.Bytes + Length, Other.Bytes, OtherLength + sizeof (*Node));
  }
  Expect ("later Linux node", &Path, FALSE);

  Length = MakePath (&Path, "\\EFI\\Microsoft\\Boot");
  {
    unsigned OtherLength = MakePath (&Other, "bootmgfw.efi");
    memcpy (Path.Bytes + Length, Other.Bytes, OtherLength + sizeof (*Node));
  }
  Expect ("split Windows path", &Path, TRUE);

  puts ("PASS: Windows loaders accepted; Linux, macOS, fallback and malformed paths rejected");
  return 0;
}
