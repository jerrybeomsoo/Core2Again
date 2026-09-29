/** @file
  VmxExitHandler.c - the VM-exit router.

  AsmVmExitHandler (VmxAsm.asm / VmxAsm.S) saves the guest GPRs into a
  GUEST_REGS frame plus an FXSAVE image and calls HandleVmExit with pointers to
  both.  On return the assembly restores them and executes VMRESUME.

  Three kinds of exit arrive here:

    * the one we exist for - #UD from an SSE4.1/4.2/POPCNT instruction, handed
      to the decoder and emulated;
    * exits that happen unconditionally in non-root operation and that Windows
      takes constantly (CPUID, MOV CR3, XSETBV, MSR access) - these MUST be
      serviced or the guest hangs or faults immediately;
    * INIT/SIPI, which is Windows starting a second processor.

  Everything else is either reflected into the guest or reported and stopped.

  Diagnostic state lives in Diagnostics.c (gHvDiag and the per-CPU counters);
  this file records into it but never paints, except on the failure paths that
  end in CpuDeadLoop, where there is no later heartbeat to do it for us.
**/

#include "Hypervisor.h"
#include <Library/IoLib.h>

/**
  This processor's state block, from any address on its host stack.

  The stack is allocated aligned to its own size with the block at the base, so
  this is a mask.  The magic check costs one compare and turns "the invariant
  broke" from silent memory corruption into a degraded but working host.
**/
//
// Defined below, but needed by handlers that change paging or EFER: both make
// the entry controls stale for the resume they are about to do.
//
STATIC VOID SyncEntryControls (IN OUT HV_PCPU *Pcpu);

STATIC
HV_PCPU *
HvPcpu (
  IN VOID  *StackAddr
  )
{
  HV_PCPU  *Pcpu = HV_PCPU_OF (StackAddr);

  if (Pcpu->Magic == HV_PCPU_MAGIC) {
    return Pcpu;
  }

  //
  // Fall back to asking the hardware who we are.  CPUID is serializing and this
  // is the hot path, which is exactly why it is not the normal route.
  //
  {
    UINT32  Eax, Ebx, Ecx, Edx;

    AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
    gHvPcpuFallback.ApicId = Ebx >> 24;
    gHvPcpuFallback.IsBsp  = (BOOLEAN)(gHvPcpuFallback.ApicId == gHvBspApicId);
  }
  return &gHvPcpuFallback;
}

//
// Reflect a hardware exception back into the guest without advancing RIP, so it
// is delivered at the faulting instruction exactly as bare metal would.
//
STATIC
VOID
ReinjectException (
  IN UINT32  ExitIntrInfo
  )
{
  UINTN  ErrCode;

  AsmVmWrite (VMCS_ENTRY_INTR_INFO, ExitIntrInfo | INTR_INFO_VALID);

  if ((ExitIntrInfo & INTR_INFO_ERRCODE_VALID) != 0) {
    AsmVmRead  (VMCS_EXIT_INTR_ERRCODE, &ErrCode);
    AsmVmWrite (VMCS_ENTRY_EXCEPTION_ERRCODE, ErrCode);
  }
}

//
// Inject an exception we have SYNTHESIZED, with an error code we computed.
//
// Not the same operation as reflecting one: ReinjectException takes its error
// code from VMCS_EXIT_INTR_ERRCODE, which is only meaningful for the exception
// that actually caused this exit.  Calling it for a synthesized #PF - after
// writing the intended error code - silently replaced that code with whatever
// the #UD exit left in the exit field, and the guest received a page fault
// whose error code contradicted its own page tables.
//
STATIC
VOID
InjectException (
  IN UINT32   Vector,
  IN UINT32   ErrCode,
  IN BOOLEAN  HasErrCode
  )
{
  UINT32  Info = INTR_INFO_VALID |
                 (INTR_TYPE_HARDWARE_EXCEPTION << INTR_INFO_TYPE_SHIFT) |
                 Vector;

  if (HasErrCode) {
    Info |= INTR_INFO_ERRCODE_VALID;
    AsmVmWrite (VMCS_ENTRY_EXCEPTION_ERRCODE, ErrCode);
  }
  AsmVmWrite (VMCS_ENTRY_INTR_INFO, Info);

  //
  // Remember what we synthesized, so a #DF that follows can name the fault that
  // preceded it rather than only where it landed.  CR2 is cleared here and set
  // by the #PF path alone, so a non-zero CR2 on the heartbeat always belongs to
  // the vector shown beside it.
  //
  gHvDiag.InjVec = Vector;
  gHvDiag.InjErr = ErrCode;
  gHvDiag.InjCr2 = 0;
}

//
// Advance guest RIP past an instruction-execution exit (CPUID/XSETBV/...).
// For these exits VMCS_EXIT_INSTR_LEN is architecturally defined.
//
STATIC
VOID
AdvanceRip (
  VOID
  )
{
  UINTN  Rip, Len;

  AsmVmRead  (VMCS_GUEST_RIP,      &Rip);
  AsmVmRead  (VMCS_EXIT_INSTR_LEN, &Len);
  AsmVmWrite (VMCS_GUEST_RIP, Rip + Len);
}

//
// CPUID unconditionally exits.  We pass it through, but for leaf 1 we OR in the
// SSE4.1/SSE4.2/POPCNT feature bits so guest feature detection sees them as
// present - the instructions themselves are then trapped via #UD and emulated.
//
STATIC
VOID
HandleCpuid (
  IN OUT GUEST_REGS  *Regs
  )
{
  UINT32  Eax, Ebx, Ecx, Edx;

  AsmCpuidEx ((UINT32)Regs->Rax, (UINT32)Regs->Rcx, &Eax, &Ebx, &Ecx, &Edx);

  if ((UINT32)Regs->Rax == 1) {
    Ecx |= (CPUID1_ECX_SSE41 | CPUID1_ECX_SSE42 | CPUID1_ECX_POPCNT);
  }

  Regs->Rax = Eax;
  Regs->Rbx = Ebx;
  Regs->Rcx = Ecx;
  Regs->Rdx = Edx;
  AdvanceRip ();
}

