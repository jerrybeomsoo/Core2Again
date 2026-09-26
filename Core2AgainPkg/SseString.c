/** @file
  SseString.c - Software emulation of the XMM-operand SSE4.2 instructions:

     PCMPGTQ    66 0F 38 37 /r          packed signed qword greater-than
     PCMPESTRM  66 0F 3A 60 /r ib       explicit-length, mask result   (XMM0)
     PCMPESTRI  66 0F 3A 61 /r ib       explicit-length, index result  (ECX)
     PCMPISTRM  66 0F 3A 62 /r ib       implicit-length, mask result   (XMM0)
     PCMPISTRI  66 0F 3A 63 /r ib       implicit-length, index result  (ECX)

  CRC32 and POPCNT (the scalar SSE4.2 members) live in SseDecoder.c.  Together
  these cover the full SSE4.2 opcode set.  The PCMPxSTRx model follows the Intel
  SDM Vol. 2B "Operation" pseudocode: build IntRes1 by aggregation, apply the
  polarity to get IntRes2, then produce the index/mask output and EFLAGS.

  Operands arrive as raw 16-byte little-endian images (XMM registers copied out
  of the guest FXSAVE area, or a 16-byte memory operand).

  Validated against hardware by tests/run.sh, which runs the real instructions
  on a host that has them and compares results and flags over ~4.1 M cases
  covering every imm8 and all four forms.  The string aggregation does have many
  boundary cases: that test is how the Equal-Ordered bug was found.
**/

#include "Hypervisor.h"

#define RFLAG_CF  BIT0
#define RFLAG_PF  BIT2
#define RFLAG_AF  BIT4
#define RFLAG_ZF  BIT6
#define RFLAG_SF  BIT7
#define RFLAG_OF  BIT11

//
// PCMPGTQ: for each of the two 64-bit lanes, Dst.q = (Dst.q > Src.q) ? ~0 : 0
// using signed comparison.
//
VOID
SseXmmPcmpGtq (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  UINTN  Lane;

  for (Lane = 0; Lane < 2; Lane++) {
    INT64   D = *(INT64 *)(Dst + Lane * 8);
    INT64   S = *(INT64 *)(Src + Lane * 8);
    UINT64  R = (D > S) ? 0xFFFFFFFFFFFFFFFFULL : 0;
    *(UINT64 *)(Dst + Lane * 8) = R;
  }
}

//
// Read element k (byte or word) of a 16-byte operand, sign- or zero-extended.
//
STATIC
INT64
ReadElem (
  IN UINT8    *Op,
  IN UINTN     K,
  IN UINTN     ElemBytes,
  IN BOOLEAN   Signed
  )
{
  if (ElemBytes == 1) {
    UINT8  V = Op[K];
    return Signed ? (INT64)(INT8)V : (INT64)V;
  } else {
    UINT16  V = (UINT16)(Op[K * 2] | ((UINT16)Op[K * 2 + 1] << 8));
    return Signed ? (INT64)(INT16)V : (INT64)V;
  }
}

STATIC
BOOLEAN
ElemIsZero (
  IN UINT8   *Op,
  IN UINTN    K,
  IN UINTN    ElemBytes
  )
{
  if (ElemBytes == 1) {
    return (BOOLEAN)(Op[K] == 0);
  }
  return (BOOLEAN)((Op[K * 2] == 0) && (Op[K * 2 + 1] == 0));
}

