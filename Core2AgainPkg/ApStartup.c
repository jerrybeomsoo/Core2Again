/** @file
  ApStartup.c - bring the application processors under the hypervisor.

  WHY THIS EXISTS
  ---------------
  Only the instructions executed on a *virtualized* processor can be trapped and
  emulated, and only a virtualized processor reports the spoofed CPUID feature
  bits.  Leave the APs alone and the two halves of the machine disagree about
  what the CPU is: Windows starts an AP, asks it for CPUID, sees no SSE4.2 where
  the boot processor promised SSE4.2, and bugchecks 0x5D UNSUPPORTED_PROCESSOR.
  That is exactly what happens without `numproc=1`.  Multi-core support and that
  bugcheck are therefore the same problem: every core must be virtualized, or
  none may run.

  WHY NOT UEFI MP SERVICES
  ------------------------
  EFI_MP_SERVICES_PROTOCOL would be the polite way to run code on an AP, but a
  BIOS-hosted UEFI environment may not publish it. On a legacy boot the APs can
  still be in wait-for-SIPI where the BIOS left them. There is no wait loop to borrow, so
  we send INIT-SIPI-SIPI ourselves.  This also sidesteps the failure that made
  the first attempt at AP support unusable: an AP started through firmware MP
  services "returns" into the firmware's wait loop, which lives in boot-services
  memory that the OS reclaims and overwrites the moment it starts allocating -
  after which the AP is executing whatever Windows wrote there.  Here every byte
  an AP touches after the trampoline (its stack, its park loop, its VMX regions)
  is memory we allocated as EfiRuntimeServices*, so nothing can reclaim it.

  THE SEQUENCE
  ------------
    driver load   HvPrepareApStartup(): allocate the trampoline page below 1 MiB,
                  one runtime stack per AP, and each AP's VMX regions.  Nothing
                  is started yet - firmware still needs its real-mode BIOS
                  thunks on the BSP.
    ExitBootSvcs  The BSP virtualizes itself, firmware completes
                  ExitBootServices, then HvStartAps() sends INIT-SIPI-SIPI to
                  all-excluding-self. Each AP runs the trampoline (real mode ->
                  long mode), enters VMX, and parks in a guest HLT loop.
    OS runtime    Windows sends its own INIT-SIPI to each AP.  Those are VM-exits
                  now: HandleInitSignal parks the vCPU in wait-for-SIPI and
                  HandleSipi hands Windows' real-mode trampoline to RealMode.c,
                  which interprets it up to the far jump into protected mode.

  Failure is always non-fatal: if the trampoline page cannot be had, if the host
  page tables live above 4 GiB (the trampoline loads CR3 before long mode, where
  CR3 is 32-bit), or if no AP checks in before the timeout, we simply leave the
  APs alone and the machine runs exactly as it did BSP-only - which means the
  user must keep numproc=1, not that the boot breaks.
**/

#include "Hypervisor.h"
#include "ApTrampoline.h"

//
// Most APs this build will start.  A Penryn-era machine has at most four cores,
// so seven is headroom without reserving runtime memory nobody will use, and it
// fills HV_MAX_CPUS exactly: the BSP plus seven.
//
// Preparation is deliberately all-or-nothing: a machine where we could only
// prepare *some* APs would start the rest un-virtualized, and a half-virtualized
// machine is worse than a single-core one - it is exactly the 0x5D bugcheck
// this file exists to prevent.
//
#define AP_MAX_STARTED  7
#define AP_STACK_SIZE   SIZE_8KB

//
// In a diagnostic build, halt each AP the moment it reaches long mode.  An AP
// that halts can be reported on; one that faults takes the machine down with
// it, because there is no IDT that early and every exception is a triple fault.
// Stage 3 is the last point common to every path.
//
#ifdef HV_AP_DEBUG
#define AP_STOP_AT_STAGE  3
#else
#define AP_STOP_AT_STAGE  0
#endif