STATIC
VOID
HandleXsetbv (
  IN GUEST_REGS  *Regs
  )
{
  UINT32  Index = (UINT32)Regs->Rcx;
  UINT64  Value = ((UINT64)(UINT32)Regs->Rdx << 32) | (UINT32)Regs->Rax;

  //
  // XSETBV requires CR4.OSXSAVE=1 in the *current* (host) context.  Our minimal
  // host CR4 never enables it, so executing the guest's XSETBV here would #GP
  // and triple-fault the host - the "frozen at the Windows logo" hang on real
  // Penryn hardware (the T9900 exposes XSAVE, so Windows does use XSETBV).
  // Enable OSXSAVE first; the guest's requested XCR0 is valid for this same CPU.
  //
  AsmWriteCr4 (AsmReadCr4 () | CR4_OSXSAVE);
  AsmXsetbv (Index, Value);
  AdvanceRip ();
}

STATIC
VOID
HandleRdmsr (
  IN OUT GUEST_REGS  *Regs,
  IN     HV_PCPU     *Pcpu
  )
{
  UINT32  Index = (UINT32)Regs->Rcx;
  UINT64  V;

  //
  // Microcode revision: an application processor reports whatever the bootstrap
  // processor currently reports, and the bootstrap processor reports the truth.
  //
  // Both cores of the target leave the BIOS at the same revision - measured,
  // 0xa07 on each - so the mismatch is not the firmware's. It is the boot
  // order: winload applies a microcode update to the bootstrap processor
  // BEFORE ExitBootServices, which is before this hypervisor exists. By the
  // time the kernel starts the second processor and tries to update that one,
  // it is already in VMX non-root operation, where the update does not reliably
  // take. One processor updated, one not, and Windows compares them as the
  // second comes online: bugcheck 0x17E.
  //
  // Tracking the bootstrap processor's live value rather than a captured one
  // matters: an earlier attempt pinned every processor to the revision read at
  // driver load, which is the pre-update BIOS value, so the guest read back its
  // own update as having failed - the same bugcheck by the opposite route.
  //
  if (Index == MSR_IA32_BIOS_SIGN_ID) {
    if (Pcpu->IsBsp) {
      V = AsmReadMsr64 (Index);
      gHvBspMicroRev = V;
    } else {
      V = gHvBspMicroRev;
    }
  } else {
    V = AsmReadMsr64 (Index);
  }

  Regs->Rax = (UINT32)V;
  Regs->Rdx = (UINT32)(V >> 32);
  AdvanceRip ();
}

STATIC
VOID
HandleWrmsr (
  IN GUEST_REGS   *Regs,
  IN OUT HV_PCPU  *Pcpu
  )
{
  UINT32  Index = (UINT32)Regs->Rcx;
  UINT64  V     = ((UINT64)(UINT32)Regs->Rdx << 32) | (UINT32)Regs->Rax;

  if (Index == MSR_IA32_EFER) {
    UINTN  Ctls;

    Pcpu->GuestEfer = V;

    //
    // Where the write has to land depends on the part, and getting it wrong is
    // silent.  With Load-IA32_EFER the VMCS field is what VM entry puts into
    // EFER, so writing the real MSR here is undone by the very next entry - the
    // field is the only thing that persists.  Penryn has no such field: there
    // EFER is one register shared with the host, so the write goes to the MSR
    // and must keep LME set, or the host stops being in long mode on VMRESUME.
    //
    // LMA is left to SyncEntryControls either way; it has to agree with the
    // IA-32e-mode-guest control, which is not decided here.
    //
    Ctls = Pcpu->EntryCtls;
    if (Ctls == HV_ENTRY_CTLS_UNKNOWN) {
      AsmVmRead (VMCS_ENTRY_CTLS, &Ctls);
    }
    if ((Ctls & ENTRY_CTL_LOAD_IA32_EFER) != 0) {
      AsmVmWrite (VMCS_GUEST_IA32_EFER, (UINTN)(V & ~(UINT64)EFER_LMA_BIT));
      Pcpu->LastEferValid = FALSE;          // we just wrote it from elsewhere
    } else {
      AsmWriteMsr64 (Index, V | EFER_LME_BIT);
    }

    //
    // LMA was just cleared out of that field and the entry control still says
    // IA-32e mode, which is the mismatch VM entry rejects.  Re-derive both now:
    // the sync at the top of this exit ran before the write existed.
    //
    SyncEntryControls (Pcpu);
    AdvanceRip ();
    return;
  }

  AsmWriteMsr64 (Index, V);
  AdvanceRip ();
}

//
// INIT: per the SDM the guest is left in the "wait-for-SIPI" activity state, so
// resuming simply waits for the SIPI.  Clear interruptibility (blocking-by-
// STI/MOV-SS) as VM-entry into wait-for-SIPI requires.
//
STATIC
VOID
HandleInitSignal (
  IN HV_PCPU  *Pcpu
  )
{
  gHvDiag.LastEvent = 2;

  //
  // An INIT delivered to an AP is part of the OS's INIT-SIPI startup sequence.
  // An INIT delivered to the BSP is different: on legacy Intel chipsets the
  // keyboard-controller reset command can assert INIT# instead of resetting
  // the platform.  VMX turns that signal into an exit, so parking the BSP in
  // wait-for-SIPI leaves the entire machine frozen during a restart.
  //
  // ICH8's CF9 reset control register distinguishes a CPU-only INIT (SYS_RST=0)
  // from a hard platform reset (SYS_RST=1).  Write SYS_RST with RST_CPU clear,
  // then raise RST_CPU to reset the chipset and both processors.  If a platform
  // ignores CF9, checkpoint 31 remains on screen in a DEBUG build.
  //
  if (Pcpu->IsBsp) {
    HvMark (31, 'R');
    IoWrite8 (0xCF9, 0x02);
    IoWrite8 (0xCF9, 0x06);
    CpuDeadLoop ();
  }

  AsmVmWrite (VMCS_GUEST_INTERRUPTIBILITY, 0);
  AsmVmWrite (VMCS_GUEST_ACTIVITY_STATE, 3);   // 3 = wait-for-SIPI
}

