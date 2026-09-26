/** @file
  RealMode.c - Software interpreter for Application-Processor startup on CPUs
  WITHOUT "unrestricted guest" (e.g. Q6600 / Kentsfield).

  Such a CPU cannot enter a VMX guest that is in real mode OR that has paging
  off: IA32_VMX_CR0_FIXED0 forces guest CR0.PE=1 AND CR0.PG=1.  So when Windows
  SIPIs an AP, we cannot resume it until its trampoline has enabled paging (and,
  in practice, long mode).  This module interprets the trampoline in software -
  through the real-mode prologue AND the flat 32-bit protected-mode setup - and
  hands off to hardware only once CR0.PG is set (at the far jump into the
  paged/long-mode entry).  From there the guest runs natively; the exit handler
  keeps the IA-32e entry control matched to EFER.LMA.

  The interpreter models a straight-line trampoline that uses prebuilt page
  tables (loads CR3 rather than constructing tables): segment/GPR/immediate
  MOVs, MOV to/from CR, the basic ALU group (ADD/OR/AND/SUB/XOR/CMP/TEST), INC/
  DEC, PUSH/POP, LEA, LGDT/LIDT, WRMSR/RDMSR, WBINVD, port I/O (no-ops), and
  near/short/far/conditional jumps with approximate CF/ZF/SF.  An un-modeled
  opcode returns FALSE so the caller parks the core (visibly) rather than
  mis-executing.  Validate/extend against the actual Windows 24H2 trampoline.
**/

#include "Hypervisor.h"

#define CR0_PE  BIT0
#define CR0_PG  BIT31
#define EFER_LME 0x100ULL
#define EFER_LMA 0x400ULL
#define FL_CF   BIT0
#define FL_ZF   BIT6
#define FL_SF   BIT7

typedef struct {
  UINT32   Reg[8];        // EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
  UINT16   SegSel[6];     // ES,CS,SS,DS,FS,GS
  UINT32   SegBase[6];
  UINT32   Eip;
  UINT32   Eflags;
  UINT64   Cr0, Cr3, Cr4, Efer;
  //
  // FS/GS bases are 64-bit and live in the VMCS, not in SegBase[] - a UINT32
  // cannot hold a kernel base anyway.  They are tracked here so a WRMSR during
  // interpretation survives the hand-off; see CommitGuest.
  //
  UINT64   FsBase, GsBase;
  UINT64   GdtrBase; UINT16 GdtrLimit;
  UINT64   IdtrBase; UINT16 IdtrLimit;
  BOOLEAN  Def32;         // current code segment default operand/address size
} REAL_CPU;

enum { S_ES = 0, S_CS = 1, S_SS = 2, S_DS = 3, S_FS = 4, S_GS = 5 };

STATIC UINT8  PhysR8  (IN UINT64 P) { return *(volatile UINT8  *)(UINTN)P; }
STATIC UINT16 PhysR16 (IN UINT64 P) { return *(volatile UINT16 *)(UINTN)P; }
STATIC UINT32 PhysR32 (IN UINT64 P) { return *(volatile UINT32 *)(UINTN)P; }
STATIC VOID   PhysW8  (IN UINT64 P, IN UINT8  V) { *(volatile UINT8  *)(UINTN)P = V; }
STATIC VOID   PhysW16 (IN UINT64 P, IN UINT16 V) { *(volatile UINT16 *)(UINTN)P = V; }
STATIC VOID   PhysW32 (IN UINT64 P, IN UINT32 V) { *(volatile UINT32 *)(UINTN)P = V; }

STATIC
UINT32
GetReg (IN REAL_CPU *C, IN UINT8 Idx, IN UINTN Size)
{
  if (Size == 1) {
    if (Idx < 4) { return C->Reg[Idx] & 0xFF; }
    return (C->Reg[Idx - 4] >> 8) & 0xFF;
  }
  if (Size == 2) { return C->Reg[Idx] & 0xFFFF; }
  return C->Reg[Idx];
}

STATIC
VOID
SetReg (IN REAL_CPU *C, IN UINT8 Idx, IN UINTN Size, IN UINT32 Val)
{
  if (Size == 1) {
    if (Idx < 4) { C->Reg[Idx] = (C->Reg[Idx] & ~0xFFu) | (Val & 0xFF); }
    else         { C->Reg[Idx - 4] = (C->Reg[Idx - 4] & ~0xFF00u) | ((Val & 0xFF) << 8); }
  } else if (Size == 2) {
    C->Reg[Idx] = (C->Reg[Idx] & ~0xFFFFu) | (Val & 0xFFFF);
  } else {
    C->Reg[Idx] = Val;
  }
}

//
// Instruction fetch: linear = CS.base + [E]IP; physical == linear (paging is
// still off while we interpret - we hand off once CR0.PG turns on).
//
STATIC
UINT8
Fetch8 (IN REAL_CPU *C)
{
  UINT8  B;
  if (C->Cr0 & CR0_PE) {
    B = PhysR8 (C->SegBase[S_CS] + C->Eip);
    C->Eip += 1;
  } else {
    B = PhysR8 (C->SegBase[S_CS] + (C->Eip & 0xFFFF));
    C->Eip = (C->Eip + 1) & 0xFFFF;
  }
  return B;
}

STATIC UINT16 Fetch16 (IN REAL_CPU *C) { UINT16 a = Fetch8 (C); a |= (UINT16)Fetch8 (C) << 8; return a; }
STATIC UINT32 Fetch32 (IN REAL_CPU *C) { UINT32 a = Fetch16 (C); a |= (UINT32)Fetch16 (C) << 16; return a; }

typedef struct {
  BOOLEAN  IsReg;
  UINT8    Reg;
  UINT8    RmReg;
  UINT64   Addr;          // segment base + Off: where the operand lives
  UINT32   Off;           // the effective address alone, which is what LEA loads
} MODRM;

STATIC
VOID
DecodeModRm (
  IN REAL_CPU *C, IN BOOLEAN AddrSize32, IN UINT8 SegOverride, OUT MODRM *M
  )
{
  UINT8   ModRm = Fetch8 (C);
  UINT8   Mod   = (UINT8)(ModRm >> 6);
  UINT8   Rm    = (UINT8)(ModRm & 7);
  UINT64  Off   = 0;
  UINT8   Seg   = S_DS;

  M->Reg = (UINT8)((ModRm >> 3) & 7);
  if (Mod == 3) { M->IsReg = TRUE; M->RmReg = Rm; return; }
  M->IsReg = FALSE;

  if (!AddrSize32) {
    UINT16  Bx = (UINT16)C->Reg[3], Bp = (UINT16)C->Reg[5];
    UINT16  Si = (UINT16)C->Reg[6], Di = (UINT16)C->Reg[7];
    switch (Rm) {
      case 0: Off = (UINT16)(Bx + Si); break;
      case 1: Off = (UINT16)(Bx + Di); break;
      case 2: Off = (UINT16)(Bp + Si); Seg = S_SS; break;
      case 3: Off = (UINT16)(Bp + Di); Seg = S_SS; break;
      case 4: Off = Si; break;
      case 5: Off = Di; break;
      case 6: if (Mod == 0) { Off = Fetch16 (C); } else { Off = Bp; Seg = S_SS; } break;
      default: Off = Bx; break;
    }
    if (Mod == 1)      { Off = (UINT16)(Off + (INT8)Fetch8 (C)); }
    else if (Mod == 2) { Off = (UINT16)(Off + Fetch16 (C)); }
    Off &= 0xFFFF;
  } else {
    UINT32  Base = 0;
    if (Rm == 4) {
      UINT8  Sib = Fetch8 (C);
      UINT8  Bidx = (UINT8)(Sib & 7), Iidx = (UINT8)((Sib >> 3) & 7), Scl = (UINT8)(Sib >> 6);
      if (!((Bidx == 5) && (Mod == 0))) { Base += C->Reg[Bidx]; }
      if (Iidx != 4) { Base += C->Reg[Iidx] << Scl; }
    } else if ((Rm == 5) && (Mod == 0)) {
      Base = Fetch32 (C);
    } else {
      Base = C->Reg[Rm];
    }
    if (Mod == 1)      { Base += (UINT32)(INT8)Fetch8 (C); }
    else if (Mod == 2) { Base += Fetch32 (C); }
    Off = Base;
  }

  if (SegOverride != 0xFF) { Seg = SegOverride; }
  M->Off  = (UINT32)Off;
  M->Addr = C->SegBase[Seg] + Off;
}

