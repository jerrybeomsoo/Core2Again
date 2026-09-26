/** @file
  SseDecoder.c - Minimal x86-64 decoder + emulator for the two instruction
  families Windows 11 24H2 requires but pre-SSE4.2 CPUs lack:

     POPCNT  : F3 0F B8 /r          (r16/r32/r64, r/m)
     CRC32   : F2 0F 38 F0 /r       (r32/r64, r/m8)
               F2 0F 38 F1 /r       (r32/r64, r/m16/32/64)

  The decoder fetches opcode bytes from guest linear memory (walking the guest
  page tables under the live CR3), parses legacy prefixes / REX / ModRM / SIB /
  displacement, reads the source (register or memory), computes the result,
  writes it into the guest register frame, updates RFLAGS per the Intel SDM, and
  reports the exact instruction length so the exit handler can advance RIP.

  Deliberate scope limits (documented, not hidden):
   * 67h (32-bit address size) is not modeled - long-mode kernels use 64-bit
     addressing, so an address-size override would be rejected as un-emulated.
   * Segment-override prefixes are consumed but ignored (flat long mode: all
     data segments have base 0; FS/GS-relative operands are not expected for
     these opcodes in the Windows kernel path).
**/

#include "Hypervisor.h"

#define RFLAG_CF  BIT0
#define RFLAG_PF  BIT2
#define RFLAG_AF  BIT4
#define RFLAG_ZF  BIT6
#define RFLAG_SF  BIT7
#define RFLAG_OF  BIT11

STATIC
UINT8
ReadReg8 (
  IN GUEST_REGS  *Regs,
  IN UINT8        Index,
  IN BOOLEAN      HasRex
  )
{
  if (!HasRex && (Index >= 4) && (Index <= 7)) {
    return (UINT8)(HvReadGpr (Regs, (UINT8)(Index - 4)) >> 8);
  }
  return (UINT8)HvReadGpr (Regs, Index);
}

//
// Population count.
//
STATIC
UINT64
PopCount (
  IN UINT64  Value,
  IN UINTN   Bytes
  )
{
  UINT64  Mask;
  UINT64  Count;
  UINTN   I;

  Mask  = (Bytes == 8) ? 0xFFFFFFFFFFFFFFFFULL : (((UINT64)1 << (Bytes * 8)) - 1);
  Value &= Mask;

  Count = 0;
  for (I = 0; I < Bytes * 8; I++) {
    Count += (Value >> I) & 1;
  }
  return Count;
}

//
// One byte of reflected CRC-32C (Castagnoli, polynomial 0x11EDC6F41,
// reversed 0x82F63B78) - the algorithm the SSE4.2 CRC32 instruction implements.
//
STATIC
UINT32
Crc32cByte (
  IN UINT32  Crc,
  IN UINT8   Data
  )
{
  UINTN  I;

  Crc ^= Data;
  for (I = 0; I < 8; I++) {
    UINT32  Mask = (UINT32)(-(INT32)(Crc & 1));
    Crc = (Crc >> 1) ^ (0x82F63B78 & Mask);
  }
  return Crc;
}