/**
  SIPI: Windows is starting this processor.

  On a CPU without "unrestricted guest" - which is every part this targets - an
  AP that receives INIT then SIPI cannot simply be resumed, because it must
  begin in 16-bit real mode at the SIPI vector and VM-entry forbids that.
  RealMode.c interprets Windows' trampoline prologue in software up to the far
  jump into protected mode, then writes protected-mode guest state so hardware
  takes over from there.
**/
STATIC
VOID
HandleSipi (
  IN OUT GUEST_REGS  *Regs,
  IN OUT HV_PCPU     *Pcpu
  )
{
  UINTN  ExitQualification;
  UINTN  GuestCr3;
  UINT8  Vector;

  gHvDiag.LastEvent = 1;
  AsmVmRead (VMCS_EXIT_QUALIFICATION, &ExitQualification);   // SIPI vector in bits 7:0
  AsmVmRead (VMCS_GUEST_CR3, &GuestCr3);
  Vector = (UINT8)(ExitQualification & 0xFF);

#ifdef HV_AP_DEBUG
  //
  // Diagnostic build: stop this AP here rather than resuming it into the
  // interpreted trampoline.  Resuming with even slightly wrong guest state
  // triple-faults, which resets the machine and destroys the evidence; parking
  // the AP instead leaves the BSP running and repainting, so the AP status word
  // on row 8 can actually be read.  White 25 means the SIPI reached us - i.e.
  // everything up to RealMode.c works.
  //
  HvMark  (25, 'S');
  HvMarkN (HV_BAND_BASE + 0, (UINTN)Vector);
  CpuDeadLoop ();
#endif

  //
  // RealModeStartAp writes VMCS_ENTRY_CTLS itself, so our cached copy is stale
  // either way.
  //
  Pcpu->EntryCtls = HV_ENTRY_CTLS_UNKNOWN;

  if (RealModeStartAp (Vector, (UINT64)GuestCr3, Regs, Pcpu)) {
    gHvDiag.ApResult = 1;
    return;   // the assembly will VMRESUME into the protected-mode guest
  }

  //
  // The trampoline used an opcode the interpreter does not model.  Make it
  // visible instead of silently parking the core: a stuck AP hangs the whole
  // boot while the BSP waits for it.  gHvRmFailAddr/Bytes name the instruction.
  //
  gHvDiag.ApResult = 2;
  HvMark  (23, 'S');                              // white 23 = real-mode AP failure
  HvMarkN (HV_BAND_BASE + 0, (UINTN)Vector);
  HvMarkN (HV_BAND_BASE + 1, Pcpu->Count.Sipi);
  DEBUG ((DEBUG_ERROR,
    "[HV] SIPI vector 0x%02x: trampoline used an un-modeled opcode; parking core.\n",
    (UINT32)Vector));
  CpuDeadLoop ();
}

/**
  Keep the IA-32e-mode-guest VM-entry control, and EFER.LMA beside it, agreeing
  with the guest's actual mode before every VMRESUME.

  This is what lets an AP walk protected -> long mode after CommitProtected
  hands it to hardware: the trampoline enables long mode itself, and the first
  exit after that re-derives the control so VMRESUME succeeds.

  The decision is NOT the real CR0.PG.  Without unrestricted guest we force that
  bit on so an unpaged guest is legal at all, so it says nothing about the
  guest.  What decides IA-32e mode is the guest's own paging intent - tracked in
  Pcpu->GuestPaging, because we own every MOV to CR0 - and its EFER.LME, which
  on a part with no VMCS EFER field is knowable only because WRMSR to it is
  intercepted.

  Two VM-entry checks have to be satisfied together, and each cost a bring-up
  round to find:

    LMA must equal the IA-32e-mode-guest control; and
    if CR0.PG is 1, LME must equal LMA.

  The second is the awkward one.  Windows' trampoline sets LME several
  instructions BEFORE it turns paging on, and our forced PG makes LME=1/LMA=0
  illegal in that window - so while paging is shadowed off, LME is shadowed off
  with it.  Both reappear the moment the guest pages for itself.  Every other
  bit of EFER is the guest's; NXE in particular, whose loss turns every NX
  page-table entry into a reserved-bit fault far from the cause.

  Both the control word and the EFER value are cached per-CPU because we are
  their only writer, which turns the steady state - the overwhelming majority of
  exits, where nothing about the guest's mode changed - into two compares
  instead of two VMWRITEs.
**/
STATIC
VOID
SyncEntryControls (
  IN OUT HV_PCPU  *Pcpu
  )
{
  UINTN    Ctls, Want;
  BOOLEAN  Paging;

  UINTN    Efer;
  BOOLEAN  HaveEferCtls;

  Ctls = Pcpu->EntryCtls;
  if (Ctls == HV_ENTRY_CTLS_UNKNOWN) {
    AsmVmRead (VMCS_ENTRY_CTLS, &Ctls);
  }

  //
  // Where EFER comes from depends on the part.  With the VMCS controls present,
  // Save-IA32_EFER has already refreshed the field with the guest's real EFER
  // on this very exit - NXE, SCE and all - so it is authoritative and must not
  // be overwritten wholesale from anything we track.  Doing that strips NXE and
  // turns every NX page-table entry into a reserved-bit fault, which surfaces
  // as PAGE_FAULT_IN_NONPAGED_AREA nowhere near the cause.
  //
  // Penryn has neither control, so there the tracked copy is all there is: EFER
  // is one register shared with the host, whose LME is always 1 because the
  // host is in long mode, and the guest's own LME is knowable only because
  // WRMSR to it is intercepted.
  //
  HaveEferCtls = (BOOLEAN)((Ctls & ENTRY_CTL_LOAD_IA32_EFER) != 0);
  Efer         = (UINTN)Pcpu->GuestEfer;

  //
  // Not the real CR0.PG: that is forced on so a VMX guest is legal at all
  // without unrestricted guest.  IA-32e mode is the guest's own paging intent
  // AND its LME.
  //
  Paging = (BOOLEAN)(Pcpu->GuestPaging && ((Efer & EFER_LME_BIT) != 0));

  Want = Paging ? (Ctls | ENTRY_CTL_IA32E_MODE_GUEST)
                : (Ctls & ~(UINTN)ENTRY_CTL_IA32E_MODE_GUEST);

  if ((Want != Ctls) || (Pcpu->EntryCtls == HV_ENTRY_CTLS_UNKNOWN)) {
    AsmVmWrite (VMCS_ENTRY_CTLS, Want);
    Pcpu->EntryCtls = Want;
  }

  if (HaveEferCtls) {
    UINTN  NewEfer = Efer;

    if (!Pcpu->GuestPaging) {
      NewEfer &= ~(UINTN)(EFER_LME_BIT | EFER_LMA_BIT);
    } else if (Paging) {
      NewEfer |= EFER_LMA_BIT;
    } else {
      NewEfer &= ~(UINTN)EFER_LMA_BIT;
    }
    if (!Pcpu->LastEferValid || (NewEfer != Pcpu->LastEfer)) {
      AsmVmWrite (VMCS_GUEST_IA32_EFER, NewEfer);
      Pcpu->LastEfer      = NewEfer;
      Pcpu->LastEferValid = TRUE;
    }
  }
}