STATIC
UINT32
ReadRm (IN REAL_CPU *C, IN MODRM *M, IN UINTN Size)
{
  if (M->IsReg) { return GetReg (C, M->RmReg, Size); }
  if (Size == 1) { return PhysR8  (M->Addr); }
  if (Size == 2) { return PhysR16 (M->Addr); }
  return PhysR32 (M->Addr);
}

STATIC
VOID
WriteRm (IN REAL_CPU *C, IN MODRM *M, IN UINTN Size, IN UINT32 Val)
{
  if (M->IsReg) { SetReg (C, M->RmReg, Size, Val); return; }
  if (Size == 1)      { PhysW8  (M->Addr, (UINT8)Val); }
  else if (Size == 2) { PhysW16 (M->Addr, (UINT16)Val); }
  else                { PhysW32 (M->Addr, Val); }
}

//
// ALU with approximate CF/ZF/SF.  Ext: 0 ADD,1 OR,4 AND,5 SUB,6 XOR,7 CMP.
// Returns TRUE if the result should be stored (FALSE for CMP).  ADC/SBB (2/3)
// are not modeled; callers reject them.
//
STATIC
BOOLEAN
Alu (IN REAL_CPU *C, IN UINT8 Ext, IN UINT32 A, IN UINT32 B, IN UINTN Size, OUT UINT32 *Res)
{
  UINT64  R;
  UINT32  Mask = (Size == 1) ? 0xFF : (Size == 2) ? 0xFFFF : 0xFFFFFFFF;
  UINT32  Sign = (Size == 1) ? 0x80 : (Size == 2) ? 0x8000 : 0x80000000;
  BOOLEAN Store = TRUE;

  switch (Ext) {
    case 0: R = (UINT64)A + B; break;
    case 1: R = A | B; break;
    case 4: R = A & B; break;
    case 5: R = (UINT64)A - B; break;
    case 6: R = A ^ B; break;
    case 7: R = (UINT64)A - B; Store = FALSE; break;
    default: *Res = A; return FALSE;
  }
  *Res = (UINT32)R & Mask;

  C->Eflags &= ~(FL_ZF | FL_SF | FL_CF);
  if (*Res == 0)     { C->Eflags |= FL_ZF; }
  if (*Res & Sign)   { C->Eflags |= FL_SF; }
  if (Ext == 0)      { if ((R >> (Size * 8)) & 1) { C->Eflags |= FL_CF; } }
  else if (Ext == 5 || Ext == 7) { if (A < B) { C->Eflags |= FL_CF; } }
  return Store;
}

//
// Shift/rotate group: C0/C1 (imm8), D0/D1 (by 1), D2/D3 (by CL).
//   0=ROL 1=ROR 2=RCL 3=RCR 4=SHL 5=SHR 6=SAL(=SHL) 7=SAR
//
// Absent entirely until now, so Windows' AP trampoline parked the core on a
// plain `shl eax, 4` - which is how a two-core machine came up single-core.
//
// Only the flags a following conditional jump can actually consult are modelled:
// CF from the last bit shifted out, plus ZF/SF from the result.  OF is defined
// only for a count of 1 and nothing in this trampoline reads it.  A count of 0
// leaves the flags untouched, as the hardware does.
//
STATIC
BOOLEAN
ShiftOp (
  IN REAL_CPU *C, IN UINT8 Ext, IN UINT32 A, IN UINT8 Count, IN UINTN Size, OUT UINT32 *Res
  )
{
  UINT32  Mask  = (Size == 1) ? 0xFF : (Size == 2) ? 0xFFFF : 0xFFFFFFFF;
  UINT32  Sign  = (Size == 1) ? 0x80 : (Size == 2) ? 0x8000 : 0x80000000;
  UINT8   N     = (UINT8)(Count & 0x1F);          // masked to 5 bits, as x86 does
  UINT32  V     = A & Mask;
  BOOLEAN Cf    = (C->Eflags & FL_CF) != 0;
  UINTN   I;

  if (N == 0) {
    *Res = V;
    return TRUE;                                   // no flags touched
  }

  for (I = 0; I < N; I++) {
    switch (Ext) {
      case 0:                                      // ROL
        Cf = (V & Sign) != 0;
        V  = ((V << 1) | (Cf ? 1u : 0u)) & Mask;
        break;
      case 1:                                      // ROR
        Cf = (V & 1) != 0;
        V  = ((V >> 1) | (Cf ? Sign : 0u)) & Mask;
        break;
      case 2: {                                    // RCL
        BOOLEAN In = Cf;
        Cf = (V & Sign) != 0;
        V  = ((V << 1) | (In ? 1u : 0u)) & Mask;
        break;
      }
      case 3: {                                    // RCR
        BOOLEAN In = Cf;
        Cf = (V & 1) != 0;
        V  = ((V >> 1) | (In ? Sign : 0u)) & Mask;
        break;
      }
      case 4:
      case 6:                                      // SHL / SAL
        Cf = (V & Sign) != 0;
        V  = (V << 1) & Mask;
        break;
      case 5:                                      // SHR
        Cf = (V & 1) != 0;
        V  = (V >> 1) & Mask;
        break;
      case 7:                                      // SAR - sign-propagating
        Cf = (V & 1) != 0;
        V  = ((V >> 1) | (V & Sign)) & Mask;
        break;
      default:
        *Res = A;
        return FALSE;
    }
  }

  *Res = V;

  //
  // Rotates leave SF/ZF alone; shifts set them from the result.
  //
  C->Eflags &= ~FL_CF;
  if (Cf) { C->Eflags |= FL_CF; }
  if (Ext >= 4) {
    C->Eflags &= ~(FL_ZF | FL_SF);
    if (V == 0)    { C->Eflags |= FL_ZF; }
    if (V & Sign)  { C->Eflags |= FL_SF; }
  }
  return TRUE;
}

STATIC
VOID
ParseDescriptor (
  IN UINT64 GdtBase, IN UINT16 Selector, OUT UINT32 *Base, OUT UINT32 *Limit, OUT UINT32 *ArVmcs
  )
{
  UINT64  D = GdtBase + (Selector & 0xFFF8);
  UINT8   B0 = PhysR8 (D+0), B1 = PhysR8 (D+1), B2 = PhysR8 (D+2), B3 = PhysR8 (D+3);
  UINT8   B4 = PhysR8 (D+4), B5 = PhysR8 (D+5), B6 = PhysR8 (D+6), B7 = PhysR8 (D+7);
  UINT32  Lim = (UINT32)B0 | ((UINT32)B1 << 8) | ((UINT32)(B6 & 0x0F) << 16);

  if (B6 & 0x80) { Lim = (Lim << 12) | 0xFFF; }
  *Base   = (UINT32)B2 | ((UINT32)B3 << 8) | ((UINT32)B4 << 16) | ((UINT32)B7 << 24);
  *Limit  = Lim;
  *ArVmcs = (UINT32)B5 | ((UINT32)(B6 & 0xF0) << 8);
}

STATIC VOID FlatData (IN UINT32 Sel, IN UINT32 SelF, IN UINT32 BaseF, IN UINT32 LimF, IN UINT32 ArF)
{
  AsmVmWrite (SelF, Sel); AsmVmWrite (BaseF, 0);
  AsmVmWrite (LimF, 0xFFFFFFFF); AsmVmWrite (ArF, 0xC093);
}