//
// ---------------------------------------------------------------------------
// Delay without boot services.
//
// AP startup now runs AFTER we have chained to the firmware's ExitBootServices
// (see HvExitBootServicesHook), so the boot-services Stall no longer exists by
// the time we need to wait between IPIs.  Calibrate the TSC against it while it
// is still alive - at driver load - and busy-wait on the TSC afterwards.
//
// The T9900 is a Penryn, which has constant TSC, so one fixed ticks-per-
// microsecond figure is good enough for the millisecond-scale delays here.
// ---------------------------------------------------------------------------
//
STATIC UINT64  gTscPerUs = 0;

/**
  Measure the TSC against a known firmware delay.  Must be called while boot
  services are still alive.
**/
VOID
HvCalibrateStall (
  VOID
  )
{
  UINT64  T0, T1, Rate;

  T0 = AsmReadTsc ();
  gBS->Stall (10000);                     // 10 ms
  T1 = AsmReadTsc ();
  Rate = (T1 - T0) / 10000;

  //
  // Clamp rather than trust the reading.  A firmware Stall that returns
  // immediately would give 0 and turn every later delay into a no-op; a wild
  // reading would make one of them effectively infinite.  100 MHz to 10 GHz
  // covers any processor this will run on.
  //
  if ((Rate < 100) || (Rate > 10000)) {
    Rate = 1000;                          // assume 1 GHz; delays stay sane
  }
  gTscPerUs = Rate;
}

/**
  Busy-wait for a number of microseconds.  Safe on either side of
  ExitBootServices, which the firmware Stall is not.
**/
STATIC
VOID
HvStall (
  IN UINTN  Microseconds
  )
{
  UINT64  Target;

  if (gTscPerUs == 0) {
    HvCalibrateStall ();                  // not reached in a normal boot
  }
  Target = AsmReadTsc () + (UINT64)Microseconds * gTscPerUs;
  while (AsmReadTsc () < Target) {
    CpuPause ();
  }
}

STATIC EFI_PHYSICAL_ADDRESS  gTrampoline   = 0;
STATIC VOID                  *gApStackBase = NULL;
STATIC BOOLEAN               gApPrepared   = FALSE;

//
// Bumped by each AP as it arrives in C, before it tries to virtualize itself.
// The difference between this and gHvCpusLaunched separates "the trampoline
// works but VMX bring-up failed" from "the AP never got here at all".
//
volatile UINTN  gHvApArrived = 0;

//
// Why AP startup declined; 0 means it did not.  See HV_AP_FAIL_* .
//
volatile UINT32  gHvApFailReason = HV_AP_FAIL_NONE;

//
// ---------------------------------------------------------------------------
// Local APIC (xAPIC MMIO).  Penryn predates x2APIC; if something has switched
// the APIC into x2APIC mode the MMIO window is disabled and we decline to start
// APs rather than write into a dead page.
// ---------------------------------------------------------------------------
//
#define APIC_BASE_MSR       0x0000001B
#define   APIC_BASE_EXTD    BIT10          // x2APIC enabled
#define   APIC_BASE_ENABLE  BIT11
#define APIC_ICR_LOW        0x300
#define   ICR_DELIVERY_PENDING  BIT12

STATIC
volatile UINT32 *
ApicIcr (
  VOID
  )
{
  UINT64  Base = AsmReadMsr64 (APIC_BASE_MSR);

  //
  // x2APIC would put the ICR in an MSR instead of this MMIO window; Penryn does
  // not have it, and if something has switched it on we decline rather than
  // write into a dead page.
  //
  if ((Base & APIC_BASE_EXTD) != 0) {
    return NULL;
  }

  //
  // A legacy BIOS boot commonly leaves the local APIC *disabled* - the OS is
  // expected to turn it on - and CloverEFI is a shim over exactly that.  We
  // need it to send INIT-SIPI-SIPI, so enable it rather than giving up: this is
  // the state Windows is about to establish for itself anyway.
  //
  if ((Base & APIC_BASE_ENABLE) == 0) {
    Base |= APIC_BASE_ENABLE;
    AsmWriteMsr64 (APIC_BASE_MSR, Base);
    Base = AsmReadMsr64 (APIC_BASE_MSR);
    if ((Base & APIC_BASE_ENABLE) == 0) {
      return NULL;                      // refused - nothing more we can do
    }
  }

  return (volatile UINT32 *)(UINTN)((Base & 0x000FFFFFFFFFF000ULL) + APIC_ICR_LOW);
}