//
// Clamp a proposed CR0/CR4 value to the VMX-required fixed bits so the value we
// place in the VMCS is always VM-entry-legal (bits set in FIXED0 must be 1;
// bits clear in FIXED1 must be 0).
//
STATIC
UINT64
CrApplyFixed (
  IN UINT64   Value,
  IN UINT64   Fixed0,
  IN UINT64   Fixed1,
  IN BOOLEAN  IsCr0
  )
{
  //
  // IA32_VMX_CR0_FIXED0 reports PE and PG as required whatever the processor
  // supports, but with unrestricted guest those two are NOT enforced - that is
  // the whole point of the control.  Forcing them there would deny the guest
  // the real mode the hardware is willing to run, so honour them only when we
  // are the ones who need CR0.PG set.
  //
  if (IsCr0 && VmxUnrestrictedGuestAvailable ()) {
    Fixed0 &= ~(UINT64)(CR0_PE_BIT | CR0_PG_BIT);
  }
  return (Value | Fixed0) & Fixed1;
}

//
// Service a control-register access VM-exit (reason 28).  The one that MUST be
// handled on Core 2 / Penryn is MOV to/from CR3 (default-1 exiting there), but
// we service every sub-case so no CR access can ever wedge the guest.
//
STATIC
VOID
HandleCrAccess (
  IN OUT GUEST_REGS  *Regs,
  IN OUT HV_PCPU     *Pcpu
  )
{
  UINTN   Qual;
  UINT32  CrNum, Type;
  UINT64  Val;
  UINTN   Cur;

  AsmVmRead (VMCS_EXIT_QUALIFICATION, &Qual);
  CrNum = CR_ACCESS_CR_NUM (Qual);
  Type  = CR_ACCESS_TYPE (Qual);

  switch (Type) {
    case CR_ACCESS_TYPE_TO_CR:
      Val = HvReadGpr (Regs, CR_ACCESS_GPR (Qual));
      switch (CrNum) {
        case 3:
          //
          // Record it always; apply it only once the guest is actually paging.
          // Before that it is running on our identity map and installing its
          // half-built tables early would fault the very next fetch.
          //
          Pcpu->GuestCr3 = Val;
          if (Pcpu->GuestPaging) {
            AsmVmWrite (VMCS_GUEST_CR3, (UINTN)Val);
          }
          break;

        case 0: {
          BOOLEAN  WantPaging = (BOOLEAN)((Val & CR0_PG_BIT) != 0);
          UINT64   RealCr0    = Val;

          //
          // The transition that matters.  Turning paging on means swapping our
          // identity map for the CR3 the guest asked for earlier; turning it
          // off means swapping back.  CR0.PG itself stays set in the real
          // register either way, and the guest sees its own value in the shadow.
          //
          if (WantPaging != Pcpu->GuestPaging) {
            AsmVmWrite (VMCS_GUEST_CR3,
                        (UINTN)(WantPaging ? Pcpu->GuestCr3
                                           : VmxPaeIdentityCr3 ()));
            Pcpu->GuestPaging = WantPaging;
          }

          if (!VmxUnrestrictedGuestAvailable ()) {
            RealCr0 |= CR0_PE_BIT | CR0_PG_BIT;      // legal guest, shadowed
          }
          AsmVmWrite (VMCS_GUEST_CR0,
                      (UINTN)CrApplyFixed (RealCr0, VmxCr0Fixed0 (),
                                           VmxCr0Fixed1 (), TRUE));
          AsmVmWrite (VMCS_CR0_READ_SHADOW, (UINTN)Val);

          //
          // Paging state just moved, and the entry controls were synced before
          // this exit was decoded.  Re-derive them or we VMRESUME into the mode
          // the guest was in a moment ago.
          //
          SyncEntryControls (Pcpu);
          break;
        }

        case 4: {
          UINT64  Real = Val | CR4_VMXE;

          //
          // While paging is shadowed off the guest runs on a PAE identity map,
          // so PAE has to stay on underneath whatever the guest thinks. Once it
          // pages for itself its own setting is the one that must hold.
          //
          if (!Pcpu->GuestPaging && !VmxUnrestrictedGuestAvailable ()) {
            Real |= CR4_PAE;
          }
          AsmVmWrite (VMCS_GUEST_CR4,
                      (UINTN)CrApplyFixed (Real, VmxCr4Fixed0 (),
                                           VmxCr4Fixed1 (), FALSE));
          AsmVmWrite (VMCS_CR4_READ_SHADOW, (UINTN)(Val & ~(UINT64)CR4_VMXE));
          break;
        }
        default:
          break;   // CR8: no shadow; not enabled to exit here.
      }
      break;

    case CR_ACCESS_TYPE_FROM_CR:
      switch (CrNum) {
        //
        // Report what the guest believes, not what we installed underneath it.
        //
        case 3:  Cur = (UINTN)Pcpu->GuestCr3;            break;
        case 0:  AsmVmRead (VMCS_CR0_READ_SHADOW, &Cur); break;
        case 4:  AsmVmRead (VMCS_CR4_READ_SHADOW, &Cur); break;
        default: Cur = 0;                                break;
      }
      HvWriteGpr (Regs, CR_ACCESS_GPR (Qual), (UINT64)Cur);
      break;

    case CR_ACCESS_TYPE_CLTS:
      AsmVmRead (VMCS_GUEST_CR0, &Cur);
      Cur &= ~(UINTN)BIT3;                         // clear CR0.TS
      AsmVmWrite (VMCS_GUEST_CR0, Cur);
      break;

    case CR_ACCESS_TYPE_LMSW:
      AsmVmRead (VMCS_GUEST_CR0, &Cur);
      //
      // LMSW writes CR0 bits 3:0 (PE,MP,EM,TS) but can only set PE, never clear.
      //
      Cur = (Cur & ~(UINTN)0xF) | (CR_ACCESS_LMSW_SRC (Qual) & 0xF) | (Cur & (UINTN)BIT0);
      AsmVmWrite (VMCS_GUEST_CR0,
                  (UINTN)CrApplyFixed (Cur, VmxCr0Fixed0 (),
                                       VmxCr0Fixed1 (), TRUE));
      break;

    default:
      break;
  }

  AdvanceRip ();
}

#if HV_DIAG_ENABLED