/**
  Commit the interpreted state at the moment the trampoline enters PROTECTED
  mode, and let hardware run everything after that.

  This is the whole point of the design.  Without unrestricted guest a VMX guest
  must have CR0.PE and CR0.PG set, so an unpaged guest cannot be handed back to
  hardware as-is - which is why this code used to keep interpreting all the way
  to long mode, modelling LGDT, descriptor parsing, CR3, CR4.PAE, WRMSR to EFER
  and a second far jump.  Every one of those is a chance to be silently wrong,
  and being silently wrong there produces a processor that dies much later
  somewhere unrelated, with nothing pointing back.

  Instead, satisfy CR0.PG with OUR identity map and hide it: the guest reads
  CR0 through the read shadow, where PG is exactly what it wrote.  Because the
  map is an identity map, linear equals physical and the guest behaves precisely
  as if paging were off.  Everything from here - PAE, its own CR3, EFER.LME,
  turning paging on for real, the far jump to 64-bit - then executes on the
  hardware, which needs no help being correct.

  What still has to be interpreted is only the real-mode prologue: cli, lgdt,
  a CR0 write setting PE, and this far jump.  That is the part the interpreter
  was always good at.
**/
STATIC
VOID
CommitProtected (
  IN REAL_CPU *C, IN UINT16 JumpSel, IN UINT32 JumpOff,
  IN OUT GUEST_REGS *Regs, IN OUT HV_PCPU *Pcpu
  )
{
  UINT32   CsBase, CsLimit, CsAr;
  UINTN    Ctls;
  BOOLEAN  Shadow;

  ParseDescriptor (C->GdtrBase, JumpSel, &CsBase, &CsLimit, &CsAr);

  //
  // Real CR0 keeps PE and PG (CrApplyFixed would force them anyway); the shadow
  // is the guest's own value, so it still sees paging off.
  //
  //
  // Shadow paging only where the hardware leaves no choice.  With unrestricted
  // guest the processor will run this guest exactly as it is - unpaged, PE just
  // set - and pretending otherwise would be strictly worse: an identity map it
  // does not need, and a CR0 it never asked for.
  //
  Shadow = (BOOLEAN)!VmxUnrestrictedGuestAvailable ();

  // NE regardless: IA32_VMX_CR0_FIXED0 requires it and INIT clears it.
  AsmVmWrite (VMCS_GUEST_CR0,
              (UINTN)(C->Cr0 | CR0_NE_BIT |
                      (Shadow ? (CR0_PE_BIT | CR0_PG_BIT) : 0)));
  AsmVmWrite (VMCS_CR0_READ_SHADOW, (UINTN)C->Cr0);

  //
  // VM entry requires the IA-32e-mode-guest control to equal EFER.LMA in the
  // VMCS - and that field still holds the value saved for the BSP, which is in
  // long mode.  Entering 32-bit protected mode without clearing it is exactly
  // the "invalid guest state" the CPU rejects.
  //
  AsmVmWrite (VMCS_GUEST_IA32_EFER,
              (UINTN)(C->Efer & ~(UINT64)(EFER_LME_BIT | EFER_LMA_BIT)));
  Pcpu->LastEferValid = FALSE;               // written from outside SyncEntryControls

  //
  // PAE is forced with it: the identity map below is in PAE format, and the
  // guest is on its way to enabling PAE regardless.  The shadow hides that too.
  //
  AsmVmWrite (VMCS_GUEST_CR4,
              (UINTN)(C->Cr4 | CR4_VMXE | (Shadow ? CR4_PAE : 0)));
  AsmVmWrite (VMCS_CR4_READ_SHADOW, (UINTN)C->Cr4);
  AsmVmWrite (VMCS_GUEST_CR3,
              (UINTN)(Shadow ? VmxPaeIdentityCr3 () : C->Cr3));

  Pcpu->GuestPaging = (BOOLEAN)(!Shadow && ((C->Cr0 & CR0_PG_BIT) != 0));
  Pcpu->GuestCr3    = C->Cr3;      // whatever it had; applied when it pages
  Pcpu->GuestEfer   = C->Efer;

  AsmVmWrite (VMCS_GUEST_DR7, 0x400);
  AsmVmWrite (VMCS_GUEST_RIP,    JumpOff);
  AsmVmWrite (VMCS_GUEST_RSP,    C->Reg[4]);
  AsmVmWrite (VMCS_GUEST_RFLAGS, (C->Eflags | 0x2) & ~0x200u);

  AsmVmWrite (VMCS_GUEST_GDTR_BASE,  (UINTN)C->GdtrBase);
  AsmVmWrite (VMCS_GUEST_GDTR_LIMIT, C->GdtrLimit);
  AsmVmWrite (VMCS_GUEST_IDTR_BASE,  (UINTN)C->IdtrBase);
  AsmVmWrite (VMCS_GUEST_IDTR_LIMIT, C->IdtrLimit);

  AsmVmWrite (VMCS_GUEST_CS_SELECTOR, JumpSel);
  AsmVmWrite (VMCS_GUEST_CS_BASE,     CsBase);
  AsmVmWrite (VMCS_GUEST_CS_LIMIT,    CsLimit);
  AsmVmWrite (VMCS_GUEST_CS_AR,       CsAr);

  //
  // The trampoline reloads every data segment immediately after this jump, so
  // flat descriptors here match what it is about to install and satisfy the
  // VM-entry checks that the stale real-mode selectors would not.
  //
  FlatData (0x10, VMCS_GUEST_SS_SELECTOR, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR);
  FlatData (0x10, VMCS_GUEST_DS_SELECTOR, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR);
  FlatData (0x10, VMCS_GUEST_ES_SELECTOR, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR);
  FlatData (0x10, VMCS_GUEST_FS_SELECTOR, VMCS_GUEST_FS_BASE, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR);
  FlatData (0x10, VMCS_GUEST_GS_SELECTOR, VMCS_GUEST_GS_BASE, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR);
  AsmVmWrite (VMCS_GUEST_FS_BASE, (UINTN)C->FsBase);
  AsmVmWrite (VMCS_GUEST_GS_BASE, (UINTN)C->GsBase);

  AsmVmWrite (VMCS_GUEST_LDTR_SELECTOR, 0); AsmVmWrite (VMCS_GUEST_LDTR_AR, 0x10000);
  AsmVmWrite (VMCS_GUEST_ACTIVITY_STATE,   0);
  AsmVmWrite (VMCS_GUEST_INTERRUPTIBILITY, 0);

  // 32-bit protected mode: never IA-32e here, whatever LME says.
  AsmVmRead (VMCS_ENTRY_CTLS, &Ctls);
  AsmVmWrite (VMCS_ENTRY_CTLS, Ctls & ~(UINTN)ENTRY_CTL_IA32E_MODE_GUEST);
  Pcpu->EntryCtls = Ctls & ~(UINTN)ENTRY_CTL_IA32E_MODE_GUEST;

  gHvDiag.ApHandoffLong  = FALSE;
  gHvDiag.ApHandoffRip   = JumpOff;
  gHvDiag.ApHandoffRsp   = C->Reg[4];
  gHvDiag.ApHandoffGs    = C->GsBase;
  gHvDiag.ApHandoffCr3   = C->Cr3;

  Regs->Rax = C->Reg[0]; Regs->Rcx = C->Reg[1]; Regs->Rdx = C->Reg[2]; Regs->Rbx = C->Reg[3];
  Regs->Rbp = C->Reg[5]; Regs->Rsi = C->Reg[6]; Regs->Rdi = C->Reg[7];
}

