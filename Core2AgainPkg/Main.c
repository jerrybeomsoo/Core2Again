/** @file
  Main.c - driver entry point and boot lifecycle.

  The whole hypervisor turns on the timing of one call.  At driver load we only
  *prepare*: probe VMX, build the shared page tables and MSR bitmap, reserve
  every processor's regions, and reserve what AP wake-up will need.  Nothing is
  virtualized yet, because the BIOS-hosted UEFI environment still has real-mode
  BIOS thunks to run on the boot processor. This part has no "unrestricted
  guest" feature to run them under VMX.

  The switch is thrown from a hook on ExitBootServices: the last moment of UEFI,
  in long mode, with no thunks left ahead.  The BSP virtualizes itself, the
  firmware's own ExitBootServices runs, and only then do we take the APs (see
  HvExitBootServicesHook for why that order is not negotiable).

  Everything that draws on screen lives in Display.c; the VMX per-CPU routines
  live in VmxSetup.c; orchestration lives in Mp.c and ApStartup.c.
**/

#include "Hypervisor.h"
#include <Protocol/LoadedImage.h>

//
// Saved firmware ExitBootServices, replaced by our hook below.
//
STATIC EFI_EXIT_BOOT_SERVICES  mOrigExitBs = NULL;

//
// Each stage runs once, however many times the loader calls ExitBootServices.
//
STATIC BOOLEAN  mBspVirtualized = FALSE;
#ifndef HV_NO_AP_STARTUP
STATIC BOOLEAN  mApsStarted     = FALSE;
#endif

/**
  Runs when the OS loader calls ExitBootServices.

  Two orderings matter here, and both were paid for:

  * The BSP is virtualized BEFORE chaining to the firmware, so the OS about to
    start runs under the hypervisor while firmware ran natively.

  * The APs are woken AFTER the firmware's ExitBootServices returns.  They are
    parked in the firmware's MP wait loop; the firmware signals them during its
    own teardown and waits for an answer, so an INIT-SIPI-SIPI sent first takes
    them out of that loop and the BSP blocks forever inside a routine we have
    just handed control to.  Reproduced under OVMF with two or more processors
    (see docs/architecture.md). Waking them afterwards costs nothing: the OS
    loader has not run yet, so the APs are still ours.

  We also stay hooked until ExitBootServices actually succeeds.  Un-hooking on
  entry - which this used to do - loses the APs on any boot whose first attempt
  fails: the loader gets EFI_INVALID_PARAMETER because its memory map went
  stale, rebuilds it and calls again, and that call goes straight to firmware.
  The BSP is virtualized but HvStartAps never runs, and the machine comes up
  single-core with no failure reason recorded.
**/
STATIC
EFI_STATUS
EFIAPI
HvExitBootServicesHook (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;

  HvMark (5, 'L');

  if (!mBspVirtualized) {
    VmxVirtualizeBspNow ();                // BSP becomes the guest here
    mBspVirtualized = TRUE;
  }
  HvMark (6, 'D');

  Status = mOrigExitBs (ImageHandle, MapKey);
  HvMark (9, 'E');                         // firmware EBS returned (as guest)

  if (EFI_ERROR (Status)) {
    //
    // Boot services are still alive and the loader will rebuild its map and
    // call again.  Stay hooked, and do not take the APs on a boot that has not
    // actually left boot services.
    //
    return Status;
  }

#ifndef HV_NO_AP_STARTUP
  //
  // Nothing below may call boot services: they are gone.  HvStartAps uses a
  // TSC-based delay for exactly this reason.
  //
  if (!mApsStarted) {
    HvStartAps ();
    mApsStarted = TRUE;
    HvMark (8, 'P');                       // APs started; count is on the heartbeat
  }
#endif

  return Status;
}


/**
  UEFI driver entry point. Loaded by Core2AgainLoader (or any UEFI boot manager).
  All allocation happens here; nothing is
  virtualized until the ExitBootServices hook fires.
**/
EFI_STATUS
EFIAPI
HypervisorMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  HvMark (0, 'A');                         // entry point reached

  Status = VmxCheckSupport ();
  if (EFI_ERROR (Status)) {
    HvMark (1, '!');                       // no VMX: load, do nothing, let it boot
    return EFI_SUCCESS;
  }
  HvMark (1, 'B');                         // VMX present

  Status = VmxInitShared ();               // writes sub-step digits to bands 8-12
  HvMark (2, EFI_ERROR (Status) ? '?' : 'C');
  if (EFI_ERROR (Status)) {
    return Status;
  }
  HvMark (3, VmxEptAvailable () ? 'E' : 'e');
  HvMark (4, HvEmulatePopcnt () ? 'P' : 'p');

  //
  // Reserve every processor's VMX regions now.  The APs are NOT started here:
  // an AP brought up through firmware MP services "returns" into the firmware's
  // wait loop, which is boot-services memory the OS reclaims and overwrites -
  // after which the AP executes whatever Windows wrote there.  ApStartup.c wakes
  // them onto a stack and a park loop we own instead.
  //
  Status = VmxPrepareAndVirtualizeAps ();
  if (EFI_ERROR (Status)) {
    HvMark (7, '!');
    return Status;
  }
#ifndef HV_NO_AP_STARTUP
  HvCalibrateStall ();                     // TSC vs. firmware Stall, while there is one
  HvPrepareApStartup ();                   // reserve what AP wake-up will need
#else
  gHvApFailReason = HV_AP_FAIL_DISABLED;   // built single-core on purpose
#endif
  HvMark (7, 'A');                         // per-CPU regions allocated

  //
  // Hook ExitBootServices, then fix the boot services table CRC so the loader's
  // own validation still passes.
  //
  mOrigExitBs           = gBS->ExitBootServices;
  gBS->ExitBootServices = HvExitBootServicesHook;
  gBS->Hdr.CRC32        = 0;
  gBS->CalculateCrc32 ((VOID *)gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  HvMark (7, 'H');                         // hooked; returning so firmware runs

  return EFI_SUCCESS;
}