//
// Pack the guest's mode-consistency bits into one small number for on-screen
// diagnosis of a VM-entry failure (reason 33).  A healthy 64-bit guest reads
// 255 (all of PE|PG|PAE|LMA|LME|IA32e|CS.L|CR3!=0).  A missing bit names the
// broken invariant; +256/+512 means the failure followed a SIPI/INIT (AP path).
//   1=PE  2=PG  4=PAE  8=LMA  16=LME  32=IA32e-ctl  64=CS.L  128=CR3!=0
//
STATIC
UINTN
GuestModeFlags (
  VOID
  )
{
  UINTN  Cr0, Cr4, Efer, Ctls, CsAr, Cr3, F;

  AsmVmRead (VMCS_GUEST_CR0,       &Cr0);
  AsmVmRead (VMCS_GUEST_CR4,       &Cr4);
  AsmVmRead (VMCS_GUEST_IA32_EFER, &Efer);
  AsmVmRead (VMCS_ENTRY_CTLS,      &Ctls);
  AsmVmRead (VMCS_GUEST_CS_AR,     &CsAr);
  AsmVmRead (VMCS_GUEST_CR3,       &Cr3);

  F = 0;
  if ((Cr0  & BIT0)  != 0) { F |= 1;   }    // CR0.PE
  if ((Cr0  & BIT31) != 0) { F |= 2;   }    // CR0.PG
  if ((Cr4  & BIT5)  != 0) { F |= 4;   }    // CR4.PAE
  if ((Efer & BIT10) != 0) { F |= 8;   }    // EFER.LMA
  if ((Efer & BIT8)  != 0) { F |= 16;  }    // EFER.LME
  if ((Ctls & ENTRY_CTL_IA32E_MODE_GUEST) != 0) { F |= 32; }
  if ((CsAr & BIT13) != 0) { F |= 64;  }    // CS.L (64-bit code segment)
  if (Cr3 != 0)            { F |= 128; }
  return F | (gHvDiag.LastEvent << 8);
}
#endif

/**
  Called from AsmVmExitHandler when VMRESUME returns - which it only does when
  VM entry was rejected, i.e. the VMCS describes a guest the CPU will not run.

  Without this the assembly simply halted, and an unacceptable VMCS looked
  exactly like a guest that had stopped taking exits.  The VM-instruction error
  number names the specific check that failed, which is the difference between
  "something is wrong" and a line in the SDM.
**/
VOID
EFIAPI
HvVmResumeFailed (
  VOID
  )
{
  UINTN     Err    = 0;
  UINTN     Rip    = 0;
  UINTN     Reason = 0;
  HV_PCPU  *Pcpu   = HvPcpu (&Err);      // a local: definitely on the host stack

  AsmVmRead (VMCS_VM_INSTRUCTION_ERROR, &Err);
  AsmVmRead (VMCS_GUEST_RIP,            &Rip);
  AsmVmRead (VMCS_EXIT_REASON,          &Reason);

  //
  // Record before painting.  The paint will be overwritten by the BSP's next
  // heartbeat; this will not.
  //
  if (!Pcpu->IsBsp) {
    gHvDiag.ApEntryFailErr = (Err != 0) ? Err : 0xFF;   // 0xFF: rejected, error unread
  }

  HvMark  (24, 'V');                                       // white 24 = entry rejected
  HvMarkN (HV_BAND_BASE + 0, Err);                         // VM-instruction error
  HvMarkN (HV_BAND_BASE + 1, (UINTN)(Rip & 0xFFFFFFFF));   // guest RIP low
  HvMarkN (HV_BAND_BASE + 2, (UINTN)(Rip >> 32));          // guest RIP high
  HvMarkN (HV_BAND_BASE + 3, (UINTN)(Reason & EXIT_REASON_MASK));
  HvMarkN (HV_BAND_BASE + 4, gHvCpusLaunched);

  DEBUG ((DEBUG_ERROR, "[HV] VM entry rejected, VM-instruction error %lu\n", (UINT64)Err));
  //
  // The caller halts this CPU; do not return into a VMRESUME loop.
  //
}

//
// A VM-ENTRY failure: the CPU read the VMCS we built and refused it.  The
// generic "unexpected exit reason" dump is the wrong dump here - the reason
// alone (33) says only "invalid guest state", never which field - so show the
// guest state we actually programmed, which is where the answer is.
//
STATIC
VOID
ReportEntryFailure (
  IN UINT32   BasicReason,
  IN UINTN    GuestRip,
  IN HV_PCPU  *Pcpu
  )
{
#if HV_DIAG_ENABLED
  UINTN  Cr0, Cr3, Cr4, Rflags, CsAr, Activity, Intr, Efer;

  AsmVmRead (VMCS_GUEST_CR0,              &Cr0);
  AsmVmRead (VMCS_GUEST_CR3,              &Cr3);
  AsmVmRead (VMCS_GUEST_CR4,              &Cr4);
  AsmVmRead (VMCS_GUEST_RFLAGS,           &Rflags);
  AsmVmRead (VMCS_GUEST_CS_AR,            &CsAr);
  AsmVmRead (VMCS_GUEST_ACTIVITY_STATE,   &Activity);
  AsmVmRead (VMCS_GUEST_INTERRUPTIBILITY, &Intr);
  AsmVmRead (VMCS_GUEST_IA32_EFER,        &Efer);

  HvMark  (22, 'V');                                    // white 22 = VMCS rejected
  HvMarkN (HV_BAND_BASE + 0, (UINTN)BasicReason);       // 33 invalid guest state, 34 MSR load
  HvMarkN (HV_BAND_BASE + 1, Cr0    & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 2, Cr4    & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 3, Efer   & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 4, Rflags & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 5, CsAr   & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 6, (Activity & 0xFF) * 1000000 + (Intr & 0xFFFF));
  HvMarkN (HV_BAND_BASE + 7, Cr3 & 0xFFFFFFFF);
  HvMarkN (HV_BAND_BASE + 8, GuestRip & 0xFFFFFFFF);

  //
  // Latch it for the heartbeat: this processor is about to stop, and the BSP is
  // the only one that can still paint.
  //
  {
    UINTN  V0, V3, V4, VCs, VTr, VEf;

    AsmVmRead (VMCS_GUEST_CR0,       &V0);
    AsmVmRead (VMCS_GUEST_CR3,       &V3);
    AsmVmRead (VMCS_GUEST_CR4,       &V4);
    AsmVmRead (VMCS_GUEST_CS_AR,     &VCs);
    AsmVmRead (VMCS_GUEST_TR_AR,     &VTr);
    AsmVmRead (VMCS_GUEST_IA32_EFER, &VEf);

    gHvDiag.EfCr0  = V0;  gHvDiag.EfCr3  = V3;  gHvDiag.EfCr4  = V4;
    gHvDiag.EfCsAr = VCs; gHvDiag.EfTrAr = VTr; gHvDiag.EfEfer = VEf;

    gHvDiag.EfGuestPaging = Pcpu->GuestPaging;
    gHvDiag.EfGuestCr3    = Pcpu->GuestCr3;
    gHvDiag.EfGuestEfer   = Pcpu->GuestEfer;
  }

  DEBUG ((DEBUG_ERROR, "[HV] VM-entry failure %lu\n", (UINT64)BasicReason));
#endif
  CpuDeadLoop ();
}