//
// Commit the interpreted state as a paged guest (long mode if EFER.LMA, else
// 32-bit protected+paging) and let hardware resume at the far-jump target.
// Reached only if a trampoline turns on PE and PG in one write; the ordinary
// path stops at CommitProtected above.
//
STATIC
VOID
CommitGuest (
  IN REAL_CPU *C, IN UINT16 JumpSel, IN UINT32 JumpOff, IN OUT GUEST_REGS *Regs
  )
{
  UINT32   CsBase, CsLimit, CsAr;
  BOOLEAN  Long = (BOOLEAN)((C->Efer & EFER_LMA) != 0);
  UINTN    Ctls;

  ParseDescriptor (C->GdtrBase, JumpSel, &CsBase, &CsLimit, &CsAr);

  //
  // Paging on with CR3 zero is not a state a guest can run in: the very next
  // instruction fetch translates through a page table at physical 0.  If the
  // interpreter reaches here like that it never saw the trampoline's CR3 load,
  // and committing it produces a processor that fails somewhere far away with
  // nothing pointing back here.  Say so instead.
  //
  if (((C->Cr0 & CR0_PG) != 0) && (C->Cr3 == 0)) {
    HvMark  (24, 'C');                              // white 24 = CR3 never seen
    HvMarkN (HV_BAND_BASE + 0, (UINTN)(C->Cr0  & 0xFFFFFFFF));
    HvMarkN (HV_BAND_BASE + 1, (UINTN)(C->Cr4  & 0xFFFFFFFF));
    HvMarkN (HV_BAND_BASE + 2, (UINTN)(C->Efer & 0xFFFFFFFF));
    HvMarkN (HV_BAND_BASE + 3, gHvDiag.ApHandoffSteps);
    HvMarkN (HV_BAND_BASE + 4, (UINTN)JumpSel);
    HvMarkN (HV_BAND_BASE + 5, (UINTN)JumpOff);
    HvMarkN (HV_BAND_BASE + 6, (UINTN)(C->GdtrBase & 0xFFFFFFFF));
    HvMarkN (HV_BAND_BASE + 7, (UINTN)(C->SegBase[S_DS] & 0xFFFFFFFF));
    HvMarkN (HV_BAND_BASE + 8, (UINTN)C->Reg[0]);   // EAX: usually the value loaded
    CpuDeadLoop ();
  }

  AsmVmWrite (VMCS_GUEST_CR0, (UINTN)C->Cr0);                 // PE=1, PG=1
  AsmVmWrite (VMCS_GUEST_CR3, (UINTN)C->Cr3);
  AsmVmWrite (VMCS_GUEST_CR4, (UINTN)(C->Cr4 | CR4_VMXE));    // fixed-bit VMXE
  AsmVmWrite (VMCS_GUEST_IA32_EFER, (UINTN)C->Efer);
  AsmVmWrite (VMCS_GUEST_DR7, 0x400);

  AsmVmWrite (VMCS_GUEST_RIP,    JumpOff);
  AsmVmWrite (VMCS_GUEST_RSP,    C->Reg[4]);
  AsmVmWrite (VMCS_GUEST_RFLAGS, (C->Eflags | 0x2) & ~0x200u);  // reserved set, IF clear

  AsmVmWrite (VMCS_GUEST_GDTR_BASE,  (UINTN)C->GdtrBase);
  AsmVmWrite (VMCS_GUEST_GDTR_LIMIT, C->GdtrLimit);
  AsmVmWrite (VMCS_GUEST_IDTR_BASE,  (UINTN)C->IdtrBase);
  AsmVmWrite (VMCS_GUEST_IDTR_LIMIT, C->IdtrLimit);

  AsmVmWrite (VMCS_GUEST_CS_SELECTOR, JumpSel);
  AsmVmWrite (VMCS_GUEST_CS_BASE,     Long ? 0 : CsBase);
  AsmVmWrite (VMCS_GUEST_CS_LIMIT,    CsLimit);
  AsmVmWrite (VMCS_GUEST_CS_AR,       CsAr);

  //
  // Flat data segments.  CS/SS/DS/ES bases really are ignored in 64-bit mode -
  // but FS and GS are NOT.  fs:/gs: addressing uses IA32_FS_BASE/IA32_GS_BASE,
  // which VM entry loads from these VMCS fields, and Windows keeps the
  // per-processor KPCR behind GS.  Writing 0 here handed the kernel a null KPCR
  // on every AP: the first gs:-relative pointer load read address 0, faulted,
  // and the processor was lost while the BSP - which never goes through this
  // interpreter - ran on untouched.  Set the selectors flat, then put the bases
  // back.
  //
  FlatData (0x10, VMCS_GUEST_SS_SELECTOR, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR);
  FlatData (0x10, VMCS_GUEST_DS_SELECTOR, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR);
  FlatData (0x10, VMCS_GUEST_ES_SELECTOR, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR);
  FlatData (0x10, VMCS_GUEST_FS_SELECTOR, VMCS_GUEST_FS_BASE, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR);
  FlatData (0x10, VMCS_GUEST_GS_SELECTOR, VMCS_GUEST_GS_BASE, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR);
  AsmVmWrite (VMCS_GUEST_FS_BASE, (UINTN)C->FsBase);
  AsmVmWrite (VMCS_GUEST_GS_BASE, (UINTN)C->GsBase);

  gHvDiag.ApHandoffLong = Long;
  gHvDiag.ApHandoffRip = JumpOff;
  gHvDiag.ApHandoffRsp = C->Reg[4];
  gHvDiag.ApHandoffGs  = C->GsBase;
  gHvDiag.ApHandoffCr3 = C->Cr3;

  AsmVmWrite (VMCS_GUEST_LDTR_SELECTOR, 0); AsmVmWrite (VMCS_GUEST_LDTR_AR, 0x10000);
  AsmVmWrite (VMCS_GUEST_ACTIVITY_STATE,   0);
  AsmVmWrite (VMCS_GUEST_INTERRUPTIBILITY, 0);
  // TR retains the synthesized busy-TSS descriptor from the initial VMCS setup.

  AsmVmRead (VMCS_ENTRY_CTLS, &Ctls);
  if (Long) { Ctls |= ENTRY_CTL_IA32E_MODE_GUEST; }
  else      { Ctls &= ~(UINTN)ENTRY_CTL_IA32E_MODE_GUEST; }
  AsmVmWrite (VMCS_ENTRY_CTLS, Ctls);

  // Interpreted GPRs -> guest frame (RSP goes via the VMCS above).
  Regs->Rax = C->Reg[0]; Regs->Rcx = C->Reg[1]; Regs->Rdx = C->Reg[2]; Regs->Rbx = C->Reg[3];
  Regs->Rbp = C->Reg[5]; Regs->Rsi = C->Reg[6]; Regs->Rdi = C->Reg[7];
}

//
// The last instruction this interpreter attempted, and how many it had already
// executed.  Every bail-out below is a bare "return FALSE"; the AP then parks
// and paints white 23, which the BSP's heartbeat repaints over within
// milliseconds - so on hardware the failure has always been invisible.  Record
// it here instead and let the BSP publish it.
//
volatile UINT64  gHvRmFailAddr  = 0;   // linear address of that instruction
volatile UINT32  gHvRmFailBytes = 0;   // its first four bytes

