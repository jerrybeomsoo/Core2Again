/** @file
  Sse41.c - Software emulation of the SSE4.1 XMM instruction set.

  Enabled automatically only when the host CPU lacks SSE4.1 (Q6600 / Kentsfield);
  on Penryn/Yorkfield the instructions execute in hardware and never trap, so
  these routines are dormant.  Each operates on 16-byte little-endian register
  images that the decoder copies out of / back into the guest FXSAVE area.

  Coverage: PMOVSX/ZX, PCMPEQQ, PMULLD, PMULDQ, PACKUSDW, PMIN/PMAX(SB/SD/UW/UD),
  PHMINPOSUW, PTEST, PBLENDVB, BLENDVPS, BLENDVPD, PBLENDW, INSERTPS, MPSADBW,
  ROUND(PS/PD/SS/SD), DPPS, DPPD, and the element get/set used by
  PEXTR/PINSR/EXTRACTPS.

  The floating-point members (ROUND*, DPPS, DPPD) are validated against hardware
  by tests/run.sh; ROUND is implemented by IEEE-754 bit manipulation (no CRT float
  helpers), DPPS/DPPD use SSE2 scalar arithmetic (present on every VMX CPU).
**/

#include "Hypervisor.h"

//
// DPPS/DPPD below use scalar float/double (SSE2, present on every VMX CPU).
// MSVC references _fltused when a module uses floating point; EDK II does not
// provide it, so define it here to satisfy the linker.  (No CRT is pulled in -
// the arithmetic compiles to inline SSE2 mulss/addss / mulsd/addsd.)
//
#if defined (_MSC_VER)
int  _fltused = 1;
#endif

// Little-endian element accessors (x86 permits unaligned access).
#define U8P(p,i)    (((UINT8  *)(p))[i])
#define U16P(p,i)   (((UINT16 *)(p))[i])
#define U32P(p,i)   (((UINT32 *)(p))[i])
#define U64P(p,i)   (((UINT64 *)(p))[i])
#define I8P(p,i)    (((INT8   *)(p))[i])
#define I16P(p,i)   (((INT16  *)(p))[i])
#define I32P(p,i)   (((INT32  *)(p))[i])
#define I64P(p,i)   (((INT64  *)(p))[i])

#define RFLAG_CF  BIT0
#define RFLAG_PF  BIT2
#define RFLAG_AF  BIT4
#define RFLAG_ZF  BIT6
#define RFLAG_SF  BIT7
#define RFLAG_OF  BIT11

//
// PMOVSX/PMOVZX: sign- or zero-extend N source elements to wider dest elements.
//
VOID
Sse41Pmovx (
  IN OUT UINT8    *Dst,
  IN     UINT8    *Src,
  IN     UINTN     SrcBytes,
  IN     UINTN     DstBytes,
  IN     BOOLEAN   Sign
  )
{
  UINTN   Count = 16 / DstBytes;
  UINT8   Tmp[16];
  UINTN   I;

  for (I = 0; I < Count; I++) {
    INT64   V;
    if (SrcBytes == 1) {
      V = Sign ? (INT64)I8P (Src, I) : (INT64)U8P (Src, I);
    } else if (SrcBytes == 2) {
      V = Sign ? (INT64)I16P (Src, I) : (INT64)U16P (Src, I);
    } else {
      V = Sign ? (INT64)I32P (Src, I) : (INT64)U32P (Src, I);
    }
    if (DstBytes == 2)      { U16P (Tmp, I) = (UINT16)V; }
    else if (DstBytes == 4) { U32P (Tmp, I) = (UINT32)V; }
    else                    { U64P (Tmp, I) = (UINT64)V; }
  }
  CopyMem (Dst, Tmp, 16);
}

VOID
Sse41Pcmpeqq (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  UINTN  I;
  for (I = 0; I < 2; I++) {
    U64P (Dst, I) = (U64P (Dst, I) == U64P (Src, I)) ? 0xFFFFFFFFFFFFFFFFULL : 0;
  }
}