//
// #DF: never healthy - a first guest fault's handler itself faulted, one step
// from an opaque triple fault.  Freeze and dump where the #DF hit, where the
// most recent #GP hit (often the true first fault), the #GP count, how many SSE
// instructions we had emulated (implicates or exonerates the emulator), and -
// the half that usually explains it - the exception WE synthesized just before.
//
STATIC
VOID
ReportDoubleFault (
  IN UINTN  DfRip
  )
{
#if HV_DIAG_ENABLED
  HV_COUNTERS  C;
  UINTN        ApExits, DfCs;
  HV_PCPU      *Self;

  //
  // Freeze the injection state before anything else can move it.
  //
  //
  // &DfCs is a local, so it is an address on this processor's host stack -
  // which is what HvPcpu masks.  Passing anything else (NULL included) walks
  // the mask off into nothing and dereferences it.
  //
  Self = HvPcpu (&DfCs);

  gHvDiag.DfInjVec = gHvDiag.InjVec;
  gHvDiag.DfInjErr = gHvDiag.InjErr;
  gHvDiag.DfInjCr2 = gHvDiag.InjCr2;

  {
    UINT64  BspMicro, ApMicro;
    HvDiagSumCounters (&C, &ApExits, &BspMicro, &ApMicro);
  }
  AsmVmRead (VMCS_GUEST_CS_SELECTOR, &DfCs);

  //
  //   row 0  VV-EEEEE  the fault BEFORE this one - the one to fix
  //   row 1            ...its CR2, if it was a #PF
  //   row 2/3          #DF RIP, low then high
  //   row 4            ...the precursor's RIP, low half
  //   row 5  B-AAA-III WHICH processor froze here: BSP flag, APIC id, index
  //   row 6            CS at the #DF
  //   row 7  VV-EEEEE  what WE injected as of the #DF, 0 if nothing
  //   row 8            ...its CR2
  //
  HvMark  (20, 'D');                                                   // white 20 = #DF dump
  HvMarkN (HV_BAND_BASE + 0, gHvDiag.ApPrevExcVec * 100000u +
                             (gHvDiag.ApPrevExcErr & 0xFFFFu));
  HvMarkN (HV_BAND_BASE + 1, (UINTN)(gHvDiag.ApPrevExcCr2 & 0xFFFFFFFF));
  HvMarkN (HV_BAND_BASE + 2, (UINTN)(DfRip & 0xFFFFFFFF));
  HvMarkN (HV_BAND_BASE + 3, (UINTN)(DfRip >> 32));
  HvMarkN (HV_BAND_BASE + 4, (UINTN)(gHvDiag.ApPrevExcRip & 0xFFFFFFFF));
  HvMarkN (HV_BAND_BASE + 5, (Self->IsBsp ? 1000000u : 0u) +
                             (UINTN)(Self->ApicId & 0xFF) * 1000u +
                             (Self->CpuIndex & 0xFF));
  HvMarkN (HV_BAND_BASE + 6, DfCs);
  HvMarkN (HV_BAND_BASE + 7, gHvDiag.DfInjVec * 100000u + (gHvDiag.DfInjErr & 0xFFFFu));
  HvMarkN (HV_BAND_BASE + 8, (UINTN)(gHvDiag.DfInjCr2 & 0xFFFFFFFF));

  DEBUG ((DEBUG_ERROR, "[HV] #DF rip=0x%lx lastGP=0x%lx gp=%lu sse=%lu\n",
          (UINT64)DfRip, gHvDiag.LastGpRip, (UINT64)C.Gp, (UINT64)C.SseEmul));
#endif
  CpuDeadLoop ();
}


//
// Record an un-emulated #UD.  The guest is about to take
// STATUS_ILLEGAL_INSTRUCTION for an instruction we advertised support for, so
// these bytes name the exact opcode to implement.  Read only within the page -
// the next one may be absent, and 8 bytes identifies any of these encodings.
//
STATIC
VOID
RecordUnemulatedUd (
  IN UINT64  GuestRip,
  IN UINT64  GuestCr3,
  IN UINT32  Why
  )
{
#if HV_DIAG_ENABLED
  UINT8  Ud[8];
  UINTN  Avail;

  ZeroMem (Ud, sizeof (Ud));
  Avail = SIZE_4KB - (UINTN)(GuestRip & (SIZE_4KB - 1));
  if (Avail > sizeof (Ud)) {
    Avail = sizeof (Ud);
  }

  if (GuestReadLinear (GuestCr3, GuestRip, Ud, Avail)) {
    gHvDiag.UdBytes[0] = (UINT32)Ud[0] | ((UINT32)Ud[1] << 8) |
                         ((UINT32)Ud[2] << 16) | ((UINT32)Ud[3] << 24);
    gHvDiag.UdBytes[1] = (UINT32)Ud[4] | ((UINT32)Ud[5] << 8) |
                         ((UINT32)Ud[6] << 16) | ((UINT32)Ud[7] << 24);
  }
  gHvDiag.UdRip = GuestRip;
  gHvDiag.UdWhy = Why;
#endif
}