BOOLEAN
RealModeStartAp (
  IN UINT8 SipiVector, IN UINT64 GuestCr3, IN OUT GUEST_REGS *Regs,
  IN OUT HV_PCPU *Pcpu
  )
{
  REAL_CPU  Cpu;
  UINTN     Guard;

  (VOID)GuestCr3;

  ZeroMem (&Cpu, sizeof (Cpu));
  Cpu.SegSel[S_CS]  = (UINT16)((UINT16)SipiVector << 8);
  Cpu.SegBase[S_CS] = (UINT32)SipiVector << 12;
  Cpu.Eflags        = 0x2;
  Cpu.Cr0           = 0x60000010ULL;      // INIT value: PE=0, PG=0
  Cpu.GdtrLimit     = 0xFFFF;
  Cpu.IdtrLimit     = 0xFFFF;
  Cpu.Def32         = FALSE;

  for (Guard = 0; Guard < 65536; Guard++) {
    //
    // Snapshot before decoding, so whichever bail-out fires names the
    // instruction that caused it.
    //
    {
      UINT64  Lin = Cpu.SegBase[S_CS] +
                    ((Cpu.Cr0 & CR0_PE) ? Cpu.Eip : (Cpu.Eip & 0xFFFF));

      gHvRmFailAddr  = Lin;
      gHvRmFailBytes = (UINT32)PhysR8 (Lin)               |
                       ((UINT32)PhysR8 (Lin + 1) << 8)    |
                       ((UINT32)PhysR8 (Lin + 2) << 16)   |
                       ((UINT32)PhysR8 (Lin + 3) << 24);
    }

    BOOLEAN  Has66 = FALSE, Has67 = FALSE;
    UINT8    SegOv = 0xFF;
    UINT8    Op;
    BOOLEAN  OpSize32, AddrSize32;
    UINTN    OSize;

    for (;;) {
      Op = Fetch8 (&Cpu);
      if (Op == 0x66) { Has66 = TRUE; continue; }
      if (Op == 0x67) { Has67 = TRUE; continue; }
      if (Op == 0x2E) { SegOv = S_CS; continue; }
      if (Op == 0x36) { SegOv = S_SS; continue; }
      if (Op == 0x3E) { SegOv = S_DS; continue; }
      if (Op == 0x26) { SegOv = S_ES; continue; }
      if (Op == 0x64) { SegOv = S_FS; continue; }
      if (Op == 0x65) { SegOv = S_GS; continue; }
      if (Op == 0xF0 || Op == 0xF2 || Op == 0xF3) { continue; }
      break;
    }

    OpSize32   = (BOOLEAN)(Cpu.Def32 ^ Has66);
    AddrSize32 = (BOOLEAN)(Cpu.Def32 ^ Has67);
    OSize      = OpSize32 ? 4 : 2;

    switch (Op) {
      case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0x90: break;   // CLI/STI/CLD/STD/NOP
      case 0xE4: case 0xE5: case 0xE6: case 0xE7: (VOID)Fetch8 (&Cpu); break;  // IN/OUT imm8
      case 0xEC: case 0xED: case 0xEE: case 0xEF: break;                        // IN/OUT dx

      case 0xB0: case 0xB1: case 0xB2: case 0xB3:
      case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        SetReg (&Cpu, (UINT8)(Op - 0xB0), 1, Fetch8 (&Cpu));
        break;
      case 0xB8: case 0xB9: case 0xBA: case 0xBB:
      case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        SetReg (&Cpu, (UINT8)(Op - 0xB8), OSize, OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu));
        break;

      //
      // The groups below were all absent.  Each one that Windows' AP trampoline
      // happens to use costs a boot to discover, so they are added together
      // rather than one reboot at a time; the failure recorder still names
      // anything that remains.
      //
      case 0xFE: case 0xFF: {                     // Grp4/Grp5: INC/DEC/PUSH/JMP/CALL
        MODRM M; UINTN Sz = (Op == 0xFE) ? 1 : OSize; UINT32 V, R;
        UINT32 Mask = (Sz == 1) ? 0xFF : (Sz == 2) ? 0xFFFF : 0xFFFFFFFF;
        UINT32 Sign = (Sz == 1) ? 0x80 : (Sz == 2) ? 0x8000 : 0x80000000;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if ((Op == 0xFE) && (M.Reg > 1)) { return FALSE; }   // FE defines only /0 and /1
        V = ReadRm (&Cpu, &M, Sz);
        switch (M.Reg) {
          case 0: case 1:                         // INC / DEC (CF unaffected)
            R = ((M.Reg == 0) ? (V + 1) : (V - 1)) & Mask;
            WriteRm (&Cpu, &M, Sz, R);
            Cpu.Eflags &= ~(FL_ZF | FL_SF);
            if (R == 0)   { Cpu.Eflags |= FL_ZF; }
            if (R & Sign) { Cpu.Eflags |= FL_SF; }
            break;
          case 4:                                 // JMP near r/m
            Cpu.Eip = V;
            break;
          case 2:                                 // CALL near r/m
            Cpu.Reg[4] -= (UINT32)OSize;
            if (OSize == 4) { PhysW32 (Cpu.SegBase[S_SS] + Cpu.Reg[4], Cpu.Eip); }
            else            { PhysW16 (Cpu.SegBase[S_SS] + Cpu.Reg[4], (UINT16)Cpu.Eip); }
            Cpu.Eip = V;
            break;
          case 6:                                 // PUSH r/m
            Cpu.Reg[4] -= (UINT32)OSize;
            if (OSize == 4) { PhysW32 (Cpu.SegBase[S_SS] + Cpu.Reg[4], V); }
            else            { PhysW16 (Cpu.SegBase[S_SS] + Cpu.Reg[4], (UINT16)V); }
            break;

          case 3:                                 // CALL far m16:16/32
          case 5: {                               // JMP  far m16:16/32
            //
            // This is how the AP trampoline actually changes mode: the target
            // CS:EIP is loaded from memory rather than encoded in the
            // instruction, so opcode EA never appears and this path is the one
            // that matters.  Same handling as EA once the pair is read.
            //
            UINT32 Off; UINT16 Sel;

            if (M.IsReg) { return FALSE; }        // undefined for a register operand
            if (OSize == 4) { Off = PhysR32 (M.Addr); Sel = PhysR16 (M.Addr + 4); }
            else            { Off = PhysR16 (M.Addr); Sel = PhysR16 (M.Addr + 2); }

            if (M.Reg == 3) {                     // far CALL pushes CS:EIP
              Cpu.Reg[4] -= (UINT32)OSize;
              if (OSize == 4) { PhysW32 (Cpu.SegBase[S_SS] + Cpu.Reg[4], Cpu.SegSel[S_CS]); }
              else            { PhysW16 (Cpu.SegBase[S_SS] + Cpu.Reg[4], Cpu.SegSel[S_CS]); }
              Cpu.Reg[4] -= (UINT32)OSize;
              if (OSize == 4) { PhysW32 (Cpu.SegBase[S_SS] + Cpu.Reg[4], Cpu.Eip); }
              else            { PhysW16 (Cpu.SegBase[S_SS] + Cpu.Reg[4], (UINT16)Cpu.Eip); }
            }

            if (Cpu.Cr0 & CR0_PG) {               // paging on -> hand back to hardware
              gHvDiag.ApHandoffSteps = Guard;
              CommitGuest (&Cpu, Sel, Off, Regs);
              return TRUE;
            }
            if (Cpu.Cr0 & CR0_PE) {
              gHvDiag.ApHandoffSteps = Guard;
              CommitProtected (&Cpu, Sel, Off, Regs, Pcpu);
              return TRUE;                        // hardware takes it from here
            } else {
              Cpu.SegSel[S_CS]  = Sel;
              Cpu.SegBase[S_CS] = (UINT32)Sel << 4;
              Cpu.Eip           = Off & 0xFFFF;
            }
            break;
          }

          default:
            return FALSE;
        }
        break;
      }

      case 0xF6: case 0xF7: {                     // Grp3: TEST/NOT/NEG
        MODRM M; UINTN Sz = (Op == 0xF6) ? 1 : OSize; UINT32 V, R, Imm;
        UINT32 Mask = (Sz == 1) ? 0xFF : (Sz == 2) ? 0xFFFF : 0xFFFFFFFF;
        UINT32 Sign = (Sz == 1) ? 0x80 : (Sz == 2) ? 0x8000 : 0x80000000;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        V = ReadRm (&Cpu, &M, Sz);
        switch (M.Reg) {
          case 0: case 1:                         // TEST r/m, imm
            Imm = (Sz == 1) ? Fetch8 (&Cpu) : (Sz == 2) ? Fetch16 (&Cpu) : Fetch32 (&Cpu);
            R   = V & Imm & Mask;
            Cpu.Eflags &= ~(FL_ZF | FL_SF | FL_CF);
            if (R == 0)   { Cpu.Eflags |= FL_ZF; }
            if (R & Sign) { Cpu.Eflags |= FL_SF; }
            break;
          case 2:                                 // NOT (no flags)
            WriteRm (&Cpu, &M, Sz, (~V) & Mask);
            break;
          case 3:                                 // NEG
            R = ((UINT32)(0 - V)) & Mask;
            WriteRm (&Cpu, &M, Sz, R);
            Cpu.Eflags &= ~(FL_ZF | FL_SF | FL_CF);
            if (R == 0)     { Cpu.Eflags |= FL_ZF; } else { Cpu.Eflags |= FL_CF; }
            if (R & Sign)   { Cpu.Eflags |= FL_SF; }
            break;
          default:
            return FALSE;                         // MUL/IMUL/DIV/IDIV
        }
        break;
      }

      case 0x86: case 0x87: {                     // XCHG r/m, r
        MODRM M; UINTN Sz = (Op == 0x86) ? 1 : OSize; UINT32 A, B;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        A = ReadRm (&Cpu, &M, Sz);
        B = GetReg (&Cpu, M.Reg, Sz);
        WriteRm (&Cpu, &M, Sz, B);
        SetReg (&Cpu, M.Reg, Sz, A);
        break;
      }

      case 0x98:                                  // CBW / CWDE
        if (OSize == 4) { Cpu.Reg[0] = (UINT32)(INT32)(INT16)(UINT16)Cpu.Reg[0]; }
        else            { Cpu.Reg[0] = (Cpu.Reg[0] & 0xFFFF0000u) |
                                       (UINT16)(INT16)(INT8)(UINT8)Cpu.Reg[0]; }
        break;
      case 0x99:                                  // CWD / CDQ
        if (OSize == 4) { Cpu.Reg[2] = ((Cpu.Reg[0] & 0x80000000u) != 0) ? 0xFFFFFFFFu : 0; }
        else            { Cpu.Reg[2] = (Cpu.Reg[2] & 0xFFFF0000u) |
                                       (((Cpu.Reg[0] & 0x8000u) != 0) ? 0xFFFFu : 0u); }
        break;

      case 0xF5: Cpu.Eflags ^= FL_CF; break;      // CMC
      case 0xF8: Cpu.Eflags &= ~FL_CF; break;     // CLC
      case 0xF9: Cpu.Eflags |= FL_CF; break;      // STC

      case 0xC0: case 0xC1:                       // shift/rotate r/m, imm8
      case 0xD0: case 0xD1:                       // shift/rotate r/m, 1
      case 0xD2: case 0xD3: {                     // shift/rotate r/m, CL
        MODRM M; UINTN Sz = (Op & 1) ? OSize : 1; UINT32 R; UINT8 Cnt;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (Op <= 0xC1)      { Cnt = Fetch8 (&Cpu); }
        else if (Op <= 0xD1) { Cnt = 1; }
        else                 { Cnt = (UINT8)(Cpu.Reg[1] & 0xFF); }   // CL
        if (!ShiftOp (&Cpu, M.Reg, ReadRm (&Cpu, &M, Sz), Cnt, Sz, &R)) {
          return FALSE;
        }
        WriteRm (&Cpu, &M, Sz, R);
        break;
      }

      case 0x88: case 0x89: {                       // MOV r/m, r
        MODRM M; UINTN Sz = (Op == 0x88) ? 1 : OSize;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        WriteRm (&Cpu, &M, Sz, GetReg (&Cpu, M.Reg, Sz));
        break;
      }
      case 0x8A: case 0x8B: {                       // MOV r, r/m
        MODRM M; UINTN Sz = (Op == 0x8A) ? 1 : OSize;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        SetReg (&Cpu, M.Reg, Sz, ReadRm (&Cpu, &M, Sz));
        break;
      }
      //
      // Only encodings 0..5 name a segment register.  6 and 7 are invalid, and
      // indexing SegSel/SegBase with them runs off the end of REAL_CPU - the
      // write lands on Eip.  Park the core instead, which is this interpreter's
      // whole contract for something it cannot model.
      //
      case 0x8C: {                                  // MOV r/m16, sreg
        MODRM M; DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (M.Reg > S_GS) { return FALSE; }
        WriteRm (&Cpu, &M, 2, Cpu.SegSel[M.Reg]);
        break;
      }
      case 0x8E: {                                  // MOV sreg, r/m16
        MODRM M; UINT16 V;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (M.Reg > S_GS) { return FALSE; }
        V = (UINT16)ReadRm (&Cpu, &M, 2);
        Cpu.SegSel[M.Reg] = V;
        if (Cpu.Cr0 & CR0_PE) {
          //
          // Protected mode: the base is whatever the descriptor says.  This
          // used to assume 0 - "flat in prot mode" - which is true of the
          // segments a trampoline uses for code and data and false of the one
          // case that matters, a based descriptor loaded into FS or GS to carry
          // a per-processor pointer across the switch to long mode.  Windows
          // keeps the KPCR behind GS; discarding that base hands the kernel a
          // null per-processor block.
          //
          UINT32  Base, Limit, Ar;

          ParseDescriptor (Cpu.GdtrBase, V, &Base, &Limit, &Ar);
          Cpu.SegBase[M.Reg] = Base;
          if (M.Reg == S_FS) { Cpu.FsBase = Base; }
          if (M.Reg == S_GS) { Cpu.GsBase = Base; }
        } else {
          Cpu.SegBase[M.Reg] = (UINT32)V << 4;
        }
        break;
      }
      case 0x8D: {                                  // LEA
        MODRM M; DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (M.IsReg) { return FALSE; }
        //
        // The OFFSET, not the linear address.  This used to load M.Addr, which
        // is correct only once the segment bases are zero - i.e. after the
        // trampoline is in flat protected mode.  Before that, in real mode, DS
        // is a paragraph base and every LEA came out 16*DS too high.
        //
        SetReg (&Cpu, M.Reg, OSize, M.Off);
        break;
      }
      case 0xC6: case 0xC7: {                       // MOV r/m, imm
        MODRM M; UINTN Sz = (Op == 0xC6) ? 1 : OSize;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        WriteRm (&Cpu, &M, Sz, (Sz == 1) ? Fetch8 (&Cpu) : (OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu)));
        break;
      }

      case 0x00: case 0x08: case 0x20: case 0x28:   // ALU r/m, r   (ADD/OR/AND/SUB..)
      case 0x30: case 0x38:
      case 0x01: case 0x09: case 0x21: case 0x29:
      case 0x31: case 0x39: {
        MODRM M; UINTN Sz = (Op & 1) ? OSize : 1; UINT32 R;
        UINT8  Ext = (UINT8)(Op >> 3);              // 0=ADD,1=OR,4=AND,5=SUB,6=XOR,7=CMP
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (Alu (&Cpu, Ext, ReadRm (&Cpu, &M, Sz), GetReg (&Cpu, M.Reg, Sz), Sz, &R)) {
          WriteRm (&Cpu, &M, Sz, R);
        }
        break;
      }
      case 0x02: case 0x0A: case 0x22: case 0x2A:   // ALU r, r/m
      case 0x32: case 0x3A:
      case 0x03: case 0x0B: case 0x23: case 0x2B:
      case 0x33: case 0x3B: {
        MODRM M; UINTN Sz = (Op & 1) ? OSize : 1; UINT32 R;
        UINT8  Ext = (UINT8)(Op >> 3);
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        if (Alu (&Cpu, Ext, GetReg (&Cpu, M.Reg, Sz), ReadRm (&Cpu, &M, Sz), Sz, &R)) {
          SetReg (&Cpu, M.Reg, Sz, R);
        }
        break;
      }
      case 0x04: case 0x0C: case 0x24: case 0x2C:   // ALU AL, imm8
      case 0x34: case 0x3C: {
        UINT32 R; UINT8 Ext = (UINT8)(Op >> 3);
        if (Alu (&Cpu, Ext, GetReg (&Cpu, 0, 1), Fetch8 (&Cpu), 1, &R)) { SetReg (&Cpu, 0, 1, R); }
        break;
      }
      case 0x05: case 0x0D: case 0x25: case 0x2D:   // ALU eAX, imm
      case 0x35: case 0x3D: {
        UINT32 R; UINT8 Ext = (UINT8)(Op >> 3);
        UINT32 Imm = OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu);
        if (Alu (&Cpu, Ext, GetReg (&Cpu, 0, OSize), Imm, OSize, &R)) { SetReg (&Cpu, 0, OSize, R); }
        break;
      }
      case 0x80: case 0x81: case 0x83: {            // Grp1 r/m, imm
        MODRM M; UINTN Sz = (Op == 0x80) ? 1 : OSize; UINT32 Imm, R; UINT8 Ext;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        Ext = M.Reg;
        if (Op == 0x81) { Imm = OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu); }
        else            { Imm = (UINT32)(INT32)(INT8)Fetch8 (&Cpu); if (Op == 0x80) { Imm &= 0xFF; } }
        if (Ext == 2 || Ext == 3) { return FALSE; }   // ADC/SBB unmodeled
        if (Alu (&Cpu, Ext, ReadRm (&Cpu, &M, Sz), Imm, Sz, &R)) { WriteRm (&Cpu, &M, Sz, R); }
        break;
      }
      case 0x84: case 0x85: {                       // TEST r/m, r
        MODRM M; UINTN Sz = (Op == 0x84) ? 1 : OSize; UINT32 R;
        DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
        (VOID)Alu (&Cpu, 4, ReadRm (&Cpu, &M, Sz), GetReg (&Cpu, M.Reg, Sz), Sz, &R);
        Cpu.Eflags &= ~FL_CF;                        // TEST clears CF
        break;
      }
      case 0xA8: {                                  // TEST AL, imm8
        UINT32 R; (VOID)Alu (&Cpu, 4, GetReg (&Cpu, 0, 1), Fetch8 (&Cpu), 1, &R); break;
      }
      case 0xA9: {                                  // TEST eAX, imm
        UINT32 R; (VOID)Alu (&Cpu, 4, GetReg (&Cpu, 0, OSize),
                             OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu), OSize, &R); break;
      }

      case 0x40: case 0x41: case 0x42: case 0x43:   // INC r (32-bit protected)
      case 0x44: case 0x45: case 0x46: case 0x47: {
        UINT32 R; (VOID)Alu (&Cpu, 0, GetReg (&Cpu, (UINT8)(Op - 0x40), OSize), 1, OSize, &R);
        SetReg (&Cpu, (UINT8)(Op - 0x40), OSize, R); break;
      }
      case 0x48: case 0x49: case 0x4A: case 0x4B:   // DEC r
      case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        UINT32 R; (VOID)Alu (&Cpu, 5, GetReg (&Cpu, (UINT8)(Op - 0x48), OSize), 1, OSize, &R);
        SetReg (&Cpu, (UINT8)(Op - 0x48), OSize, R); break;
      }

      case 0x50: case 0x51: case 0x52: case 0x53:   // PUSH r
      case 0x54: case 0x55: case 0x56: case 0x57: {
        UINT32 V = GetReg (&Cpu, (UINT8)(Op - 0x50), OSize);
        Cpu.Reg[4] -= (UINT32)OSize;
        if (OSize == 4) { PhysW32 (Cpu.SegBase[S_SS] + Cpu.Reg[4], V); }
        else            { PhysW16 (Cpu.SegBase[S_SS] + Cpu.Reg[4], (UINT16)V); }
        break;
      }
      case 0x58: case 0x59: case 0x5A: case 0x5B:   // POP r
      case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        UINT32 V = (OSize == 4) ? PhysR32 (Cpu.SegBase[S_SS] + Cpu.Reg[4])
                                : PhysR16 (Cpu.SegBase[S_SS] + Cpu.Reg[4]);
        Cpu.Reg[4] += (UINT32)OSize;
        SetReg (&Cpu, (UINT8)(Op - 0x58), OSize, V);
        break;
      }

      case 0x0F: {
        UINT8 Op2 = Fetch8 (&Cpu);
        switch (Op2) {
          case 0x01: {                              // LGDT (/2) / LIDT (/3)
            MODRM M; DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
            if (M.IsReg) { return FALSE; }
            {
              UINT16 Lim = PhysR16 (M.Addr);
              UINT32 Bs  = PhysR32 (M.Addr + 2);
              if (M.Reg == 2)      { Cpu.GdtrLimit = Lim; Cpu.GdtrBase = Bs; }
              else if (M.Reg == 3) { Cpu.IdtrLimit = Lim; Cpu.IdtrBase = Bs; }
              else { return FALSE; }
            }
            break;
          }
          case 0x20: {                              // MOV r32, CRn
            UINT8 MB = Fetch8 (&Cpu); UINT8 Cr = (UINT8)((MB >> 3) & 7); UINT8 Rm = (UINT8)(MB & 7);
            UINT64 V = (Cr == 0) ? Cpu.Cr0 : (Cr == 3) ? Cpu.Cr3 : (Cr == 4) ? Cpu.Cr4 : 0;
            SetReg (&Cpu, Rm, 4, (UINT32)V);
            break;
          }
          case 0x22: {                              // MOV CRn, r32
            UINT8 MB = Fetch8 (&Cpu); UINT8 Cr = (UINT8)((MB >> 3) & 7); UINT8 Rm = (UINT8)(MB & 7);
            UINT32 V = GetReg (&Cpu, Rm, 4);
            if (Cr == 0) {
              UINT64 New = (Cpu.Cr0 & 0xFFFFFFFF00000000ULL) | V;
              if ((New & CR0_PG) && (Cpu.Efer & EFER_LME)) { Cpu.Efer |= EFER_LMA; }  // activate long mode
              Cpu.Cr0 = New;
            } else if (Cr == 3) { Cpu.Cr3 = V; }
            else if (Cr == 4)   { Cpu.Cr4 = V; }
            break;
          }
          case 0xBA: {                              // Grp8 BT/BTS/BTR/BTC r/m, imm8
            MODRM M; UINT8 Ext; UINT8 Bit; UINT32 A; UINT32 Msk;
            DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
            Ext = M.Reg; Bit = (UINT8)(Fetch8 (&Cpu) & ((OSize == 2) ? 15 : 31));
            if (Ext < 4) { return FALSE; }          // /0../3 are not defined
            A   = ReadRm (&Cpu, &M, OSize);
            Msk = 1u << Bit;
            Cpu.Eflags &= ~FL_CF;
            if (A & Msk) { Cpu.Eflags |= FL_CF; }
            if (Ext == 5)      { WriteRm (&Cpu, &M, OSize, A |  Msk); }   // BTS
            else if (Ext == 6) { WriteRm (&Cpu, &M, OSize, A & ~Msk); }   // BTR
            else if (Ext == 7) { WriteRm (&Cpu, &M, OSize, A ^  Msk); }   // BTC
            break;                                                        // 4 = BT
          }
          case 0xB6: case 0xB7: case 0xBE: case 0xBF: {   // MOVZX / MOVSX
            MODRM M; UINTN Src = ((Op2 & 1) == 0) ? 1 : 2; UINT32 V;
            DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
            V = ReadRm (&Cpu, &M, Src);
            if (Op2 >= 0xBE) {                            // sign-extend
              V = (Src == 1) ? (UINT32)(INT32)(INT8)(UINT8)V
                             : (UINT32)(INT32)(INT16)(UINT16)V;
            }
            SetReg (&Cpu, M.Reg, OSize, V);
            break;
          }
          case 0xA2: {                              // CPUID
            UINT32 Ra, Rb, Rc, Rd;

            AsmCpuidEx (Cpu.Reg[0], Cpu.Reg[1], &Ra, &Rb, &Rc, &Rd);

            //
            // Spoof leaf 1 exactly as HandleCpuid does.
            //
            // This is the one place in the hypervisor that answered CPUID with
            // the processor's real feature bits.  The whole design rests on
            // every processor agreeing that SSE4.1/4.2/POPCNT are present -
            // Windows starts an AP, asks it, and bugchecks 0x5D
            // UNSUPPORTED_PROCESSOR the moment its processors disagree - and an
            // AP executes this interpreter for the whole of its startup
            // trampoline, before hardware takes over.  Any CPUID in there was
            // answered honestly and off-message.
            //
            if (Cpu.Reg[0] == 1) {
              Rc |= (CPUID1_ECX_SSE41 | CPUID1_ECX_SSE42 | CPUID1_ECX_POPCNT);
            }

            Cpu.Reg[0] = Ra;    // EAX
            Cpu.Reg[3] = Rb;    // EBX
            Cpu.Reg[1] = Rc;    // ECX
            Cpu.Reg[2] = Rd;    // EDX
            break;
          }
          case 0x31: {                              // RDTSC
            UINT64 T = AsmReadTsc ();
            Cpu.Reg[0] = (UINT32)T; Cpu.Reg[2] = (UINT32)(T >> 32);
            break;
          }
          case 0xA3: case 0xAB: case 0xB3: {        // BT / BTS / BTR r/m, r
            MODRM M; UINT32 A; UINT32 Bit;
            DecodeModRm (&Cpu, AddrSize32, SegOv, &M);
            Bit = GetReg (&Cpu, M.Reg, OSize);
            //
            // With a MEMORY operand and a register bit offset, the processor
            // addresses outside the addressed word - the bit base moves by
            // offset/operand-size.  Masking the offset, which is right for a
            // register operand, would silently touch the wrong bit.  Park
            // instead; no trampoline seen does this.
            //
            if (!M.IsReg && (Bit >= (UINT32)(OSize * 8))) { return FALSE; }
            Bit &= (OSize == 2) ? 15u : 31u;
            A   = ReadRm (&Cpu, &M, OSize);
            Cpu.Eflags &= ~FL_CF;
            if ((A >> Bit) & 1) { Cpu.Eflags |= FL_CF; }
            if (Op2 == 0xAB)      { WriteRm (&Cpu, &M, OSize, A |  (1u << Bit)); }
            else if (Op2 == 0xB3) { WriteRm (&Cpu, &M, OSize, A & ~(1u << Bit)); }
            break;
          }
          case 0x08: case 0x09: break;              // INVD / WBINVD
          //
          // EFER, IA32_FS_BASE and IA32_GS_BASE are VMCS guest state: VM entry
          // loads them from the VMCS, so writing the real MSR here would be
          // overwritten on the next entry - and would corrupt the HOST's own
          // base in the meantime.  Keep them in the interpreted state and let
          // CommitGuest place them.  Everything else (KERNEL_GS_BASE, PAT,
          // MTRRs, STAR/LSTAR) is not VMCS state and is genuinely the guest's
          // to write, so it goes straight through as the hardware would.
          //
          case 0x30: {                              // WRMSR
            UINT32 Idx = Cpu.Reg[1];
            UINT64 Val = ((UINT64)Cpu.Reg[2] << 32) | Cpu.Reg[0];
            if      (Idx == (UINT32)MSR_IA32_EFER)    { Cpu.Efer   = Val; }
            else if (Idx == (UINT32)MSR_IA32_FS_BASE) { Cpu.FsBase = Val; }
            else if (Idx == (UINT32)MSR_IA32_GS_BASE) { Cpu.GsBase = Val; }
            else                                      { AsmWriteMsr64 (Idx, Val); }
            break;
          }
          case 0x32: {                              // RDMSR
            UINT32 Idx = Cpu.Reg[1];
            UINT64 Val;
            if      (Idx == (UINT32)MSR_IA32_EFER)    { Val = Cpu.Efer; }
            else if (Idx == (UINT32)MSR_IA32_FS_BASE) { Val = Cpu.FsBase; }
            else if (Idx == (UINT32)MSR_IA32_GS_BASE) { Val = Cpu.GsBase; }
            else                                      { Val = AsmReadMsr64 (Idx); }
            Cpu.Reg[0] = (UINT32)Val; Cpu.Reg[2] = (UINT32)(Val >> 32);
            break;
          }
          default:
            if (Op2 >= 0x80 && Op2 <= 0x8F) {       // Jcc rel16/32
              INT32 Rel = OpSize32 ? (INT32)Fetch32 (&Cpu) : (INT32)(INT16)Fetch16 (&Cpu);
              BOOLEAN T = FALSE;
              switch (Op2 & 0x0F) {
                case 0x4: T = (Cpu.Eflags & FL_ZF) != 0; break;    // JZ
                case 0x5: T = (Cpu.Eflags & FL_ZF) == 0; break;    // JNZ
                case 0x2: T = (Cpu.Eflags & FL_CF) != 0; break;    // JB
                case 0x3: T = (Cpu.Eflags & FL_CF) == 0; break;    // JAE
                default: return FALSE;
              }
              if (T) { Cpu.Eip += (UINT32)Rel; }
            } else {
              return FALSE;
            }
            break;
        }
        break;
      }

      case 0x70: case 0x71: case 0x72: case 0x73:   // Jcc rel8
      case 0x74: case 0x75: case 0x76: case 0x77:
      case 0x78: case 0x79: case 0x7A: case 0x7B:
      case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        INT8 Rel = (INT8)Fetch8 (&Cpu); BOOLEAN T = FALSE;
        switch (Op & 0x0F) {
          case 0x4: T = (Cpu.Eflags & FL_ZF) != 0; break;
          case 0x5: T = (Cpu.Eflags & FL_ZF) == 0; break;
          case 0x2: T = (Cpu.Eflags & FL_CF) != 0; break;
          case 0x3: T = (Cpu.Eflags & FL_CF) == 0; break;
          default: return FALSE;
        }
        if (T) { Cpu.Eip += (UINT32)Rel; }
        break;
      }
      case 0xEB: { INT8 Rel = (INT8)Fetch8 (&Cpu); Cpu.Eip += (UINT32)Rel; break; }
      case 0xE9: { INT32 Rel = OpSize32 ? (INT32)Fetch32 (&Cpu) : (INT32)(INT16)Fetch16 (&Cpu);
                   Cpu.Eip += (UINT32)Rel; break; }

      case 0xEA: {                                  // JMP far ptr16:16/32
        UINT32 Off = OpSize32 ? Fetch32 (&Cpu) : Fetch16 (&Cpu);
        UINT16 Sel = Fetch16 (&Cpu);
        if (Cpu.Cr0 & CR0_PG) {                     // paging on -> paged/long guest
          gHvDiag.ApHandoffSteps = Guard;
          CommitGuest (&Cpu, Sel, Off, Regs);
          return TRUE;
        }
        if (Cpu.Cr0 & CR0_PE) {
          gHvDiag.ApHandoffSteps = Guard;
          CommitProtected (&Cpu, Sel, Off, Regs, Pcpu);
          return TRUE;                            // hardware takes it from here
        }
        Cpu.SegSel[S_CS] = Sel; Cpu.SegBase[S_CS] = (UINT32)Sel << 4; Cpu.Eip = Off & 0xFFFF;
        break;
      }

      default:
        return FALSE;                               // un-modeled opcode -> park core
    }
  }

  return FALSE;                                     // ran too long
}