STATIC
UINT32
Read32Le (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

//
// XMM register access into the guest FXSAVE image (XMMn at FX_XMM0_OFFSET+n*16).
//
STATIC
UINT8 *
XmmPtr (
  IN VOID   *FxArea,
  IN UINT8   Index
  )
{
  return (UINT8 *)FxArea + FX_XMM0_OFFSET + (UINTN)Index * 16;
}

//
// SSE4.1 opcode predicates (all use the 66 mandatory prefix).
//
STATIC
BOOLEAN
SseIsSse41_38 (
  IN UINT8  Op2
  )
{
  if ((Op2 >= 0x20 && Op2 <= 0x25) ||   // PMOVSX*
      (Op2 >= 0x30 && Op2 <= 0x35) ||   // PMOVZX*
      (Op2 >= 0x38 && Op2 <= 0x3F)) {   // PMIN/PMAX(SB/SD/UW/UD)
    return TRUE;
  }
  switch (Op2) {
    case 0x10: case 0x14: case 0x15:    // PBLENDVB / BLENDVPS / BLENDVPD
    case 0x17:                          // PTEST
    case 0x28: case 0x29: case 0x2A: case 0x2B: // PMULDQ/PCMPEQQ/MOVNTDQA/PACKUSDW
    case 0x40: case 0x41:               // PMULLD / PHMINPOSUW
      return TRUE;
    default:
      return FALSE;
  }
}

STATIC
BOOLEAN
SseIsSse41_3A (
  IN UINT8  Op2
  )
{
  switch (Op2) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: // ROUNDPS/PD/SS/SD
    case 0x0E:                                  // PBLENDW
    case 0x14: case 0x15: case 0x16: case 0x17: // PEXTRB/W/D/Q, EXTRACTPS
    case 0x20: case 0x21: case 0x22:            // PINSRB, INSERTPS, PINSRD/Q
    case 0x40: case 0x41: case 0x42:            // DPPS, DPPD, MPSADBW
      return TRUE;
    default:
      return FALSE;
  }
}