VOID
Sse41Pmulld (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  UINTN  I;
  for (I = 0; I < 4; I++) {
    U32P (Dst, I) = (UINT32)(I32P (Dst, I) * I32P (Src, I));
  }
}

VOID
Sse41Pmuldq (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  // Even dwords (0 and 2) multiplied as signed 32x32 -> 64.
  INT64  R0 = (INT64)I32P (Dst, 0) * (INT64)I32P (Src, 0);
  INT64  R1 = (INT64)I32P (Dst, 2) * (INT64)I32P (Src, 2);
  U64P (Dst, 0) = (UINT64)R0;
  U64P (Dst, 1) = (UINT64)R1;
}

VOID
Sse41Packusdw (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  UINT8  Tmp[16];
  UINTN  I;
  for (I = 0; I < 4; I++) {
    INT32  D = I32P (Dst, I);
    U16P (Tmp, I) = (UINT16)(D < 0 ? 0 : (D > 0xFFFF ? 0xFFFF : D));
  }
  for (I = 0; I < 4; I++) {
    INT32  S = I32P (Src, I);
    U16P (Tmp, 4 + I) = (UINT16)(S < 0 ? 0 : (S > 0xFFFF ? 0xFFFF : S));
  }
  CopyMem (Dst, Tmp, 16);
}

VOID
Sse41MinMax (
  IN OUT UINT8    *Dst,
  IN     UINT8    *Src,
  IN     UINTN     ElemBytes,
  IN     BOOLEAN   Signd,
  IN     BOOLEAN   IsMax
  )
{
  UINTN  Count = 16 / ElemBytes;
  UINTN  I;

  for (I = 0; I < Count; I++) {
    if (ElemBytes == 1) {
      INT64  a = Signd ? (INT64)I8P (Dst, I) : (INT64)U8P (Dst, I);
      INT64  b = Signd ? (INT64)I8P (Src, I) : (INT64)U8P (Src, I);
      U8P (Dst, I) = (UINT8)(IsMax ? (a > b ? a : b) : (a < b ? a : b));
    } else if (ElemBytes == 2) {
      INT64  a = Signd ? (INT64)I16P (Dst, I) : (INT64)U16P (Dst, I);
      INT64  b = Signd ? (INT64)I16P (Src, I) : (INT64)U16P (Src, I);
      U16P (Dst, I) = (UINT16)(IsMax ? (a > b ? a : b) : (a < b ? a : b));
    } else {
      INT64  a = Signd ? (INT64)I32P (Dst, I) : (INT64)U32P (Dst, I);
      INT64  b = Signd ? (INT64)I32P (Src, I) : (INT64)U32P (Src, I);
      U32P (Dst, I) = (UINT32)(IsMax ? (a > b ? a : b) : (a < b ? a : b));
    }
  }
}

VOID
Sse41Phminposuw (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src
  )
{
  UINT16  Min = U16P (Src, 0);
  UINTN   Pos = 0;
  UINTN   I;

  for (I = 1; I < 8; I++) {
    if (U16P (Src, I) < Min) {
      Min = U16P (Src, I);
      Pos = I;
    }
  }
  ZeroMem (Dst, 16);
  U16P (Dst, 0) = Min;
  U16P (Dst, 1) = (UINT16)Pos;
}

VOID
Sse41Ptest (
  IN     UINT8   *Dst,
  IN     UINT8   *Src,
  IN OUT UINT64  *Rflags
  )
{
  UINT64  And0  = U64P (Src, 0) & U64P (Dst, 0);
  UINT64  And1  = U64P (Src, 1) & U64P (Dst, 1);
  UINT64  Andn0 = U64P (Src, 0) & ~U64P (Dst, 0);
  UINT64  Andn1 = U64P (Src, 1) & ~U64P (Dst, 1);

  *Rflags &= ~(UINT64)(RFLAG_CF | RFLAG_ZF | RFLAG_SF | RFLAG_OF | RFLAG_AF | RFLAG_PF);
  if ((And0 | And1) == 0)   { *Rflags |= RFLAG_ZF; }   // ZF = (SRC & DST)  == 0
  if ((Andn0 | Andn1) == 0) { *Rflags |= RFLAG_CF; }   // CF = (SRC & ~DST) == 0
}