//
// Send one IPI to "all excluding self" and wait for delivery to be accepted.
// The spin is bounded: a wedged APIC must not hang the boot.
//
STATIC
VOID
ApicSendIpi (
  IN volatile UINT32  *Icr,
  IN UINT32            Command
  )
{
  UINTN  Spin;

  for (Spin = 0; (Spin < 1000000) && ((*Icr & ICR_DELIVERY_PENDING) != 0); Spin++) {
    CpuPause ();
  }
  *Icr = Command;
  for (Spin = 0; (Spin < 1000000) && ((*Icr & ICR_DELIVERY_PENDING) != 0); Spin++) {
    CpuPause ();
  }
}

/**
  First instruction of ours an AP executes in C, called from the trampoline with
  the Microsoft x64 convention (slot in RCX, shadow space reserved).

  Runs with interrupts off, on this AP's own runtime stack, with the host page
  tables loaded.  It must not call anything that allocates or touches UEFI boot
  services - the BSP is inside ExitBootServices while this runs.

  @param  Slot  0-based index among the APs, claimed atomically by the trampoline.
**/
VOID
EFIAPI
ApWakeEntry (
  IN UINTN  Slot
  )
{
  VMX_VCPU  *Vcpu;

  AsmAtomicInc (&gHvApArrived);

  //
  // Slot+1 because index 0 is the BSP.  Out of range means we prepared fewer
  // vCPUs than the machine has: park rather than scribble on someone else's.
  //
  Vcpu = HvGetVcpu (Slot + 1);
  if ((Slot < AP_MAX_STARTED) && (Vcpu != NULL) && (Vcpu->VmxonRegion != NULL)) {
    HvBringUpThisCpu (Vcpu);
  }

  //
  // Past this point we are a guest (or bring-up failed and we are simply an
  // idle core).  Park until Windows sends INIT/SIPI, which now VM-exits to
  // HandleInitSignal / HandleSipi instead of resetting a core we do not own.
  //
  for (;;) {
    DisableInterrupts ();
    CpuSleep ();
  }
}

/**
  Diagnostic build only: dump the whole AP bring-up outcome and hold it on
  screen long enough to photograph.

  This exists because the heartbeat turned out to be unreadable in the window
  that matters.  A failure that kills the machine during the Windows boot logo
  never reaches the desktop, and the guest does not present our framebuffer
  writes that early - so the only reliable moment to report is right here, inside
  the ExitBootServices hook, while the display is still ours and nothing else is
  drawing on it.
**/
VOID
HvApReport (
  IN UINTN  Mark
  )
{
#if HV_DIAG_ENABLED
  UINT32  *Page = (UINT32 *)(UINTN)gTrampoline;

  HvMark  (Mark, 'A');
  HvMarkN (HV_BAND_BASE + 0, (UINTN)gHvApFailReason);                     // row0: why we declined (0 = we did not)
  //
  // How far the furthest AP got inside the trampoline, which is the only thing
  // an AP can tell us before it has an IDT: 0 never ran, 1 real mode, 2 32-bit
  // protected mode, 3 long mode, 4 about to enter C.
  //
  HvMarkN (HV_BAND_BASE + 1, (Page != NULL) ? (UINTN)*((UINT8 *)Page + AP_OFF_PROG) : 0);
  HvMarkN (HV_BAND_BASE + 2, gHvApArrived * 1000 + gHvCpusLaunched);      // row2: arrived / launched
  HvMarkN (HV_BAND_BASE + 3, (UINTN)(gTrampoline >> 12));                 // row3: SIPI vector we sent
  HvMarkN (HV_BAND_BASE + 4, (UINTN)(AsmReadMsr64 (APIC_BASE_MSR) & 0xFFFFFFFF));  // row4: IA32_APIC_BASE low
  HvMarkN (HV_BAND_BASE + 5, (UINTN)gTrampoline);                         // row5: trampoline physical address
  HvMarkN (HV_BAND_BASE + 6, (Page != NULL) ? (UINTN)Page[0] : 0);        // row6: first 4 bytes read back
  HvMarkN (HV_BAND_BASE + 7, (UINTN)(gHostCr3 & 0xFFFFFFFF));             // row7: host CR3 low
  HvMarkN (HV_BAND_BASE + 8, (UINTN)((UINT64)(UINTN)gApStackBase & 0xFFFFFFFF));   // row8: AP stack base

  //
  // Hold it.  The OS loader is waiting on our return from ExitBootServices, so
  // nothing repaints over this until we let go.
  //
  HvStall (8 * 1000 * 1000);
#else
  (VOID)Mark;                      // nothing to report in a RELEASE image
#endif
}