/**
  Try to decode + emulate the instruction at GuestRip.  Returns TRUE and sets
  *InstrLen on success; FALSE (opcode not one of ours / memory fetch failed)
  otherwise.
**/
BOOLEAN
SseTryEmulate (
  IN OUT GUEST_REGS  *Regs,
  IN OUT VOID        *FxArea,
  IN     UINT64       GuestRip,
  IN     UINT64       GuestCr3,
  IN OUT UINT64      *Rflags,
  OUT    UINT32      *InstrLen,
  OUT    UINT64      *FaultAddr,
  OUT    UINT32      *Why
  )
{
  UINT8    Buf[16];
  UINTN    Fetched;
  //
  // Set when emulation fails because a memory OPERAND could not be read.  That
  // is not an unknown instruction and must not be reported as one: #UD is
  // raised before the operand is evaluated, so the operand's page may simply be
  // paged out, and the guest is owed a #PF at that address instead.
  //
  UINTN    P;
  BOOLEAN  Pre66, Pre67, PreF2, PreF3;
  UINT8    Rex;
  BOOLEAN  HasRex, RexW, RexR, RexX, RexB;
  UINT8    Op, Op2;
  UINT8    ModRm, Mod, RegF, RmF;
  UINT8    DestReg;
  BOOLEAN  IsMem;
  BOOLEAN  RipRel;
  UINT8    SrcRegIdx;
  UINT64   MemEa;
  INT64    Disp;
  UINT8    StrImm;

  //
  // Fetch the instruction bytes - page-aware and best-effort, deliberately not
  // an all-or-nothing 16-byte read.
  //
  // An x86 instruction is at most 15 bytes, so a 16-byte window opened near the
  // end of a page reaches into the next one, which may legitimately be absent
  // (end of a code section, or simply paged out).  Failing the whole fetch
  // there made us report "not one of ours" and reinject #UD into a guest whose
  // instruction was perfectly valid - a silent application crash, and by
  // placement alone it waits for roughly one instruction in 250.
  //
  // Bytes we cannot read are bytes this instruction cannot contain: the CPU
  // already fetched and decoded it (that is *why* we got #UD rather than #PF),
  // so every byte up to its end is present by construction.  Read what this
  // page holds, try the next page for the remainder, and let the decoder run on
  // what came back - then confirm at each exit that it never walked past it.
  //
  if (FaultAddr != NULL) {
    *FaultAddr = 0;
  }
  //
  // Why emulation failed, so a failure on hardware names its own cause instead
  // of only its opcode bytes.  1 = could not fetch the instruction, 2 = 32-bit
  // address size, 3 = opcode not recognised (or its feature not emulated),
  // 5 = decoded past the bytes we could read, 6 = internal invariant,
  // 7 = a memory DESTINATION could not be written (FaultAddr is set too).
  // Starts at 3 and is cleared once the opcode has been identified.
  //
  if (Why != NULL) {
    *Why = 3;
  }

  ZeroMem (Buf, sizeof (Buf));
  Fetched = SIZE_4KB - (UINTN)(GuestRip & (SIZE_4KB - 1));
  if (Fetched > sizeof (Buf)) {
    Fetched = sizeof (Buf);
  }
  if (!GuestReadLinear (GuestCr3, GuestRip, Buf, Fetched)) {
    if (Why != NULL) { *Why = 1; }
    return FALSE;
  }
  if ((Fetched < sizeof (Buf)) &&
      GuestReadLinear (GuestCr3, GuestRip + Fetched, &Buf[Fetched],
                       sizeof (Buf) - Fetched))
  {
    Fetched = sizeof (Buf);
  }

  //
  // Legacy prefixes (mandatory-prefix bytes F2/F3/66 plus 67h and the ones we
  // simply skip).
  //
  P = 0;
  Pre66 = Pre67 = PreF2 = PreF3 = FALSE;
  for (;;) {
    UINT8  B = Buf[P];
    if (B == 0x66) { Pre66 = TRUE; P++; continue; }
    if (B == 0x67) { Pre67 = TRUE; P++; continue; }
    if (B == 0xF2) { PreF2 = TRUE; P++; continue; }
    if (B == 0xF3) { PreF3 = TRUE; P++; continue; }
    if ((B == 0xF0) ||                                  // LOCK
        (B == 0x2E) || (B == 0x36) || (B == 0x3E) ||    // CS/SS/DS override
        (B == 0x26) || (B == 0x64) || (B == 0x65)) {    // ES/FS/GS override
      P++;
      continue;
    }
    break;
  }

  if (Pre67) {
    if (Why != NULL) { *Why = 2; }
    return FALSE;   // 32-bit address size not modeled
  }

  //
  // REX (must be the byte immediately before the opcode).
  //
  Rex = 0;
  HasRex = FALSE;
  if ((Buf[P] & 0xF0) == 0x40) {
    Rex    = Buf[P];
    HasRex = TRUE;
    P++;
  }
  RexW = (Rex & 0x8) != 0;
  RexR = (Rex & 0x4) != 0;
  RexX = (Rex & 0x2) != 0;
  RexB = (Rex & 0x1) != 0;

  //
  // Two/three-byte opcode escape.
  //
  if (Buf[P] != 0x0F) {
    return FALSE;
  }
  P++;
  Op  = Buf[P];
  Op2 = 0;

  //
  // Classify the full SSE4.2 opcode set:
  //   POPCNT     F3 0F B8            (scalar)
  //   CRC32      F2 0F 38 F0/F1      (scalar)
  //   PCMPGTQ    66 0F 38 37         (packed signed qword >)
  //   PCMP*STR*  66 0F 3A 60..63 ib  (string/compare, XMM)
  //
  BOOLEAN  IsPopcnt  = FALSE;
  BOOLEAN  IsCrc32   = FALSE;
  BOOLEAN  IsPcmpGtq = FALSE;
  BOOLEAN  IsPcmpStr = FALSE;
  BOOLEAN  IsSse41   = FALSE;
  UINTN    SrcBytes  = 0;

  if (PreF3 && (Op == 0xB8)) {
    if (!HvEmulatePopcnt ()) { return FALSE; }
    IsPopcnt = TRUE;
    P++;                       // consume B8
  } else if (Op == 0x38) {
    P++;                       // consume 38
    Op2 = Buf[P];
    P++;                       // consume the third opcode byte
    if (PreF2 && (Op2 == 0xF0)) {
      if (!HvEmulateSse42 ()) { return FALSE; }
      IsCrc32  = TRUE;
      SrcBytes = 1;
    } else if (PreF2 && (Op2 == 0xF1)) {
      if (!HvEmulateSse42 ()) { return FALSE; }
      IsCrc32  = TRUE;
      SrcBytes = RexW ? 8 : (Pre66 ? 2 : 4);
    } else if (Pre66 && (Op2 == 0x37)) {
      if (!HvEmulateSse42 ()) { return FALSE; }
      IsPcmpGtq = TRUE;
    } else if (Pre66 && SseIsSse41_38 (Op2)) {
      if (!HvEmulateSse41 ()) { return FALSE; }
      IsSse41 = TRUE;
    } else {
      return FALSE;
    }
  } else if (Pre66 && (Op == 0x3A)) {
    P++;                       // consume 3A
    Op2 = Buf[P];
    P++;                       // consume the third opcode byte
    if (Op2 >= 0x60 && Op2 <= 0x63) {
      if (!HvEmulateSse42 ()) { return FALSE; }
      IsPcmpStr = TRUE;        // 60=ESTRM 61=ESTRI 62=ISTRM 63=ISTRI
    } else if (SseIsSse41_3A (Op2)) {
      if (!HvEmulateSse41 ()) { return FALSE; }
      IsSse41 = TRUE;
    } else {
      return FALSE;
    }
  } else {
    return FALSE;
  }

  //
  // The opcode is one of ours from here on; any later failure has a specific
  // cause and sets its own code.
  //
  if (Why != NULL) {
    *Why = 0;
  }

  //
  // ModRM.
  //
  ModRm = Buf[P];
  P++;
  Mod  = (UINT8)(ModRm >> 6);
  RegF = (UINT8)((ModRm >> 3) & 7);
  RmF  = (UINT8)(ModRm & 7);
  DestReg = (UINT8)(RegF | (RexR ? 8 : 0));

  IsMem     = (Mod != 3);
  RipRel    = FALSE;
  SrcRegIdx = 0;
  MemEa     = 0;
  Disp      = 0;

  if (!IsMem) {
    SrcRegIdx = (UINT8)(RmF | (RexB ? 8 : 0));
  } else {
    BOOLEAN  HaveSib = (RmF == 4);
    BOOLEAN  NoBase  = FALSE;
    UINT8    BaseIdx  = 0xFF;
    UINT8    IndexIdx = 0xFF;
    UINT8    Scale    = 0;

    if (HaveSib) {
      UINT8  Sib   = Buf[P];
      UINT8  IdxF  = (UINT8)((Sib >> 3) & 7);
      UINT8  BaseF = (UINT8)(Sib & 7);
      P++;
      Scale = (UINT8)(Sib >> 6);
      if (!((IdxF == 4) && !RexX)) {
        IndexIdx = (UINT8)(IdxF | (RexX ? 8 : 0));
      }
      if ((BaseF == 5) && (Mod == 0)) {
        NoBase = TRUE;
      } else {
        BaseIdx = (UINT8)(BaseF | (RexB ? 8 : 0));
      }
    } else if ((Mod == 0) && (RmF == 5)) {
      RipRel = TRUE;
    } else {
      BaseIdx = (UINT8)(RmF | (RexB ? 8 : 0));
    }

    //
    // Displacement.
    //
    if (RipRel) {
      Disp = (INT64)(INT32)Read32Le (&Buf[P]);
      P += 4;
    } else if (HaveSib && NoBase) {
      Disp = (INT64)(INT32)Read32Le (&Buf[P]);
      P += 4;
    } else if (Mod == 1) {
      Disp = (INT64)(INT8)Buf[P];
      P += 1;
    } else if (Mod == 2) {
      Disp = (INT64)(INT32)Read32Le (&Buf[P]);
      P += 4;
    }

    if (!RipRel) {
      UINT64  Addr = 0;
      if (BaseIdx != 0xFF) {
        Addr += HvReadGpr (Regs, BaseIdx);
      }
      if (IndexIdx != 0xFF) {
        Addr += HvReadGpr (Regs, IndexIdx) << Scale;
      }
      Addr += (UINT64)Disp;
      MemEa = Addr;
    }
    // RIP-relative EA is resolved after P (the full length) is known.
  }

  //
  // Instructions with a trailing imm8 (PCMP*STR* and every 66 0F 3A SSE4.1 op)
  // consume it here, so P reflects the real length before RIP-relative resolve.
  //
  StrImm = 0;
  if (IsPcmpStr || (IsSse41 && (Op == 0x3A))) {
    StrImm = Buf[P];
    P++;
  }

  //
  // At this point P == full instruction length.
  //
  // Confirm it against what we could actually READ before emulating anything.
  // The 16-byte fetch window can be truncated at a page boundary whose next
  // page is absent, in which case the decoder has been reading zeroes past
  // Fetched - and every byte it consumed beyond that is invented.  This used to
  // be checked at each of the three exits below, i.e. AFTER the instruction had
  // been emulated and its results written into the guest's registers and XMM
  // state, which then returned FALSE and had the caller inject #UD on top.  A
  // guest cannot recover from a fault delivered over a half-applied
  // instruction; bail out here, while nothing has been touched.
  //
  if (P > Fetched) {
    if (Why != NULL) { *Why = 5; }
    return FALSE;
  }

  //
  // Resolve a RIP-relative EA, which needs the final length.
  //
  if (IsMem && RipRel) {
    MemEa = GuestRip + P + (UINT64)Disp;
  }

  //
  // ------------------------------------------------------------------------
  // XMM SSE4.2 instructions (PCMPGTQ, PCMP*STR*).  Operand1 is xmm[DestReg];
  // operand2 is xmm[SrcRegIdx] or a 16-byte memory operand.
  // ------------------------------------------------------------------------
  //
  if (IsPcmpGtq || IsPcmpStr) {
    UINT8  Op2Data[16];

    if (IsMem) {
      if (!GuestReadLinear (GuestCr3, MemEa, Op2Data, 16)) {
        if (FaultAddr != NULL) {
          *FaultAddr = MemEa;
        }
        return FALSE;
      }
    } else {
      CopyMem (Op2Data, XmmPtr (FxArea, SrcRegIdx), 16);
    }

    if (IsPcmpGtq) {
      SseXmmPcmpGtq (XmmPtr (FxArea, DestReg), Op2Data);
    } else {
      UINT8    Op1Data[16];
      BOOLEAN  Explicit  = (BOOLEAN)((Op2 & 0x02) == 0);   // 60/61 explicit, 62/63 implicit
      BOOLEAN  IndexForm = (BOOLEAN)((Op2 & 0x01) != 0);   // 61/63 index, 60/62 mask
      UINT32   OutIndex  = 0;
      UINT8    OutXmm0[16];

      // Explicit lengths live in EAX/EDX, or RAX/RDX when REX.W is set.
      INT64  LenA = RexW ? (INT64)HvReadGpr (Regs, 0)
                         : (INT64)(INT32)(UINT32)HvReadGpr (Regs, 0);
      INT64  LenB = RexW ? (INT64)HvReadGpr (Regs, 2)
                         : (INT64)(INT32)(UINT32)HvReadGpr (Regs, 2);

      CopyMem (Op1Data, XmmPtr (FxArea, DestReg), 16);
      SetMem (OutXmm0, 16, 0);

      SseXmmPcmpStr (
        Op1Data, Op2Data, StrImm, Explicit,
        LenA, LenB, IndexForm, &OutIndex, OutXmm0, Rflags);

      if (IndexForm) {
        HvWriteGpr (Regs, 1, (UINT64)OutIndex);   // ECX, zero-extended into RCX
      } else {
        CopyMem (XmmPtr (FxArea, 0), OutXmm0, 16);   // XMM0
      }
    }

    *InstrLen = (UINT32)P;
    return TRUE;
  }

  //
  // ------------------------------------------------------------------------
  // SSE4.1 instructions.
  // ------------------------------------------------------------------------
  //
  if (IsSse41) {
    UINT8  *RegXmm = XmmPtr (FxArea, DestReg);   // the reg-field XMM
    UINT8   SrcData[16];

    ZeroMem (SrcData, sizeof (SrcData));

    if (Op == 0x38) {
      BOOLEAN  IsPmov = (BOOLEAN)(((Op2 >= 0x20) && (Op2 <= 0x25)) ||
                                  ((Op2 >= 0x30) && (Op2 <= 0x35)));
      if (IsPmov) {
        BOOLEAN  Sign = (BOOLEAN)(Op2 < 0x30);
        UINTN    Idx  = Op2 & 0x0F;
        UINTN    Sb, Db, MemSz;

        switch (Idx) {
          case 0: Sb = 1; Db = 2; break;
          case 1: Sb = 1; Db = 4; break;
          case 2: Sb = 1; Db = 8; break;
          case 3: Sb = 2; Db = 4; break;
          case 4: Sb = 2; Db = 8; break;
          default: Sb = 4; Db = 8; break;
        }
        MemSz = (16 / Db) * Sb;
        if (IsMem) {
          if (!GuestReadLinear (GuestCr3, MemEa, SrcData, MemSz)) {
            if (FaultAddr != NULL) { *FaultAddr = MemEa; }
            return FALSE;
          }
        } else {
          CopyMem (SrcData, XmmPtr (FxArea, SrcRegIdx), 16);
        }
        Sse41Pmovx (RegXmm, SrcData, Sb, Db, Sign);
      } else {
        if (IsMem) {
          if (!GuestReadLinear (GuestCr3, MemEa, SrcData, 16)) {
            if (FaultAddr != NULL) { *FaultAddr = MemEa; }
            return FALSE;
          }
        } else {
          CopyMem (SrcData, XmmPtr (FxArea, SrcRegIdx), 16);
        }
        switch (Op2) {
          case 0x29: Sse41Pcmpeqq   (RegXmm, SrcData); break;
          case 0x40: Sse41Pmulld    (RegXmm, SrcData); break;
          case 0x28: Sse41Pmuldq    (RegXmm, SrcData); break;
          case 0x2B: Sse41Packusdw  (RegXmm, SrcData); break;
          case 0x38: Sse41MinMax (RegXmm, SrcData, 1, TRUE,  FALSE); break; // PMINSB
          case 0x39: Sse41MinMax (RegXmm, SrcData, 4, TRUE,  FALSE); break; // PMINSD
          case 0x3A: Sse41MinMax (RegXmm, SrcData, 2, FALSE, FALSE); break; // PMINUW
          case 0x3B: Sse41MinMax (RegXmm, SrcData, 4, FALSE, FALSE); break; // PMINUD
          case 0x3C: Sse41MinMax (RegXmm, SrcData, 1, TRUE,  TRUE);  break; // PMAXSB
          case 0x3D: Sse41MinMax (RegXmm, SrcData, 4, TRUE,  TRUE);  break; // PMAXSD
          case 0x3E: Sse41MinMax (RegXmm, SrcData, 2, FALSE, TRUE);  break; // PMAXUW
          case 0x3F: Sse41MinMax (RegXmm, SrcData, 4, FALSE, TRUE);  break; // PMAXUD
          case 0x41: Sse41Phminposuw (RegXmm, SrcData); break;
          case 0x17: Sse41Ptest (RegXmm, SrcData, Rflags); break;          // no writeback
          case 0x2A: CopyMem (RegXmm, SrcData, 16); break;                 // MOVNTDQA
          case 0x10: Sse41Blendv (RegXmm, SrcData, XmmPtr (FxArea, 0), 1); break; // PBLENDVB
          case 0x14: Sse41Blendv (RegXmm, SrcData, XmmPtr (FxArea, 0), 4); break; // BLENDVPS
          case 0x15: Sse41Blendv (RegXmm, SrcData, XmmPtr (FxArea, 0), 8); break; // BLENDVPD
          default: return FALSE;
        }
      }
    } else {   // Op == 0x3A
      if ((Op2 == 0x20) || (Op2 == 0x22)) {
        //
        // PINSRB / PINSRD/Q: dst = RegXmm, src = r/m (GPR or memory).
        //
        UINTN   Eb       = (Op2 == 0x20) ? 1 : (RexW ? 8 : 4);
        UINTN   NumElems = 16 / Eb;
        UINTN   Index    = StrImm & (NumElems - 1);
        UINT64  Val;

        if (IsMem) {
          UINT8  Tmp[8];
          UINTN  K;
          ZeroMem (Tmp, sizeof (Tmp));
          if (!GuestReadLinear (GuestCr3, MemEa, Tmp, Eb)) {
            if (FaultAddr != NULL) { *FaultAddr = MemEa; }
            return FALSE;
          }
          Val = 0;
          for (K = 0; K < Eb; K++) { Val |= (UINT64)Tmp[K] << (8 * K); }
        } else {
          Val = HvReadGpr (Regs, SrcRegIdx);
        }
        if (Eb < 8) { Val &= (((UINT64)1 << (Eb * 8)) - 1); }
        Sse41SetElem (RegXmm, Index, Eb, Val);

      } else if ((Op2 >= 0x14) && (Op2 <= 0x17)) {
        //
        // PEXTRB/W/D/Q, EXTRACTPS: dst = r/m (GPR or memory), src = RegXmm.
        //
        UINTN   Eb;
        UINTN   NumElems, Index;
        UINT64  Val;

        switch (Op2) {
          case 0x14: Eb = 1; break;              // PEXTRB
          case 0x15: Eb = 2; break;              // PEXTRW
          case 0x16: Eb = RexW ? 8 : 4; break;   // PEXTRD/Q
          default:   Eb = 4; break;              // EXTRACTPS
        }
        NumElems = 16 / Eb;
        Index    = StrImm & (NumElems - 1);
        Val      = Sse41GetElem (RegXmm, Index, Eb);

        if (IsMem) {
          UINT8  Tmp[8];
          UINTN  K;
          for (K = 0; K < Eb; K++) { Tmp[K] = (UINT8)(Val >> (8 * K)); }
          if (!GuestWriteLinear (GuestCr3, MemEa, Tmp, Eb)) {
            //
            // A destination that cannot be written owes the guest a #PF just as
            // an unreadable source does - but with W/R set, or the OS services
            // it as a read and the store faults again forever.  Why = 7 tells
            // the caller which it was.
            //
            if (FaultAddr != NULL) { *FaultAddr = MemEa; }
            if (Why != NULL)       { *Why = 7; }
            return FALSE;
          }
        } else {
          HvWriteGpr (Regs, SrcRegIdx, Val);       // zero-extended into the GPR
        }

      } else {
        //
        // Category B: rm = xmm/m128 (some with a sub-128 memory operand), imm8.
        //
        if (IsMem) {
          if (Op2 == 0x21) {                              // INSERTPS m32
            UINTN  Pos = (StrImm >> 6) & 3;
            if (!GuestReadLinear (GuestCr3, MemEa, &SrcData[Pos * 4], 4)) {
              if (FaultAddr != NULL) { *FaultAddr = MemEa; }
              return FALSE;
            }
          } else if (Op2 == 0x0A) {                       // ROUNDSS m32
            if (!GuestReadLinear (GuestCr3, MemEa, SrcData, 4)) {
              if (FaultAddr != NULL) { *FaultAddr = MemEa; }
              return FALSE;
            }
          } else if (Op2 == 0x0B) {                       // ROUNDSD m64
            if (!GuestReadLinear (GuestCr3, MemEa, SrcData, 8)) {
              if (FaultAddr != NULL) { *FaultAddr = MemEa; }
              return FALSE;
            }
          } else {
            if (!GuestReadLinear (GuestCr3, MemEa, SrcData, 16)) {
              if (FaultAddr != NULL) { *FaultAddr = MemEa; }
              return FALSE;
            }
          }
        } else {
          CopyMem (SrcData, XmmPtr (FxArea, SrcRegIdx), 16);
        }

        switch (Op2) {
          case 0x0E: Sse41Pblendw (RegXmm, SrcData, StrImm); break;
          case 0x08: Sse41Round (RegXmm, SrcData, StrImm, 4, FALSE); break; // ROUNDPS
          case 0x09: Sse41Round (RegXmm, SrcData, StrImm, 8, FALSE); break; // ROUNDPD
          case 0x0A: Sse41Round (RegXmm, SrcData, StrImm, 4, TRUE);  break; // ROUNDSS
          case 0x0B: Sse41Round (RegXmm, SrcData, StrImm, 8, TRUE);  break; // ROUNDSD
          case 0x40: Sse41Dpps    (RegXmm, SrcData, StrImm); break;
          case 0x41: Sse41Dppd    (RegXmm, SrcData, StrImm); break;
          case 0x42: Sse41Mpsadbw (RegXmm, SrcData, StrImm); break;
          case 0x21: Sse41Insertps (RegXmm, SrcData, StrImm); break;
          default: return FALSE;
        }
      }
    }

    *InstrLen = (UINT32)P;
    return TRUE;
  }

  //
  // Only the two scalar GPR forms can still be live here: POPCNT and CRC32.
  // Every packed/XMM class (PCMPGTQ, PCMP*STR*, SSE4.1) returned above.  State
  // that invariant as a check rather than a comment, so a future opcode class
  // added without its own return path bails out instead of falling into the
  // CRC32 tail below.
  //
  if (!IsPopcnt && !IsCrc32) {
    if (Why != NULL) { *Why = 6; }
    return FALSE;
  }

  //
  // Determine the source operand size.
  //
  UINTN  OpBytes;
  if (IsPopcnt) {
    OpBytes  = RexW ? 8 : (Pre66 ? 2 : 4);
    SrcBytes = OpBytes;
  } else {
    OpBytes = 0;   // unused for CRC32 (dest accumulator is always 32-bit)
  }

  //
  // Fetch the source value (little-endian) into a 64-bit holder.
  //
  UINT64  SrcVal = 0;

  if (IsMem) {
    UINT8  Tmp[8];
    UINTN  I;
    ZeroMem (Tmp, sizeof (Tmp));
    if (!GuestReadLinear (GuestCr3, MemEa, Tmp, SrcBytes)) {
      if (FaultAddr != NULL) { *FaultAddr = MemEa; }
      return FALSE;
    }
    for (I = 0; I < SrcBytes; I++) {
      SrcVal |= (UINT64)Tmp[I] << (8 * I);
    }
  } else {
    if (SrcBytes == 1) {
      SrcVal = ReadReg8 (Regs, SrcRegIdx, HasRex);
    } else {
      UINT64  Full = HvReadGpr (Regs, SrcRegIdx);
      UINT64  M    = (SrcBytes == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                     : (((UINT64)1 << (SrcBytes * 8)) - 1);
      SrcVal = Full & M;
    }
  }

  //
  // Emulate.
  //
  if (IsPopcnt) {
    UINT64  Result = PopCount (SrcVal, OpBytes);

    if (OpBytes == 8) {
      HvWriteGpr (Regs, DestReg, Result);
    } else if (OpBytes == 4) {
      HvWriteGpr (Regs, DestReg, Result & 0xFFFFFFFFULL);            // zero-extend
    } else { // 2
      UINT64  Old = HvReadGpr (Regs, DestReg);
      HvWriteGpr (Regs, DestReg, (Old & ~0xFFFFULL) | (Result & 0xFFFF));
    }

    //
    // POPCNT: clear OF/SF/ZF/AF/CF/PF; then ZF = (SRC == 0).
    //
    *Rflags &= ~(UINT64)(RFLAG_OF | RFLAG_SF | RFLAG_AF | RFLAG_CF | RFLAG_PF | RFLAG_ZF);
    if (SrcVal == 0) {
      *Rflags |= RFLAG_ZF;
    }
  } else /* IsCrc32 */ {
    UINT32  Crc = (UINT32)(HvReadGpr (Regs, DestReg) & 0xFFFFFFFFULL);
    UINTN   I;

    for (I = 0; I < SrcBytes; I++) {
      Crc = Crc32cByte (Crc, (UINT8)(SrcVal >> (8 * I)));
    }

    //
    // Result zero-extends into the 64-bit destination (true for both the r32
    // and REX.W r64 forms).  CRC32 does not affect RFLAGS.
    //
    HvWriteGpr (Regs, DestReg, (UINT64)Crc);
  }

  *InstrLen = (UINT32)P;
  return TRUE;
}