/**
  Emulate the SSE4.1/4.2/POPCNT instruction that raised this #UD.

  Three outcomes, and the difference between the last two is a bug that took a
  long time to find:

    * emulated - commit the flags and step past the instruction;
    * recognised, but its memory operand was not resident - the guest is owed a
      #PF at that address, NOT a #UD.  #UD is raised BEFORE the operand is
      evaluated, so the operand's page may be legitimately paged out; on a CPU
      that implements the instruction the access would simply have taken a #PF,
      the OS would have paged it in, and the instruction would have re-executed.
      Reporting #UD instead tells the application its own code is illegal;
    * genuinely unknown - behave like hardware and deliver #UD.
**/
STATIC
VOID
HandleInvalidOpcode (
  IN OUT GUEST_REGS  *Regs,
  IN OUT VOID        *FxArea,
  IN OUT HV_PCPU     *Pcpu,
  IN     UINT64       GuestRip
  )
{
  UINTN   GuestCr3, Rflags;
  UINT64  NewRflags;
  UINT64  FaultAddr = 0;
  UINT32  InstrLen  = 0;
  UINT32  Why       = 0;

  //
  // Pcpu->GuestCr3 is authoritative: every MOV to CR3 exits and is recorded
  // there, so reading the VMCS field again costs a VMREAD to learn what we
  // already knew.  Fall back to the field only before the first CR3 exit.
  //
  if (Pcpu->GuestPaging && (Pcpu->GuestCr3 != 0)) {
    GuestCr3 = (UINTN)Pcpu->GuestCr3;
  } else {
    AsmVmRead (VMCS_GUEST_CR3, &GuestCr3);
  }
  AsmVmRead (VMCS_GUEST_RFLAGS, &Rflags);
  NewRflags = Rflags;

  if (SseTryEmulate (Regs, FxArea, GuestRip, (UINT64)GuestCr3, &NewRflags,
                     &InstrLen, &FaultAddr, &Why))
  {
    //
    // The #UD exception exit does not provide a reliable VM-exit instruction
    // length for faults, which is exactly why the decoder computes it itself.
    //
#if HV_DIAG_ENABLED
    Pcpu->Count.SseEmul++;
#endif
    //
    // Most of SSE4.1 leaves the flags alone; only PTEST and the string compares
    // touch them.  Writing RFLAGS regardless costs a VMWRITE on every emulated
    // instruction, and on the target that is millions of them per boot.
    //
    if (NewRflags != (UINT64)Rflags) {
      AsmVmWrite (VMCS_GUEST_RFLAGS, (UINTN)NewRflags);
    }
    AsmVmWrite (VMCS_GUEST_RIP, (UINTN)(GuestRip + InstrLen));
    return;
  }

  if (FaultAddr != 0) {
    UINTN   CsSel;
    UINT32  ErrCode;

    AsmVmRead (VMCS_GUEST_CS_SELECTOR, &CsSel);

    //
    // P=0 (not present).  U/S from the guest's current privilege level, which
    // is the low two bits of CS - a user-mode fault reported as supervisor
    // would send the OS down the wrong path.  W/R set when the access was a
    // store (Why == 7).
    //
    ErrCode = ((CsSel & 3) == 3) ? 0x4 : 0x0;
    if (Why == 7) {
      ErrCode |= 0x2;
    }

    //
    // CR2 is not a VMCS field and is not swapped by VM entry, so the guest
    // reads whatever the host leaves in it; write it here.
    //
    AsmWriteCr2 ((UINTN)FaultAddr);
    InjectException (EXCEPTION_VECTOR_PF, ErrCode, TRUE);
    gHvDiag.InjCr2 = FaultAddr;
#if HV_DIAG_ENABLED
    Pcpu->Count.PfInject++;
#endif
    return;
  }

  RecordUnemulatedUd (GuestRip, (UINT64)GuestCr3, Why);
#if HV_DIAG_ENABLED
  Pcpu->Count.UdFail++;
#endif

  DEBUG ((DEBUG_WARN, "[HV] Un-emulated #UD at RIP=0x%lx (why %u); reinjecting\n",
          (UINT64)GuestRip, Why));
  InjectException (EXCEPTION_VECTOR_UD, 0, FALSE);   // synthesized, not reflected
}

#if HV_DIAG_ENABLED

//
// Attribute an exit to an application processor.  An AP that stops exiting has
// usually faulted, and the vector says whether we handed it something we
// emulate (#UD) or reflected a fault into a processor with no IDT yet.  It
// cannot report this itself: whatever it paints, the BSP's heartbeat erases.
//
STATIC
VOID
RecordApExit (
  IN UINT32  BasicReason
  )
{
  gHvDiag.ApLastReason = BasicReason;

  if (BasicReason == EXIT_REASON_EXCEPTION_NMI) {
    UINTN   Info, Rip;
    UINTN   ErrCode = 0;
    UINT32  Vec;

    AsmVmRead (VMCS_EXIT_INTR_INFO, &Info);
    AsmVmRead (VMCS_GUEST_RIP, &Rip);
    if ((Info & INTR_INFO_ERRCODE_VALID) != 0) {
      AsmVmRead (VMCS_EXIT_INTR_ERRCODE, &ErrCode);
    }
    Vec = (UINT32)(Info & INTR_INFO_VECTOR_MASK);

    gHvDiag.ApLastExcVec = Vec;
    gHvDiag.ApLastExcErr = ErrCode;
    gHvDiag.ApLastExcRip = (UINT64)Rip;

    //
    // Keep the last exception that was not the fatal one.  CR2 is right to read
    // here: VMX neither saves nor restores it, so it still holds whatever
    // address the guest faulted on.
    //
    if (Vec != EXCEPTION_VECTOR_DF) {
      gHvDiag.ApPrevExcVec = Vec;
      gHvDiag.ApPrevExcErr = ErrCode;
      gHvDiag.ApPrevExcCr2 = (UINT64)AsmReadCr2 ();
      gHvDiag.ApPrevExcRip = (UINT64)Rip;
    }
  }
}
#endif


