/** @file
  HvLifecycle.c - drive the real hypervisor through ExitBootServices, with no OS.

  WHY THIS EXISTS
  ---------------
  Everything interesting in this hypervisor happens inside the ExitBootServices
  hook: the APs are woken and virtualized, then the BSP virtualizes itself, and
  from that instant every exit runs through the host stack.  Reaching that point
  normally means booting a whole operating system, which needs an OS image and
  several minutes, and which reports failures as a reset.

  This application is the smallest thing that gets there.  It loads the shipping
  driver, calls ExitBootServices itself, and then executes CPUID forever.  Each
  CPUID is a VM exit, so the driver's own heartbeat keeps repainting - which
  means the machine's state is legible from a screenshot, using exactly the
  instrumentation that exists for the real hardware:

    checkpoint 30  the heartbeat is running, so we are a guest and exits work
    row 8          packed AP status - the multi-core path, in the real driver

  Deliberately NOT a test of the SSE4 emulators.  Under KVM the physical CPU has
  SSE4.2 whatever CPUID is masked to, so no #UD is ever raised and the decoder
  never runs; that path is covered by tests/ (41 M differential cases) and only
  really exists on a CPU where CPUID and the ISA agree.

  Run it with tests/hv/run.sh.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#ifndef HV_TEST_NON_WINDOWS
#include "../common/FakeWindowsPath.h"
#endif

//
// The framebuffer, resolved BEFORE ExitBootServices and used after it.  This is
// our own liveness signal, independent of the hypervisor's heartbeat: if the
// hypervisor's numbers stop moving we still need to know whether THIS processor
// is running, and the heartbeat cannot answer that - it is painted from inside
// the hypervisor, not from here.
//
STATIC volatile UINT32  *mFb    = NULL;
STATIC UINTN             mFbPps = 0;
STATIC UINTN             mFbH   = 0;

/**
  Paint a solid block at a fixed spot, bottom-left, clear of the marker display.
**/
STATIC
VOID
PaintBlock (
  IN UINT32  Color
  )
{
  UINTN  X, Y, Y0;

  if (mFb == NULL) {
    return;
  }
  Y0 = (mFbH > 80) ? (mFbH - 80) : 0;
  for (Y = Y0; Y < Y0 + 60; Y++) {
    for (X = 300; X < 500; X++) {
      mFb[Y * mFbPps + X] = Color;
    }
  }
}

EFI_STATUS
EFIAPI
HvLifecycleMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;
  EFI_HANDLE                 HvHandle;
  EFI_MEMORY_DESCRIPTOR      *Map;
  UINTN                      MapSize, MapKey, DescSize;
  UINT32                     DescVer;
  UINT32                     Eax, Ebx, Ecx, Edx;
  UINTN                      Attempt;

  Print (L"[LIFE] loading hypervisor driver\r\n");

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    Print (L"[LIFE] no LoadedImage: %r\r\n", Status);
    return Status;
  }

  Dp = FileDevicePath (Li->DeviceHandle, L"\\Core2Again.efi");
  if (Dp == NULL) {
    Print (L"[LIFE] FileDevicePath failed\r\n");
    return EFI_OUT_OF_RESOURCES;
  }
  HvHandle = NULL;
  Status   = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &HvHandle);
  FreePool (Dp);
  if (EFI_ERROR (Status)) {
    Print (L"[LIFE] LoadImage: %r\r\n", Status);
    return Status;
  }
  Status = gBS->StartImage (HvHandle, NULL, NULL);
  Print (L"[LIFE] driver StartImage: %r\r\n", Status);

  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  Print (L"[LIFE] pre-EBS  CPUID.1:ECX = %08x  (SSE4.2 bit20 = %d)\r\n",
         Ecx, (Ecx >> 20) & 1);
  //
  // Resolve the framebuffer while boot services still exist.
  //
  {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
    EFI_GUID                      GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    if (!EFI_ERROR (gBS->LocateProtocol (&GopGuid, NULL, (VOID **)&Gop)) &&
        (Gop->Mode != NULL) && (Gop->Mode->FrameBufferBase != 0))
    {
      mFb    = (volatile UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
      mFbPps = Gop->Mode->Info->PixelsPerScanLine;
      mFbH   = Gop->Mode->Info->VerticalResolution;
      Print (L"[LIFE] framebuffer %lx  %dx%d\r\n",
             Gop->Mode->FrameBufferBase,
             Gop->Mode->Info->HorizontalResolution, mFbH);
    }
  }

  Print (L"[LIFE] calling ExitBootServices - the hook virtualizes here\r\n");

#ifndef HV_TEST_NON_WINDOWS
  TestUseWindowsBootPath (Li);
#endif

  //
  // The map key must be current at the moment of the call.  The hook
  // virtualizes the BSP before chaining to firmware ExitBootServices and wakes
  // the APs only after that call succeeds.  Its boot-time resources were
  // allocated at driver load, so it need not invalidate the map key here.
  //
  Map     = NULL;
  MapSize = 0;
  for (Attempt = 0; Attempt < 8; Attempt++) {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (Map != NULL) {
        FreePool (Map);
      }
      MapSize += 4 * DescSize;
      Map      = AllocatePool (MapSize);
      if (Map == NULL) {
        Print (L"[LIFE] out of memory for the map\r\n");
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }
    if (EFI_ERROR (Status)) {
      Print (L"[LIFE] GetMemoryMap: %r\r\n", Status);
      return Status;
    }
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      break;
    }
    // EFI_INVALID_PARAMETER means the map moved under us; rebuild and retry.
  }

  if (EFI_ERROR (Status)) {
    Print (L"[LIFE] ExitBootServices failed: %r\r\n", Status);
    return Status;
  }

  //
  // No boot services from here on, and no interrupt handlers either - the
  // firmware's are gone and we installed none.
  //
  DisableInterrupts ();