//
// Diagnostic builds walk HvStartAps in visible stages: each marker is left on
// screen long enough to read, so the LAST number seen before a reset names the
// step that killed the machine.  Nothing is emitted in a normal build.
//
#ifdef HV_AP_DEBUG
#define AP_STAGE(n)  do { HvMark ((n), 'A'); HvStall (2 * 1000 * 1000); } while (0)
#else
#define AP_STAGE(n)  do { } while (0)
#endif

/**
  Driver-load phase: reserve everything AP startup will need later.

  Allocation must happen here, not at ExitBootServices, because the OS loader
  has already snapshotted the memory map by then and any allocation would
  invalidate its map key.
**/
VOID
HvPrepareApStartup (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  UINTN                 I;

  //
  // The trampoline starts in real mode, so it must live in a page-aligned page
  // below 1 MiB: the SIPI vector *is* the page frame number, and it is 8 bits.
  //
  // Prefer runtime memory: an AP parks with its guest GDTR still pointing into
  // this page, and boot-services memory is reclaimed and overwritten by the OS
  // as soon as it starts allocating.  But a page below 1 MiB matters more than
  // its type - some firmware will not hand out runtime memory down there - so
  // fall back rather than decline AP startup entirely.
  Addr   = 0x00100000;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesCode, 1, &Addr);
  if (EFI_ERROR (Status)) {
    Addr   = 0x00100000;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesCode, 1, &Addr);
  }
  if (EFI_ERROR (Status)) {
    gHvApFailReason = HV_AP_FAIL_NO_LOWPAGE;
    DEBUG ((DEBUG_WARN, "[HV] No page below 1MB for the AP trampoline; BSP-only.\n"));
    return;
  }
  gTrampoline = Addr;

  //
  // One stack per AP, in runtime memory: an AP keeps running on its stack as a
  // parked guest long after boot services are gone.
  //
  gApStackBase = AllocateRuntimePages (EFI_SIZE_TO_PAGES ((AP_MAX_STARTED + 1) * AP_STACK_SIZE));
  if (gApStackBase == NULL) {
    gHvApFailReason = HV_AP_FAIL_NO_STACKS;
    DEBUG ((DEBUG_WARN, "[HV] No AP stacks; BSP-only.\n"));
    return;
  }
  ZeroMem (gApStackBase, (AP_MAX_STARTED + 1) * AP_STACK_SIZE);

  //
  // Each AP's VMX regions, allocated by us because an AP cannot allocate: it
  // wakes up with boot services already gone.
  //
  for (I = 1; I <= AP_MAX_STARTED; I++) {
    VMX_VCPU  *Vcpu = HvGetVcpu (I);

    if (Vcpu == NULL) {
      gHvApFailReason = HV_AP_FAIL_NO_REGIONS;
      return;
    }
    Vcpu->CpuIndex = I;
    if (EFI_ERROR (VmxAllocRegions (Vcpu))) {
      gHvApFailReason = HV_AP_FAIL_NO_REGIONS;
      DEBUG ((DEBUG_WARN, "[HV] AP %lu regions failed; no APs will be started.\n",
              (UINT64)I));
      return;                      // gApPrepared stays FALSE: start none of them
    }
  }

  gApPrepared = TRUE;
}