VOID
SseXmmPcmpStr (
  IN     UINT8    *Op1,
  IN     UINT8    *Op2,
  IN     UINT8     Imm8,
  IN     BOOLEAN   ExplicitLen,
  IN     INT64     LenRax,
  IN     INT64     LenRdx,
  IN     BOOLEAN   IndexForm,
  OUT    UINT32   *OutIndex,
  OUT    UINT8    *OutXmm0,
  IN OUT UINT64   *Rflags
  )
{
  UINTN    ElemBytes = (Imm8 & 0x01) ? 2 : 1;
  UINTN    NumElems  = (Imm8 & 0x01) ? 8 : 16;
  BOOLEAN  Signed    = (Imm8 & 0x02) ? TRUE : FALSE;
  UINTN    AggOp     = (Imm8 >> 2) & 3;
  UINTN    Polarity  = (Imm8 >> 4) & 3;
  UINTN    OutSel    = (Imm8 >> 6) & 1;

  INT64    A[16];
  INT64    B[16];
  UINTN    LenA, LenB;
  UINT32   IntRes1 = 0;
  UINT32   IntRes2;
  UINT32   FullMask = (UINT32)((1u << NumElems) - 1);
  UINTN    I, J;

  for (I = 0; I < NumElems; I++) {
    A[I] = ReadElem (Op1, I, ElemBytes, Signed);
    B[I] = ReadElem (Op2, I, ElemBytes, Signed);
  }

  //
  // Valid lengths of each operand.
  //
  if (ExplicitLen) {
    INT64  AbsA = (LenRax < 0) ? -LenRax : LenRax;
    INT64  AbsB = (LenRdx < 0) ? -LenRdx : LenRdx;
    LenA = (AbsA > (INT64)NumElems) ? NumElems : (UINTN)AbsA;
    LenB = (AbsB > (INT64)NumElems) ? NumElems : (UINTN)AbsB;
  } else {
    LenA = NumElems;
    LenB = NumElems;
    for (I = 0; I < NumElems; I++) {
      if (ElemIsZero (Op1, I, ElemBytes)) { LenA = I; break; }
    }
    for (I = 0; I < NumElems; I++) {
      if (ElemIsZero (Op2, I, ElemBytes)) { LenB = I; break; }
    }
  }

  //
  // Aggregate.  Bit i of IntRes1 corresponds to element i of operand2 (B).
  //
  for (I = 0; I < NumElems; I++) {
    BOOLEAN  R = FALSE;

    switch (AggOp) {
      case 0: // Equal Any: B[i] matches any valid element of A
        R = FALSE;
        if (I < LenB) {
          for (J = 0; J < LenA; J++) {
            if (B[I] == A[J]) { R = TRUE; break; }
          }
        }
        break;

      case 1: // Ranges: A[2k]..A[2k+1] pairs; B[i] within any range
        R = FALSE;
        if (I < LenB) {
          for (J = 0; J + 1 < LenA; J += 2) {
            if ((B[I] >= A[J]) && (B[I] <= A[J + 1])) { R = TRUE; break; }
          }
        }
        break;

      case 2: // Equal Each: element-wise A[i]==B[i] with validity override
      {
        BOOLEAN  Av = (BOOLEAN)(I < LenA);
        BOOLEAN  Bv = (BOOLEAN)(I < LenB);
        if (Av && Bv) {
          R = (BOOLEAN)(A[I] == B[I]);
        } else if (!Av && !Bv) {
          R = TRUE;               // both strings ended -> forced match
        } else {
          R = FALSE;              // exactly one ended -> forced mismatch
        }
        break;
      }

      case 3: // Equal Ordered: is A a substring of B starting at i?
      default:
        //
        // The inner loop stops at the end of the REGISTER, not at the end of
        // the needle: the SDM runs it for i = 0 to UpperBound - j, so a
        // comparison that would fall past element 15 (or 7) is never performed
        // at all.  Bounding it by NumElems alone and forcing a mismatch for
        // (I + J) >= LenB conflated two different things - "past the valid
        // string", which really is a forced mismatch, and "past the register",
        // which contributes nothing - and so missed every match starting near
        // the end of the haystack.  That is the substring search behind
        // strstr/wcsstr, wrong roughly 1% of the time.
        //
        R = TRUE;
        for (J = 0; (I + J) < NumElems; J++) {
          if (J >= LenA) {
            break;                // needle exhausted -> forced match from here on
          }
          if ((I + J) >= LenB) {
            R = FALSE;            // needle element remains but haystack ended
            break;
          }
          if (A[J] != B[I + J]) {
            R = FALSE;
            break;
          }
        }
        break;
    }

    if (R) {
      IntRes1 |= (1u << I);
    }
  }

  //
  // Polarity -> IntRes2.
  //   00 positive        : as-is
  //   01 negate all      : complement every element bit
  //   10 positive masked : as-is
  //   11 negate valid    : complement only bits of valid operand2 positions
  //
  switch (Polarity) {
    case 1:
      IntRes2 = (~IntRes1) & FullMask;
      break;
    case 3:
    {
      UINT32  ValidMask = (LenB >= NumElems) ? FullMask
                                             : (UINT32)((1u << LenB) - 1);
      IntRes2 = (IntRes1 ^ ValidMask) & FullMask;
      break;
    }
    case 0:
    case 2:
    default:
      IntRes2 = IntRes1 & FullMask;
      break;
  }

  //
  // Output.
  //
  if (IndexForm) {
    UINT32  Index = (UINT32)NumElems;   // default when IntRes2 == 0
    if (IntRes2 != 0) {
      if (OutSel == 0) {                // least-significant set bit
        for (I = 0; I < NumElems; I++) {
          if (IntRes2 & (1u << I)) { Index = (UINT32)I; break; }
        }
      } else {                          // most-significant set bit
        for (I = NumElems; I > 0; I--) {
          if (IntRes2 & (1u << (I - 1))) { Index = (UINT32)(I - 1); break; }
        }
      }
    }
    if (OutIndex != NULL) {
      *OutIndex = Index;
    }
  } else {
    // Mask form: XMM0.
    if (OutXmm0 != NULL) {
      for (I = 0; I < 16; I++) {
        OutXmm0[I] = 0;
      }
      if (OutSel == 0) {
        // Bit mask in the low NumElems bits.
        for (I = 0; I < NumElems; I++) {
          if (IntRes2 & (1u << I)) {
            OutXmm0[I / 8] |= (UINT8)(1u << (I % 8));
          }
        }
      } else {
        // Expanded byte/word mask.
        for (I = 0; I < NumElems; I++) {
          UINT8  Fill = (IntRes2 & (1u << I)) ? 0xFF : 0x00;
          if (ElemBytes == 1) {
            OutXmm0[I] = Fill;
          } else {
            OutXmm0[I * 2]     = Fill;
            OutXmm0[I * 2 + 1] = Fill;
          }
        }
      }
    }
  }

  //
  // EFLAGS: CF = IntRes2 != 0; ZF = operand2 ended; SF = operand1 ended;
  // OF = IntRes2 bit 0; AF = PF = 0.
  //
  *Rflags &= ~(UINT64)(RFLAG_CF | RFLAG_ZF | RFLAG_SF | RFLAG_OF | RFLAG_AF | RFLAG_PF);
  if (IntRes2 != 0)        { *Rflags |= RFLAG_CF; }
  if (LenB < NumElems)     { *Rflags |= RFLAG_ZF; }
  if (LenA < NumElems)     { *Rflags |= RFLAG_SF; }
  if (IntRes2 & 1u)        { *Rflags |= RFLAG_OF; }
}