#ifdef HV_TEST_BOOT_GATE
  // QEMU masks SSE4.2 in this mode. Red means Core2Again restored its CPUID
  // bit for a Windows loader; blue means a different loader stayed native.
  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  PaintBlock (((Ecx >> 20) & 1) ? 0x00FF0000 : 0x000000FF);
  for (;;) {
    CpuPause ();
  }
#endif

#ifdef HV_TEST_BSP_INIT
  //
  // Reboot regression: a physical INIT aimed at the boot processor becomes a
  // VM exit.  A guest BSP must never be left waiting for a SIPI, because only
  // application processors participate in the INIT-SIPI startup sequence.
  // Paint red so a frozen screenshot identifies the old behavior.
  //
  {
    UINT64           ApicBase;
    volatile UINT32  *Icr;
    UINT32           ApicId;

    ApicBase = AsmReadMsr64 (0x1B);
    Icr = (volatile UINT32 *)(UINTN)((ApicBase & 0x000FFFFFFFFFF000ULL) + 0x300);
    AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
    ApicId = Ebx >> 24;
    PaintBlock (0x00FF0000);
    while ((Icr[0] & 0x1000) != 0) {
      CpuPause ();
    }
    Icr[4] = ApicId << 24;
    Icr[0] = 0x00004500;  // INIT, level assert, physical BSP destination
  }
#endif

  //
  // Deliberately NO serial output from here on.  An earlier version wrote to
  // COM1 directly, on the reasoning that the UART is just I/O ports and survives
  // ExitBootServices.  It does - but port I/O in a GUEST traps, and under nested
  // VMX an exit costs milliseconds, so the UART-ready spin ran for minutes per
  // character and was indistinguishable from a hang.  The screen is the
  // instrument.
  //
  // Spin on CPUID.  Every one of these is an unconditional VM exit, so a
  // fast-climbing exit count in heartbeat row 0 is proof that this processor is
  // a guest AND that the exit path is healthy at speed.  If the hypervisor did
  // not take, the loop is harmless and no heartbeat ever appears - which is
  // itself the answer.
  //
  //
  // Blink our own block while spinning.  A block that alternates colour across
  // consecutive screenshots proves THIS processor is executing; one frozen at a
  // single colour proves it is not, whatever the hypervisor's heartbeat says.
  //
  PaintBlock (0x0000FF00);                 // green: reached the loop at all
  for (Attempt = 0; ; Attempt++) {
    AsmCpuid (0, &Eax, &Ebx, &Ecx, &Edx);
    if ((Attempt & 0xFFFFF) == 0) {
      PaintBlock (((Attempt >> 20) & 1) ? 0x00FF00FF : 0x0000FFFF);
    }
  }
}