/**
  ExitBootServices phase: wake the APs and wait for them to virtualize.

  Called after the BSP virtualizes itself and firmware's ExitBootServices
  succeeds, but before the OS starts scheduling on the APs.

  @return  Number of APs that reported in.  Zero is a normal, non-fatal outcome
           on a uniprocessor machine or when startup was declined.
**/
UINTN
HvStartAps (
  VOID
  )
{
  volatile UINT32  *Icr;
  UINT8            *Page;
  UINT32            Vector;
  UINTN             Waited;
  UINTN             Settled;
  UINTN             Seen;

  if (!gApPrepared) {
    if (gHvApFailReason == HV_AP_FAIL_NONE) {
      gHvApFailReason = HV_AP_FAIL_DISABLED;
    }
    return 0;
  }

  //
  // The trampoline loads CR3 while still in 32-bit mode, so the host page
  // tables must live below 4 GiB.  They always do on the machines this targets,
  // but assuming it silently would be a triple fault rather than a message.
  //
  if (gHostCr3 >= BASE_4GB) {
    gHvApFailReason = HV_AP_FAIL_CR3_HIGH;
    DEBUG ((DEBUG_WARN, "[HV] Host CR3 above 4GB; skipping AP startup.\n"));
    return 0;
  }

  Icr = ApicIcr ();
  if (Icr == NULL) {
    gHvApFailReason = HV_AP_FAIL_NO_APIC;
    DEBUG ((DEBUG_WARN, "[HV] No usable xAPIC ICR; skipping AP startup.\n"));
    return 0;
  }

  //
  // Lay out the page: code at 0, then the GDT/GDTR and the values the code
  // reads back out of its own page.
  //
  Page = (UINT8 *)(UINTN)gTrampoline;
  HvApStampPage (
    Page,
    gHostCr3,
    (UINT64)(UINTN)gApStackBase,
    AP_STACK_SIZE,
    (UINT64)(UINTN)ApWakeEntry,
    AP_STOP_AT_STAGE
    );

  Vector = (UINT32)(gTrampoline >> 12);

  //
  // Everything is staged and nothing has been woken yet: report before taking
  // any irreversible step, so a machine that dies during wake-up still tells us
  // whether the preparation was sound.
  //
#ifdef HV_AP_DEBUG
  HvApReport (26);
#endif

  //
  // INIT, then two SIPIs - the second is the architecturally recommended retry
  // and is harmless for an AP that already started.
  //
  AP_STAGE (27);                                    // about to send INIT
  ApicSendIpi (Icr, 0x000C4500);                    // INIT, assert, all-excl-self
  HvStall (10000);                               // 10 ms

  AP_STAGE (28);                                    // INIT survived; about to SIPI
  ApicSendIpi (Icr, 0x000C4600 | Vector);           // SIPI
  HvStall (200);
  ApicSendIpi (Icr, 0x000C4600 | Vector);           // SIPI (retry)

  AP_STAGE (29);                                    // SIPIs survived; now wait

  //
  // Wait for the APs to virtualize themselves, then for the count to stop
  // moving.  Bounded at ~100 ms: a machine where an AP never arrives still
  // boots, just BSP-only.
  //
  Settled = 0;
  Seen    = 0;
  for (Waited = 0; Waited < 1000; Waited++) {
    HvStall (100);                               // 100 us per tick
    if (gHvApArrived != Seen) {
      Seen    = gHvApArrived;
      Settled = 0;
    } else if (Seen > 0 && ++Settled > 100) {
      break;                                        // 10 ms with no new arrival
    }
  }

  if (gHvApArrived == 0) {
    //
    // The IPIs went out and nothing came back.  Distinguishing this from "we
    // never sent them" is the difference between suspecting the trampoline and
    // suspecting the allocation, so record it.
    //
    gHvApFailReason = HV_AP_FAIL_NO_RESPONSE;
  }

#ifdef HV_AP_DEBUG
  HvApReport (31);                                  // final tally
#endif

  DEBUG ((DEBUG_INFO, "[HV] %lu AP(s) arrived, %lu virtualized.\n",
          (UINT64)gHvApArrived, (UINT64)gHvCpusLaunched));
  return gHvApArrived;
}