VOID
EFIAPI
HandleVmExit (
  IN OUT GUEST_REGS  *Regs,
  IN OUT VOID        *FxArea
  )
{
  HV_PCPU  *Pcpu = HvPcpu (Regs);
  UINTN     ExitReason, IntrInfo, GuestRip;
  UINT32    BasicReason, Vector;

  AsmVmRead (VMCS_EXIT_REASON, &ExitReason);
  BasicReason = (UINT32)(ExitReason & EXIT_REASON_MASK);

  //
  // The exit count is kept in both builds: it is the cadence for the microcode
  // refresh below, which is not diagnostic.
  //
  Pcpu->Count.Exits++;
#if HV_DIAG_ENABLED
  Pcpu->LastExitReason = BasicReason;
  if (Pcpu->IsBsp) {
    gHvDiag.LastExitReason = BasicReason;   // the heartbeat reads the BSP's
  }
#endif

  //
  // Re-read this processor's microcode revision now and then.  The guest loads
  // its own update well after the processor is running, so the value taken at
  // setup is the BIOS's - not the one Windows compares across processors when
  // it decides whether to bugcheck 0x17E.
  //
  //
  // NOT diagnostic, and so kept in both builds: gHvBspMicroRev is what an
  // application processor is told when it reads IA32_BIOS_SIGN_ID, and getting
  // it wrong is bugcheck 0x17E.  HandleRdmsr keeps it current whenever the
  // bootstrap processor reads that MSR - which Windows does before it compares
  // any other processor - and this is the belt to that pair of braces.  The
  // per-processor copy beside it is only ever displayed.
  //
  if ((Pcpu->Count.Exits & 0x1FFFu) == 0) {
    UINT64  Rev = HvReadMicrocodeRevision ();

    if (Pcpu->IsBsp) {
      gHvBspMicroRev = Rev;
    }
#if HV_DIAG_ENABLED
    Pcpu->MicroRev = Rev;
#endif
  }

#if HV_DIAG_ENABLED
  if (!Pcpu->IsBsp) {
    RecordApExit (BasicReason);
  }

  //
  // Sample the display rarely: painting is far more expensive than an exit, and
  // the numbers are read by eye off a screen, not sampled.  The first few exits
  // paint too, so a machine that dies early still shows something.
  //
  if (Pcpu->IsBsp &&
      ((Pcpu->Count.Exits <= 8) || ((Pcpu->Count.Exits & 0x3FFFu) == 0)))
  {
    HvHeartbeat ();
  }
#endif

  //
  // Keep the IA-32e-mode-guest entry control matched to the guest's current
  // mode before any VMRESUME (covers the AP real -> protected -> long walk).
  //
  SyncEntryControls (Pcpu);

  //
  // Instructions that VM-exit unconditionally in non-root operation and that a
  // Windows guest executes routinely.  These MUST be serviced or the guest
  // hangs immediately (CPUID) or faults (XSETBV).
  //
  switch (BasicReason) {
    case EXIT_REASON_CPUID:
      HandleCpuid (Regs);
      return;

    case EXIT_REASON_CR_ACCESS:
      HandleCrAccess (Regs, Pcpu);
      return;

    case EXIT_REASON_XSETBV:
      HandleXsetbv (Regs);
      return;

    case EXIT_REASON_RDMSR:            // insurance if the MSR bitmap is absent
      HandleRdmsr (Regs, Pcpu);
      return;

    case EXIT_REASON_WRMSR:
      HandleWrmsr (Regs, Pcpu);
      return;

    case EXIT_REASON_INVD:
      AsmWbinvd ();
      AdvanceRip ();
      return;

    case EXIT_REASON_INIT_SIGNAL:
#if HV_DIAG_ENABLED
      Pcpu->Count.Init++;
#endif
      HandleInitSignal (Pcpu);
      return;

    case EXIT_REASON_SIPI:
#if HV_DIAG_ENABLED
      Pcpu->Count.Sipi++;
#endif
      HandleSipi (Regs, Pcpu);
      return;

    case EXIT_REASON_PREEMPT_TIMER:    // periodic sampling tick; just resume
    case EXIT_REASON_MTF:              // single-step tick, if ever enabled
      return;

    default:
      break;
  }

  AsmVmRead (VMCS_GUEST_RIP, &GuestRip);

  if ((ExitReason & EXIT_REASON_ENTRY_FAILURE) != 0) {
    ReportEntryFailure (BasicReason, GuestRip, Pcpu);
    return;
  }

  if (BasicReason != EXIT_REASON_EXCEPTION_NMI) {
    //
    // We enabled only the #UD/#DF/#GP exception bitmap bits and service the
    // unconditional exits above, so nothing else should reach here.  Show the
    // reason and stop, so an unexpected exit is diagnosed rather than silently
    // mishandled.  This paints only on the failure path, so the thousands of
    // routine CR3 exits do not repaint every time.
    //
    UINTN  Efer, EntryCtls, ExitCtls;

    AsmVmRead (VMCS_GUEST_IA32_EFER, &Efer);
    AsmVmRead (VMCS_ENTRY_CTLS,      &EntryCtls);
    AsmVmRead (VMCS_EXIT_CTLS,       &ExitCtls);

    HvMark  (19, 'X');
    HvMarkN (HV_BAND_BASE + 0, Efer);
    HvMarkN (HV_BAND_BASE + 1, EntryCtls);
    HvMarkN (HV_BAND_BASE + 2, ExitCtls);
    HvMarkN (HV_BAND_BASE + 3, GuestModeFlags ());
    HvMarkN (HV_BAND_BASE + 4, (UINTN)BasicReason);
    DEBUG ((DEBUG_ERROR, "[HV] Unexpected VM-exit reason=%lu\n", (UINT64)BasicReason));
    CpuDeadLoop ();
    return;
  }

  AsmVmRead (VMCS_EXIT_INTR_INFO, &IntrInfo);
  Vector = (UINT32)(IntrInfo & INTR_INFO_VECTOR_MASK);

  if (Vector == EXCEPTION_VECTOR_DF) {
    ReportDoubleFault (GuestRip);
    return;
  }


  //
  // #GP: deliver it to the guest exactly as hardware would.
  //
  // This used to freeze on the FIRST #GP, reasoning that the guest - then a
  // virtualized CloverEFI that never showed its UI - had to be broken already.
  // That premise is gone.  The guest is now ntoskrnl, which reaches the boot
  // animation and takes benign #GP(0)s by design: probing MSRs and CPU features
  // inside SEH blocks that KiTrap0D handles and continues past.  Freezing on one
  // of those parks the BSP inside the hypervisor and stops the whole machine -
  // the freeze IS the hang, not evidence of one.
  //
  // So reinject (no RIP advance - the fault must be re-delivered at the faulting
  // instruction) and keep only a breadcrumb.  A genuine fault storm - our
  // emulator corrupting guest state - still stands out as a runaway count on
  // row 6, and a real cascade still stops in ReportDoubleFault above, which
  // reports this RIP as the first fault.
  //
  if (Vector == EXCEPTION_VECTOR_GP) {
#if HV_DIAG_ENABLED
    Pcpu->Count.Gp++;
#endif
    gHvDiag.LastGpRip = (UINT64)GuestRip;

    DEBUG ((DEBUG_WARN, "[HV] #GP at rip=0x%lx; reinjecting\n", (UINT64)GuestRip));
    ReinjectException ((UINT32)IntrInfo);
    return;
  }

  if (Vector != EXCEPTION_VECTOR_UD) {
    ReinjectException ((UINT32)IntrInfo);   // something else slipped through
    return;
  }

  HandleInvalidOpcode (Regs, FxArea, Pcpu, (UINT64)GuestRip);
}