//
// Variable blends: select each element from Src where the corresponding mask
// element's most-significant bit is set (PBLENDVB byte / BLENDVPS dword /
// BLENDVPD qword).  Mask is XMM0.
//
VOID
Sse41Blendv (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8  *Mask,
  IN     UINTN   ElemBytes
  )
{
  UINTN  Count = 16 / ElemBytes;
  UINTN  I;

  for (I = 0; I < Count; I++) {
    BOOLEAN  Take;
    if (ElemBytes == 1)      { Take = (BOOLEAN)((U8P  (Mask, I) & 0x80) != 0); }
    else if (ElemBytes == 4) { Take = (BOOLEAN)((U32P (Mask, I) & 0x80000000U) != 0); }
    else                     { Take = (BOOLEAN)((U64P (Mask, I) & 0x8000000000000000ULL) != 0); }

    if (Take) {
      if (ElemBytes == 1)      { U8P  (Dst, I) = U8P  (Src, I); }
      else if (ElemBytes == 4) { U32P (Dst, I) = U32P (Src, I); }
      else                     { U64P (Dst, I) = U64P (Src, I); }
    }
  }
}

VOID
Sse41Pblendw (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8   Imm8
  )
{
  UINTN  I;
  for (I = 0; I < 8; I++) {
    if (Imm8 & (1u << I)) {
      U16P (Dst, I) = U16P (Src, I);
    }
  }
}

VOID
Sse41Insertps (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8   Imm8
  )
{
  UINTN   CountS = (Imm8 >> 6) & 3;
  UINTN   CountD = (Imm8 >> 4) & 3;
  UINT32  Sel    = U32P (Src, CountS);
  UINTN   I;

  U32P (Dst, CountD) = Sel;
  for (I = 0; I < 4; I++) {
    if (Imm8 & (1u << I)) {
      U32P (Dst, I) = 0;             // zero-mask (imm8[3:0])
    }
  }
}

VOID
Sse41Mpsadbw (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8   Imm8
  )
{
  UINTN  SrcOff = (Imm8 & 0x3) * 4;         // imm8[1:0] * 4
  UINTN  DstOff = ((Imm8 >> 2) & 0x1) * 4;  // imm8[2] * 4
  UINT8  Tmp[16];
  UINTN  I, J;

  for (I = 0; I < 8; I++) {
    UINT32  Sad = 0;
    for (J = 0; J < 4; J++) {
      INT32  D = (INT32)U8P (Dst, DstOff + I + J);
      INT32  S = (INT32)U8P (Src, SrcOff + J);
      INT32  Diff = D - S;
      Sad += (UINT32)(Diff < 0 ? -Diff : Diff);
    }
    U16P (Tmp, I) = (UINT16)Sad;
  }
  CopyMem (Dst, Tmp, 16);
}

