/** @file
  Diagnostics.c - the heartbeat, and what a dying processor leaves behind.

  READING THE SCREEN
  ------------------
  White 30 in the trail means the hypervisor is live.  Under it are nine rows of
  yellow decimals.  What matters is the DELTA between two photographs: numbers
  climbing means the guest is running and taking exits; everything frozen means
  it is spinning without exiting; one kind dominating means an exit loop.

    row 0  total VM exits                          liveness
    row 1  SSE instructions emulated               liveness (emulator working)
    row 2  0, or WHY-NNN: an un-emulated #UD       0 is healthy; see SseTryEmulate
    row 3  A-EE-RRR: AP fate                       A: 1 resumed, 2 un-modeled opcode
                                                   EE: AP VM-entry error
                                                   RRR: last exit reason on an AP
    row 4  VM exits taken by non-BSP processors    climbing => Windows schedules there
    row 5  BSP microcode revision                 these two must match, or the
    row 6  AP  microcode revision                  guest bugchecks 0x17E
    row 7  most recent exit reason                 alternates 0/28 on a healthy boot
    row 8  F-L-AA-II-SS: AP status word            F: why AP startup declined
                                                   L: vCPUs virtualized
                                                   AA: APs that reached C
                                                   II/SS: INIT/SIPI exits seen

  Rows 1 and 3-7 are OVERLOADED, because there are nine rows and more than nine
  things worth knowing.  Each carries its liveness meaning until something has
  gone wrong, then switches to describing that:

    once row 2 is non-zero (an un-emulated #UD), rows 3-5 become the RIP and the
    eight opcode bytes - the exact instruction to implement;

    once the AP interpreter has given up (row 3 shows A=2), rows 4-5 become the
    address and first four bytes of the instruction it could not model;

    once an AP has taken a #DF, rows 1, 5, 6 and 7 describe THE FAULT BEFORE IT
    - its CR2, its vector/error as VV-EEEEE, its RIP, and last the #DF's own
    vector/error, which is always 800000 and is what marks this reading.  A #DF
    is a fault taken while delivering another fault, and that first fault is the
    half that explains it; the #DF only says where the cascade landed.

    Row 6 used to carry "what WE injected", read live off a global that every
    processor rewrites on every injection.  By the time a screen is read it
    describes some later BSP injection, which reads as "this #DF was ours" on a
    machine where it was not.  That question is now answered by the white-20
    dump alone, from values snapshotted at the #DF itself.

  WHY IT IS A FULL REPAINT
  ------------------------
  This used to paint each row only when its value changed, which was a false
  economy: the guest owns this framebuffer too, and Windows wipes it when it
  brings up its own display.  After that wipe a row whose value is CONSTANT -
  the AP status word, a #UD count holding at some number - never came back,
  which is precisely the row worth reading.  A full repaint is ~143 K
  framebuffer writes; at one per 16384 VM-exits that is a fraction of a percent.
**/

#include "Hypervisor.h"

#if HV_DIAG_ENABLED
volatile HV_DIAG  gHvDiag = { 0 };
#else
HV_DIAG  gHvDiag = { 0 };        // written, never read; the stores fold away
#endif

//
// Stand-in per-CPU block, used only when a processor's real one cannot be
// derived from its stack pointer (see HvPcpu).  It is summed below like any
// other, so a host that has fallen back still paints real numbers rather than
// silently reporting zero - which would look exactly like a guest that had
// stopped taking exits.
//
//
// Designated, so that adding a field to HV_PCPU does not silently shift a zero
// into the wrong slot - which it has done twice.
//
HV_PCPU  gHvPcpuFallback = {
  .Magic       = HV_PCPU_MAGIC,
  .EntryCtls   = HV_ENTRY_CTLS_UNKNOWN,
  .GuestPaging = TRUE            // a fallback block is never a starting AP
};

#if HV_DIAG_ENABLED

/**
  Sum every processor's counters.

  The counters are per-CPU precisely so that no VM-exit writes a line another
  core owns; the price is that reading them is a walk, paid once per paint.
  Torn reads do not matter here - these are diagnostics, and a counter that is
  one behind is indistinguishable from a photograph taken a microsecond earlier.

  @param  Total    Receives the machine-wide totals.
  @param  ApExits  Receives the exits taken by processors other than the BSP,
                   which is the only way to tell "Windows started the second
                   core and it is running" from "it virtualized and nothing has
                   been scheduled on it".
**/
VOID
HvDiagSumCounters (
  OUT HV_COUNTERS  *Total,
  OUT UINTN        *ApExits,
  OUT UINT64       *BspMicro,
  OUT UINT64       *ApMicro
  )
{
  UINTN  Index;

  ZeroMem (Total, sizeof (*Total));
  *ApExits  = 0;
  *BspMicro = 0;
  *ApMicro  = 0;

  //
  // Index HV_MAX_CPUS is the fallback block; see above.
  //
  for (Index = 0; Index <= HV_MAX_CPUS; Index++) {
    HV_PCPU  *Pcpu;

    if (Index == HV_MAX_CPUS) {
      Pcpu = &gHvPcpuFallback;
    } else {
      VMX_VCPU  *Vcpu = HvGetVcpu (Index);

      if ((Vcpu == NULL) || (Vcpu->Pcpu == NULL)) {
        continue;
      }
      Pcpu = Vcpu->Pcpu;
    }

    if (Pcpu->Magic != HV_PCPU_MAGIC) {
      continue;                       // never brought up, or not ours
    }

    Total->Exits    += Pcpu->Count.Exits;
    Total->Init     += Pcpu->Count.Init;
    Total->Sipi     += Pcpu->Count.Sipi;
    Total->SseEmul  += Pcpu->Count.SseEmul;
    Total->Gp       += Pcpu->Count.Gp;
    Total->PfInject += Pcpu->Count.PfInject;
    Total->UdFail   += Pcpu->Count.UdFail;

    if (Pcpu->IsBsp) {
      *BspMicro = Pcpu->MicroRev;
    } else {
      *ApExits += Pcpu->Count.Exits;
      if (Pcpu->MicroRev != 0) {
        *ApMicro = Pcpu->MicroRev;
      }
    }
  }
}