//
// ------------------------------------------------------------------------
// Floating-point members.
// ------------------------------------------------------------------------
//
STATIC
UINT32
RoundF32Bits (
  IN UINT32  Bits,
  IN UINTN   Mode          // 0 nearest-even, 1 down, 2 up, 3 truncate
  )
{
  UINT32  Sign = Bits & 0x80000000U;
  INT32   Exp  = (INT32)((Bits >> 23) & 0xFF) - 127;
  UINT32  Frac;
  UINT32  FracMask;
  UINT32  Half;
  BOOLEAN Neg  = (Sign != 0);

  if (((Bits >> 23) & 0xFF) == 0xFF) {
    //
    // NaN or Infinity.  Infinity passes through unchanged, but a *signaling*
    // NaN must come back quieted (mantissa MSB set) - that is what the hardware
    // returns whenever the invalid-operation exception is masked, which is the
    // normal state.  Returning the SNaN unchanged was the only way this
    // emulator still disagreed with silicon.
    //
    if ((Bits & 0x007FFFFFU) != 0) {
      Bits |= 0x00400000U;                    // SNaN -> QNaN
    }
    return Bits;
  }
  if (Exp >= 23) {
    return Bits;                              // already integral
  }
  if (Exp < 0) {
    // |x| < 1 -> result is 0 or +/-1 depending on mode.
    UINT32  One = Sign | 0x3F800000U;         // +/-1.0
    BOOLEAN NonZero = ((Bits & 0x7FFFFFFFU) != 0);
    switch (Mode) {
      case 1:  return (Neg && NonZero) ? One : Sign;                 // down
      case 2:  return (!Neg && NonZero) ? One : Sign;               // up
      case 3:  return Sign;                                          // truncate
      default:
        // nearest-even: |x| > 0.5 -> 1, exactly 0.5 -> 0 (even), < 0.5 -> 0
        if (Exp == -1 && (Bits & 0x007FFFFFU) != 0) return One;      // >0.5
        return Sign;
    }
  }

  FracMask = (1u << (23 - Exp)) - 1;
  Frac     = Bits & FracMask;
  if (Frac == 0) {
    return Bits;                              // already integral
  }

  Bits &= ~FracMask;                          // truncated toward zero
  switch (Mode) {
    case 1:  if (Neg)  Bits += (FracMask + 1); break;               // down (floor)
    case 2:  if (!Neg) Bits += (FracMask + 1); break;               // up (ceil)
    case 3:  break;                                                  // truncate
    default:                                                         // nearest-even
      Half = (FracMask + 1) >> 1;
      if (Frac > Half || (Frac == Half && (Bits & (FracMask + 1)))) {
        Bits += (FracMask + 1);
      }
      break;
  }
  return Bits;
}

STATIC
UINT64
RoundF64Bits (
  IN UINT64  Bits,
  IN UINTN   Mode
  )
{
  UINT64  Sign = Bits & 0x8000000000000000ULL;
  INT32   Exp  = (INT32)((Bits >> 52) & 0x7FF) - 1023;
  UINT64  Frac, FracMask, Half;
  BOOLEAN Neg  = (Sign != 0);

  if (((Bits >> 52) & 0x7FF) == 0x7FF) {
    if ((Bits & 0x000FFFFFFFFFFFFFULL) != 0) {
      Bits |= 0x0008000000000000ULL;          // SNaN -> QNaN, as above
    }
    return Bits;
  }
  if (Exp >= 52) {
    return Bits;
  }
  if (Exp < 0) {
    UINT64  One = Sign | 0x3FF0000000000000ULL;
    BOOLEAN NonZero = ((Bits & 0x7FFFFFFFFFFFFFFFULL) != 0);
    switch (Mode) {
      case 1:  return (Neg && NonZero) ? One : Sign;
      case 2:  return (!Neg && NonZero) ? One : Sign;
      case 3:  return Sign;
      default:
        if (Exp == -1 && (Bits & 0x000FFFFFFFFFFFFFULL) != 0) return One;
        return Sign;
    }
  }

  FracMask = (1ULL << (52 - Exp)) - 1;
  Frac     = Bits & FracMask;
  if (Frac == 0) {
    return Bits;
  }

  Bits &= ~FracMask;
  switch (Mode) {
    case 1:  if (Neg)  Bits += (FracMask + 1); break;
    case 2:  if (!Neg) Bits += (FracMask + 1); break;
    case 3:  break;
    default:
      Half = (FracMask + 1) >> 1;
      if (Frac > Half || (Frac == Half && (Bits & (FracMask + 1)))) {
        Bits += (FracMask + 1);
      }
      break;
  }
  return Bits;
}

VOID
Sse41Round (
  IN OUT UINT8    *Dst,
  IN     UINT8    *Src,
  IN     UINT8     Imm8,
  IN     UINTN     ElemBytes,
  IN     BOOLEAN   Scalar
  )
{
  // imm8[2]=1 means "use MXCSR rounding"; we do not model MXCSR here and fall
  // back to round-to-nearest-even.
  UINTN  Mode  = (Imm8 & 0x4) ? 0 : (Imm8 & 0x3);
  UINTN  Count = Scalar ? 1 : (16 / ElemBytes);
  UINTN  I;

  for (I = 0; I < Count; I++) {
    if (ElemBytes == 4) {
      U32P (Dst, I) = RoundF32Bits (U32P (Src, I), Mode);
    } else {
      U64P (Dst, I) = RoundF64Bits (U64P (Src, I), Mode);
    }
  }
  // Scalar forms leave the upper element(s) of Dst unmodified (Dst already
  // holds them, since Dst is the destination register image).
}