//
// Clamp a value to N decimal digits, for the rows that pack several numbers
// into one decimal word.
//
STATIC
UINTN
Clamp (
  IN UINTN  Value,
  IN UINTN  Limit
  )
{
  return (Value > Limit) ? Limit : Value;
}

/**
  Paint the heartbeat.  Called from the exit handler on the BSP only.
**/
VOID
HvHeartbeat (
  VOID
  )
{
  HV_COUNTERS  C;
  UINTN        ApExits;
  UINT64       BspMicro, ApMicro;
  UINTN        Row[HV_DIAG_ROWS];
  UINTN        I;
  BOOLEAN      ApDoubleFaulted;
  BOOLEAN      ApEntryRejected;
  BOOLEAN      UnemulatedUd;
  BOOLEAN      ApInterpreterGaveUp;

  HvDiagSumCounters (&C, &ApExits, &BspMicro, &ApMicro);

  ApDoubleFaulted     = (BOOLEAN)(gHvDiag.ApLastExcVec == EXCEPTION_VECTOR_DF);
  ApEntryRejected     = (BOOLEAN)(gHvDiag.ApLastReason == EXIT_REASON_ENTRY_INVALID_GUEST);
  UnemulatedUd        = (BOOLEAN)(C.UdFail != 0);
  ApInterpreterGaveUp = (BOOLEAN)(gHvDiag.ApResult == 2);

  Row[0] = C.Exits;

  Row[1] = ApDoubleFaulted ? (UINTN)(gHvDiag.ApPrevExcCr2 & 0xFFFFFFFF)
                           : C.SseEmul;

  Row[2] = UnemulatedUd ? (gHvDiag.UdWhy * 1000u + Clamp (C.UdFail, 999))
                        : 0;

  Row[3] = UnemulatedUd
             ? (UINTN)(gHvDiag.UdRip & 0xFFFFFFFF)
             : (gHvDiag.ApResult * 100000u +
                Clamp (gHvDiag.ApEntryFailErr, 99) * 1000u +
                Clamp (gHvDiag.ApLastReason, 999));

  Row[4] = UnemulatedUd        ? gHvDiag.UdBytes[0]
         : ApInterpreterGaveUp ? (UINTN)(gHvRmFailAddr & 0xFFFFFFFF)
                               : ApExits;

  //
  // Rows 5 and 6 are the microcode revisions the two processors report.  Windows
  // compares exactly these as a processor comes online and bugchecks 0x17E when
  // they differ, so when that is the failure, this is the whole question - and
  // it is not answerable from anywhere else.
  //
  Row[5] = UnemulatedUd        ? gHvDiag.UdBytes[1]
         : ApInterpreterGaveUp ? (UINTN)gHvRmFailBytes
         : ApDoubleFaulted     ? (gHvDiag.ApPrevExcVec * 100000u +
                                  (gHvDiag.ApPrevExcErr & 0xFFFFu))
                               : (UINTN)(BspMicro >> 32);

  Row[6] = ApDoubleFaulted
             ? (UINTN)(gHvDiag.ApPrevExcRip & 0xFFFFFFFF)
             : (UINTN)(ApMicro >> 32);

  //
  // Show an AP's exception only when it is the fatal one.  Windows takes benign
  // #GP(0)s on every processor by design - probing MSRs and features inside SEH
  // blocks - so latching on any exception makes this read 1300000 forever on a
  // perfectly healthy machine, burying both the exit reason and the #PF count.
  // A #DF is the one that ends a processor.
  //
  Row[7] = ApDoubleFaulted
             ? (gHvDiag.ApLastExcVec * 100000u + (gHvDiag.ApLastExcErr & 0xFFFFu))
             : gHvDiag.LastExitReason;

  Row[8] = (UINTN)gHvApFailReason * 10000000u
         + gHvCpusLaunched * 1000000u
         + Clamp (gHvApArrived, 99) * 10000u
         + Clamp (C.Init, 99) * 100u
         + Clamp (C.Sipi, 99);

  //
  // A rejected VM entry outranks everything else these rows normally say: the
  // processor never ran, so its counters are meaningless and the state the CPU
  // refused is the only thing worth the space.
  //
  //   row 1  CR0    row 2  CR4    row 4  CR3    row 5  CS AR    row 6  TR AR
  //
  if (ApEntryRejected) {
    Row[1] = gHvDiag.EfCr0   & 0xFFFFFFFF;
    Row[2] = gHvDiag.EfCr4   & 0xFFFFFFFF;
    Row[4] = gHvDiag.EfCr3   & 0xFFFFFFFF;
    //
    // CS AR and TR AR have both read back valid; what has not is our own idea
    // of whether the guest is paging and with which CR3.
    //
    Row[5] = (gHvDiag.EfGuestPaging ? 1000000u : 0u) +
             (UINTN)(gHvDiag.EfGuestEfer & 0xFFFFu);
    Row[6] = (UINTN)(gHvDiag.EfGuestCr3 & 0xFFFFFFFF);
  }

  HvMark (30, 'H');                                // white 30 = live heartbeat
  for (I = 0; I < HV_DIAG_ROWS; I++) {
    HvMarkN (HV_BAND_BASE + I, Row[I]);
  }
}

#endif // HV_DIAG_ENABLED