VOID
Sse41Dpps (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8   Imm8
  )
{
  float  Prod[4];
  float  Sum;
  UINT8  Tmp[16];
  UINTN  I;

  //
  // Follow the SDM pseudocode exactly: every lane contributes a term (+0.0 when
  // its input-mask bit is clear), and the terms are summed as a PAIRWISE TREE,
  // (p0+p1) + (p2+p3) - not accumulated left to right into a zero-seeded total.
  //
  // Both details are observable.  Seeding with +0.0 destroys a negative-zero
  // result, because (+0.0) + (-0.0) is +0.0 under round-to-nearest, and the
  // hardware really does return -0.0 when the terms underflow negative.  The
  // tree also rounds differently from a linear sum, so the order is part of the
  // answer, not an implementation detail.
  //
  for (I = 0; I < 4; I++) {
    if (Imm8 & (0x10u << I)) {                 // input mask imm8[7:4]
      float  a = *(float *)&U32P (Dst, I);
      float  b = *(float *)&U32P (Src, I);
      Prod[I] = a * b;
    } else {
      Prod[I] = 0.0f;
    }
  }
  Sum = (Prod[0] + Prod[1]) + (Prod[2] + Prod[3]);

  for (I = 0; I < 4; I++) {
    float  V = (Imm8 & (1u << I)) ? Sum : 0.0f; // output mask imm8[3:0]
    *(float *)&U32P (Tmp, I) = V;
  }
  CopyMem (Dst, Tmp, 16);
}

VOID
Sse41Dppd (
  IN OUT UINT8  *Dst,
  IN     UINT8  *Src,
  IN     UINT8   Imm8
  )
{
  double  Prod[2];
  double  Sum;
  UINT8   Tmp[16];
  UINTN   I;

  //
  // Same shape as DPPS above: masked-off lanes contribute +0.0 and the two
  // terms are added directly, with no zero seed to swallow a negative zero.
  //
  for (I = 0; I < 2; I++) {
    if (Imm8 & (0x10u << I)) {                 // input mask imm8[5:4]
      double  a = *(double *)&U64P (Dst, I);
      double  b = *(double *)&U64P (Src, I);
      Prod[I] = a * b;
    } else {
      Prod[I] = 0.0;
    }
  }
  Sum = Prod[0] + Prod[1];

  for (I = 0; I < 2; I++) {
    double  V = (Imm8 & (1u << I)) ? Sum : 0.0; // output mask imm8[1:0]
    *(double *)&U64P (Tmp, I) = V;
  }
  CopyMem (Dst, Tmp, 16);
}

//
// Element get/set for PEXTR/PINSR/EXTRACTPS (the decoder handles the GPR or
// memory side).
//
UINT64
Sse41GetElem (
  IN UINT8  *Src,
  IN UINTN   Index,
  IN UINTN   ElemBytes
  )
{
  if (ElemBytes == 1) { return U8P  (Src, Index); }
  if (ElemBytes == 2) { return U16P (Src, Index); }
  if (ElemBytes == 4) { return U32P (Src, Index); }
  return U64P (Src, Index);
}

VOID
Sse41SetElem (
  IN OUT UINT8  *Dst,
  IN     UINTN   Index,
  IN     UINTN   ElemBytes,
  IN     UINT64  Val
  )
{
  if (ElemBytes == 1)      { U8P  (Dst, Index) = (UINT8)Val; }
  else if (ElemBytes == 2) { U16P (Dst, Index) = (UINT16)Val; }
  else if (ElemBytes == 4) { U32P (Dst, Index) = (UINT32)Val; }
  else                     { U64P (Dst, Index) = Val; }
}
