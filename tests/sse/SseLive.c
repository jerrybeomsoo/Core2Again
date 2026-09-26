/** @file
  SseLive.c - exercise the #UD emulation path on a processor that really lacks
  the instructions.

  WHY THIS EXISTS
  ---------------
  Every other test in this repository tests the emulators or the VMX plumbing,
  and neither of them tests the part in between.

    tests/run.sh  compiles Sse41.c / SseString.c in userspace and compares them
                  against the real instructions - 41 M cases.  It proves the
                  arithmetic.  It never sees a #UD, a ModRM byte or a VMCS.

    tests/hv      boots the shipping driver under QEMU.  On any development
                  host the physical CPU HAS SSE4.2, so however CPUID is masked
                  no #UD is ever raised and the decoder never runs.

  What neither covers is the layer that only exists on the target: the guest
  executes an instruction the silicon does not have, the CPU raises #UD, the
  hypervisor decodes it out of guest memory - prefixes, REX, ModRM, SIB,
  displacement, RIP-relative, imm8 - reads its operands through the guest's page
  tables, emulates it, writes the results back into the guest's GPRs and FXSAVE
  image, and advances RIP by exactly the right number of bytes.  A mistake
  anywhere in there is invisible to both suites above.

  This application runs as a guest under the shipping driver and compares:

    * POPCNT and CRC32 against a scalar reference written from the definition -
      an independent oracle, so this catches emulator bugs as well as decode
      bugs;
    * the SSE4.2 XMM instructions against Sse41.c / SseString.c called directly
      as C functions.  Both sides then share the arithmetic, which is the point:
      any difference is the decode/trap/resume path and nothing else;
    * the SSE4.1 set, which a Penryn executes natively, against the emulators -
      a true hardware differential, on silicon the userspace suite has never run
      on.

  REPORTING
  ---------
  There is no console after ExitBootServices, and an un-emulated #UD is injected
  into a guest with no IDT, which is a triple fault and a reset.  So the report
  has to survive the machine dying mid-test: a group/case marker is painted
  before every case, and the last one legible on a screenshot names exactly
  which case killed it.  Serial gets a line per group, written with a BOUNDED
  spin - under nested VMX every port write is an exit, and the usual unbounded
  UART-ready loop takes minutes per character.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>

//
// The emulators, linked in as the reference implementation.
//
VOID SseXmmPcmpGtq (UINT8 *Dst, UINT8 *Src);
VOID SseXmmPcmpStr (UINT8 *Op1, UINT8 *Op2, UINT8 Imm8, BOOLEAN ExplicitLen,
                    INT64 LenRax, INT64 LenRdx, BOOLEAN IndexForm,
                    UINT32 *OutIndex, UINT8 *OutXmm0, UINT64 *Rflags);
VOID Sse41Pmulld     (UINT8 *Dst, UINT8 *Src);
VOID Sse41Pcmpeqq    (UINT8 *Dst, UINT8 *Src);
VOID Sse41Packusdw   (UINT8 *Dst, UINT8 *Src);
VOID Sse41MinMax     (UINT8 *Dst, UINT8 *Src, UINTN ElemBytes, BOOLEAN Signd, BOOLEAN IsMax);
VOID Sse41Phminposuw (UINT8 *Dst, UINT8 *Src);
VOID Sse41Pblendw    (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Mpsadbw    (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Pmovx      (UINT8 *Dst, UINT8 *Src, UINTN SrcBytes, UINTN DstBytes, BOOLEAN Sign);

// ---------------------------------------------------------------------------
// Output: framebuffer digits (survives anything) and bounded-spin serial.
// ---------------------------------------------------------------------------

STATIC volatile UINT32  *mFb    = NULL;
STATIC UINTN             mFbPps = 0;
STATIC UINTN             mFbW   = 0;
STATIC UINTN             mFbH   = 0;

#define COM1  0x3F8

//
// One byte to the UART, with a BOUNDED wait for the holding register.  Under
// nested VMX each of these is a VM exit; an unbounded ready-spin is what makes
// serial output after ExitBootServices look like a hang.
//
STATIC
VOID
SerialByte (
  IN UINT8  B
  )
{
  UINTN  Spin;

  for (Spin = 0; Spin < 200; Spin++) {
    if ((IoRead8 (COM1 + 5) & 0x20) != 0) {
      break;
    }
  }
  IoWrite8 (COM1, B);
}

STATIC
VOID
SerialStr (
  IN CONST CHAR8  *S
  )
{
  while (*S != '\0') {
    SerialByte ((UINT8)*S++);
  }
}

STATIC
VOID
SerialNum (
  IN UINT64  V
  )
{
  CHAR8  Buf[24];
  UINTN  N = 0;

  if (V == 0) {
    SerialByte ('0');
    return;
  }
  while ((V > 0) && (N < sizeof (Buf))) {
    Buf[N++] = (CHAR8)('0' + (V % 10));
    V /= 10;
  }
  while (N > 0) {
    SerialByte ((UINT8)Buf[--N]);
  }
}

STATIC CONST UINT8  mFont[10][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

#define SC  5                      // pixel scale
#define CW  (6 * SC)               // cell width

//
// Paint a decimal at (X0,Y0).  Deliberately to the RIGHT of x=520: the
// hypervisor's own heartbeat owns the left strip and repaints it constantly.
//
STATIC
VOID
Paint (
  IN UINTN   Row,
  IN UINT64  Value
  )
{
  UINTN  X0 = 540, Y0, X, Y, I, C, R, N;
  UINT8  D[16];

  if (mFb == NULL) {
    return;
  }
  Y0 = 20 + Row * (7 * SC + 6);

  for (Y = Y0; (Y < Y0 + 7 * SC + 2) && (Y < mFbH); Y++) {
    for (X = X0; (X < X0 + CW * 13) && (X < mFbW); X++) {
      mFb[Y * mFbPps + X] = 0;
    }
  }

  N = 0;
  do {
    D[N++] = (UINT8)(Value % 10);
    Value /= 10;
  } while ((Value > 0) && (N < sizeof (D)));

  X = X0;
  for (I = N; I > 0; I--) {
    UINT8  Dig = D[I - 1];

    for (R = 0; R < 7; R++) {
      for (C = 0; C < 5; C++) {
        if ((mFont[Dig][R] & (0x10 >> C)) != 0) {
          UINTN  Px, Py;

          for (Py = 0; Py < SC; Py++) {
            for (Px = 0; Px < SC; Px++) {
              UINTN  Sx = X + C * SC + Px, Sy = Y0 + R * SC + Py;

              if ((Sx < mFbW) && (Sy < mFbH)) {
                mFb[Sy * mFbPps + Sx] = 0x0000FF00;
              }
            }
          }
        }
      }
    }
    X += CW;
  }
}

// ---------------------------------------------------------------------------
// Scoreboard.
// ---------------------------------------------------------------------------

STATIC UINTN   mRun    = 0;
STATIC UINTN   mFail   = 0;
STATIC UINTN   mFirst  = 0;        // id of the first failing case
STATIC UINT64  mExp    = 0;
STATIC UINT64  mGot    = 0;
STATIC UINTN   mCase   = 0;        // painted before every case: where a reset happened

STATIC
VOID
Check (
  IN UINTN   Id,
  IN UINT64  Expected,
  IN UINT64  Actual
  )
{
  mRun++;
  if (Expected != Actual) {
    mFail++;
    if (mFirst == 0) {
      mFirst = Id;
      mExp   = Expected;
      mGot   = Actual;
    }
  }
}

STATIC
VOID
CheckBuf (
  IN UINTN        Id,
  IN CONST UINT8  *Expected,
  IN CONST UINT8  *Actual,
  IN UINTN         Len
  )
{
  UINTN  I;

  mRun++;
  for (I = 0; I < Len; I++) {
    if (Expected[I] != Actual[I]) {
      mFail++;
      if (mFirst == 0) {
        mFirst = Id;
        CopyMem (&mExp, Expected, 8);
        CopyMem (&mGot, Actual,   8);
      }
      return;
    }
  }
}

STATIC
VOID
Mark (
  IN UINTN  Id
  )
{
  mCase = Id;
  Paint (0, Id);
}

// ---------------------------------------------------------------------------
// Independent references, written from the definition.
// ---------------------------------------------------------------------------

STATIC
UINT64
RefPopcnt (
  IN UINT64  V,
  IN UINTN   Bytes
  )
{
  UINT64  C = 0;

  if (Bytes < 8) {
    V &= (((UINT64)1 << (Bytes * 8)) - 1);
  }
  while (V != 0) {
    C += (V & 1);
    V >>= 1;
  }
  return C;
}

STATIC
UINT32
RefCrc32c (
  IN UINT32  Crc,
  IN UINT8   B
  )
{
  UINTN  I;

  Crc ^= B;
  for (I = 0; I < 8; I++) {
    Crc = (Crc >> 1) ^ (0x82F63B78u & (UINT32)(0u - (Crc & 1u)));
  }
  return Crc;
}

STATIC UINT64  mRng = 0x243F6A8885A308D3ULL;

STATIC
UINT64
Rnd (
  VOID
  )
{
  mRng ^= mRng << 13;
  mRng ^= mRng >> 7;
  mRng ^= mRng << 17;
  return mRng;
}

// ---------------------------------------------------------------------------
// The instructions themselves.  Explicit mnemonics, not intrinsics: this module
// is compiled -mno-sse4 -mno-popcnt like the rest of the package, which gates
// codegen but not the assembler.
// ---------------------------------------------------------------------------

STATIC UINT64 PopcntQ  (UINT64 V)  { UINT64 R; __asm__ volatile ("popcntq %1, %0" : "=r"(R) : "r"(V) : "cc"); return R; }
STATIC UINT32 PopcntL  (UINT32 V)  { UINT32 R; __asm__ volatile ("popcntl %1, %0" : "=r"(R) : "r"(V) : "cc"); return R; }
STATIC UINT16 PopcntW  (UINT16 V)  { UINT16 R; __asm__ volatile ("popcntw %1, %0" : "=r"(R) : "r"(V) : "cc"); return R; }

STATIC UINT64 PopcntQM (UINT64 *P) { UINT64 R; __asm__ volatile ("popcntq (%1), %0" : "=r"(R) : "r"(P) : "cc","memory"); return R; }
STATIC UINT32 PopcntLM (UINT32 *P) { UINT32 R; __asm__ volatile ("popcntl (%1), %0" : "=r"(R) : "r"(P) : "cc","memory"); return R; }

//
// Addressing-mode variants of the same instruction.  If the decoder mis-parses
// a SIB byte or a displacement it reads the wrong operand, and only a form that
// USES that encoding can show it.
//
STATIC UINT32 PopcntLD8  (UINT32 *P) { UINT32 R; __asm__ volatile ("popcntl 8(%1), %0"        : "=r"(R) : "r"(P) : "cc","memory"); return R; }
STATIC UINT32 PopcntLD32 (UINT32 *P) { UINT32 R; __asm__ volatile ("popcntl 260(%1), %0"      : "=r"(R) : "r"(P) : "cc","memory"); return R; }
STATIC UINT32 PopcntLSib (UINT32 *P, UINT64 I) { UINT32 R; __asm__ volatile ("popcntl 4(%1,%2,4), %0" : "=r"(R) : "r"(P), "r"(I) : "cc","memory"); return R; }

STATIC UINT64
PopcntQFlags (UINT64 V, UINT64 *Fl)
{
  UINT64  R, F;
  __asm__ volatile ("popcntq %2, %0\n\t pushfq\n\t popq %1"
                    : "=&r"(R), "=&r"(F) : "r"(V) : "cc");
  *Fl = F;
  return R;
}

STATIC UINT32 Crc32B  (UINT32 C, UINT8  B) { __asm__ volatile ("crc32b %1, %0" : "+r"(C) : "q"(B)); return C; }
STATIC UINT32 Crc32W  (UINT32 C, UINT16 B) { __asm__ volatile ("crc32w %1, %0" : "+r"(C) : "r"(B)); return C; }
STATIC UINT32 Crc32L  (UINT32 C, UINT32 B) { __asm__ volatile ("crc32l %1, %0" : "+r"(C) : "r"(B)); return C; }
STATIC UINT64 Crc32Q  (UINT64 C, UINT64 B) { __asm__ volatile ("crc32q %1, %0" : "+r"(C) : "r"(B)); return C; }
STATIC UINT32 Crc32BM (UINT32 C, UINT8  *P){ __asm__ volatile ("crc32b (%1), %0" : "+r"(C) : "r"(P) : "memory"); return C; }
STATIC UINT32 Crc32LM (UINT32 C, UINT32 *P){ __asm__ volatile ("crc32l (%1), %0" : "+r"(C) : "r"(P) : "memory"); return C; }
STATIC UINT64 Crc32QM (UINT64 C, UINT64 *P){ __asm__ volatile ("crc32q (%1), %0" : "+r"(C) : "r"(P) : "memory"); return C; }

STATIC VOID
PcmpGtqRR (UINT8 *A, UINT8 *B, UINT8 *Out)
{
  __asm__ volatile ("movdqu (%0), %%xmm1\n\t movdqu (%1), %%xmm2\n\t"
                    "pcmpgtq %%xmm2, %%xmm1\n\t movdqu %%xmm1, (%2)"
                    : : "r"(A), "r"(B), "r"(Out) : "xmm1","xmm2","memory");
}

STATIC VOID
PcmpGtqRM (UINT8 *A, UINT8 *B, UINT8 *Out)
{
  __asm__ volatile ("movdqu (%0), %%xmm1\n\t pcmpgtq (%1), %%xmm1\n\t movdqu %%xmm1, (%2)"
                    : : "r"(A), "r"(B), "r"(Out) : "xmm1","memory");
}

//
// REX.R form: the destination is xmm9, so the decoder must apply REX.R to the
// ModRM reg field.  Getting that wrong reads and writes xmm1 instead, which no
// register-only test can distinguish.
//
STATIC VOID
PcmpGtqRex (UINT8 *A, UINT8 *B, UINT8 *Out)
{
  __asm__ volatile ("movdqu (%0), %%xmm9\n\t movdqu (%1), %%xmm10\n\t"
                    "pcmpgtq %%xmm10, %%xmm9\n\t movdqu %%xmm9, (%2)"
                    : : "r"(A), "r"(B), "r"(Out) : "xmm9","xmm10","memory");
}

//
// PCMPISTRI / PCMPESTRI / PCMPISTRM / PCMPESTRM need imm8 as a literal, so each
// tested value is its own case.  IMM_CASE keeps them one line each.
//
#define ISTRI(I, A, B, IdxOut, FlOut)                                        \
  do { UINT32 _i; UINT64 _f;                                                  \
    __asm__ volatile ("movdqu (%2), %%xmm1\n\t movdqu (%3), %%xmm2\n\t"       \
                      "pcmpistri $" #I ", %%xmm2, %%xmm1\n\t"                 \
                      "pushfq\n\t popq %1\n\t movl %%ecx, %0"                 \
                      : "=&r"(_i), "=&r"(_f) : "r"(A), "r"(B)                 \
                      : "xmm1","xmm2","rcx","cc","memory");                   \
    *(IdxOut) = _i; *(FlOut) = _f; } while (0)

#define ISTRM(I, A, B, MOut, FlOut)                                          \
  do { UINT64 _f;                                                             \
    __asm__ volatile ("movdqu (%1), %%xmm1\n\t movdqu (%2), %%xmm2\n\t"       \
                      "pcmpistrm $" #I ", %%xmm2, %%xmm1\n\t"                 \
                      "pushfq\n\t popq %0\n\t movdqu %%xmm0, (%3)"            \
                      : "=&r"(_f) : "r"(A), "r"(B), "r"(MOut)                 \
                      : "xmm0","xmm1","xmm2","cc","memory");                  \
    *(FlOut) = _f; } while (0)

#define ESTRI(I, A, B, LA, LB, IdxOut, FlOut)                                \
  do { UINT32 _i; UINT64 _f;                                                  \
    __asm__ volatile ("movdqu (%2), %%xmm1\n\t movdqu (%3), %%xmm2\n\t"       \
                      "mov %4, %%rax\n\t mov %5, %%rdx\n\t"                   \
                      "pcmpestri $" #I ", %%xmm2, %%xmm1\n\t"                 \
                      "pushfq\n\t popq %1\n\t movl %%ecx, %0"                 \
                      : "=&r"(_i), "=&r"(_f)                                  \
                      : "r"(A), "r"(B), "r"((UINT64)(LA)), "r"((UINT64)(LB))  \
                      : "xmm1","xmm2","rax","rcx","rdx","cc","memory");       \
    *(IdxOut) = _i; *(FlOut) = _f; } while (0)

//
// A memory-operand form, which is the one that goes through GuestReadLinear.
//
#define ISTRI_M(I, A, B, IdxOut, FlOut)                                      \
  do { UINT32 _i; UINT64 _f;                                                  \
    __asm__ volatile ("movdqu (%2), %%xmm1\n\t"                               \
                      "pcmpistri $" #I ", (%3), %%xmm1\n\t"                   \
                      "pushfq\n\t popq %1\n\t movl %%ecx, %0"                 \
                      : "=&r"(_i), "=&r"(_f) : "r"(A), "r"(B)                 \
                      : "xmm1","rcx","cc","memory");                          \
    *(IdxOut) = _i; *(FlOut) = _f; } while (0)

// SSE4.1 - these execute NATIVELY on a Penryn, so the hardware side is silicon.
STATIC VOID Sse41RR (UINT8 *A, UINT8 *B, UINT8 *O, UINTN Which)
{
  switch (Which) {
    case 0: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t pmulld %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 1: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t pcmpeqq %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 2: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t packusdw %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 3: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t pminsd %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 4: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t pmaxud %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 5: __asm__ volatile ("movdqu (%1),%%xmm2\n\t phminposuw %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 6: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t pblendw $0xA5,%%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 7: __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t mpsadbw $0x03,%%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    case 8: __asm__ volatile ("movdqu (%1),%%xmm2\n\t pmovsxbw %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                              : : "r"(A),"r"(B),"r"(O) : "xmm1","xmm2","memory"); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// The test groups.  Each paints its marker before running, so a machine that
// dies mid-group names the case on its last screenshot.
//
// Ids are GGCC: group * 100 + case.
// ---------------------------------------------------------------------------

STATIC UINT8  mA[16], mB[16], mHw[16], mRef[16];
STATIC UINT64 mMem[64];

STATIC
VOID
GroupCpuid (
  VOID
  )
{
  UINT32  Eax, Ebx, Ecx, Edx;

  Mark (100);
  SerialStr ("[SSE] g1 cpuid\r\n");
  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);

  //
  // As a guest we must see all three, because the hypervisor promised them.  A
  // failure here means the spoof is not reaching this processor at all, and
  // every group below would then be testing nothing.
  //
  Check (101, 1, (Ecx >> 19) & 1);    // SSE4.1
  Check (102, 1, (Ecx >> 20) & 1);    // SSE4.2
  Check (103, 1, (Ecx >> 23) & 1);    // POPCNT
  Paint (5, Ecx);
}

STATIC
VOID
GroupPopcnt (
  VOID
  )
{
  UINTN   I;
  UINT64  Fl;

  Mark (200);
  SerialStr ("[SSE] g2 popcnt\r\n");

  for (I = 0; I < 64; I++) {
    UINT64  V = (I < 4) ? (UINT64)I : Rnd ();

    Mark (200 + I);
    Check (201, RefPopcnt (V, 8), PopcntQ (V));
    Check (202, RefPopcnt (V, 4), PopcntL ((UINT32)V));
    Check (203, RefPopcnt (V, 2), PopcntW ((UINT16)V));

    mMem[0] = V;
    mMem[1] = V >> 3;
    Check (204, RefPopcnt (V, 8), PopcntQM (&mMem[0]));
    Check (205, RefPopcnt (V, 4), PopcntLM ((UINT32 *)&mMem[0]));

    //
    // Addressing modes: disp8, disp32 and SIB.  Each reads a DIFFERENT slot, so
    // a mis-decoded displacement or scale shows up as a wrong answer rather
    // than as luck.
    //
    CopyMem (&mMem[1], &V, 8);             // +8  -> disp8 target
    Check (206, RefPopcnt (V, 4), PopcntLD8 ((UINT32 *)&mMem[0]));

    {
      UINT64  W = V ^ 0x5555555555555555ULL;

      CopyMem ((UINT8 *)&mMem[0] + 260, &W, 4);
      Check (207, RefPopcnt (W, 4), PopcntLD32 ((UINT32 *)&mMem[0]));

      // 4(%base,%index,4) with index 3 -> byte offset 16
      CopyMem ((UINT8 *)&mMem[0] + 16, &W, 4);
      Check (208, RefPopcnt (W, 4), PopcntLSib ((UINT32 *)&mMem[0], 3));
    }

    //
    // ZF is the only flag POPCNT defines, and the emulator sets RFLAGS by hand.
    //
    (VOID)PopcntQFlags (V, &Fl);
    Check (209, (V == 0) ? 1 : 0, (Fl >> 6) & 1);
  }
}

STATIC
VOID
GroupCrc32 (
  VOID
  )
{
  UINTN  I;

  Mark (300);
  SerialStr ("[SSE] g3 crc32\r\n");

  for (I = 0; I < 64; I++) {
    UINT64  V   = Rnd ();
    UINT32  Seed = (UINT32)(V >> 32);
    UINT32  Ref;
    UINTN   K;

    Mark (300 + I);

    Ref = RefCrc32c (Seed, (UINT8)V);
    Check (301, Ref, Crc32B (Seed, (UINT8)V));

    Ref = Seed;
    for (K = 0; K < 2; K++) { Ref = RefCrc32c (Ref, (UINT8)(V >> (8 * K))); }
    Check (302, Ref, Crc32W (Seed, (UINT16)V));

    Ref = Seed;
    for (K = 0; K < 4; K++) { Ref = RefCrc32c (Ref, (UINT8)(V >> (8 * K))); }
    Check (303, Ref, Crc32L (Seed, (UINT32)V));

    Ref = Seed;
    for (K = 0; K < 8; K++) { Ref = RefCrc32c (Ref, (UINT8)(V >> (8 * K))); }
    //
    // CRC32 r64, r/m64 accumulates eight bytes and zero-extends the 32-bit
    // result into the full destination.
    //
    Check (304, (UINT64)Ref, Crc32Q ((UINT64)Seed, V));

    mMem[0] = V;
    Check (305, RefCrc32c (Seed, (UINT8)V), Crc32BM (Seed, (UINT8 *)&mMem[0]));

    Ref = Seed;
    for (K = 0; K < 4; K++) { Ref = RefCrc32c (Ref, (UINT8)(V >> (8 * K))); }
    Check (306, Ref, Crc32LM (Seed, (UINT32 *)&mMem[0]));

    Ref = Seed;
    for (K = 0; K < 8; K++) { Ref = RefCrc32c (Ref, (UINT8)(V >> (8 * K))); }
    Check (307, (UINT64)Ref, Crc32QM ((UINT64)Seed, &mMem[0]));
  }
}

STATIC
VOID
GroupPcmpGtq (
  VOID
  )
{
  UINTN  I;

  Mark (400);
  SerialStr ("[SSE] g4 pcmpgtq\r\n");

  for (I = 0; I < 48; I++) {
    UINTN  K;

    Mark (400 + I);
    for (K = 0; K < 16; K++) {
      mA[K] = (UINT8)Rnd ();
      mB[K] = (UINT8)Rnd ();
    }
    //
    // Force ties and near-ties: a signed 64-bit compare is where a sign or a
    // half-word mix-up hides.
    //
    if ((I & 3) == 0) { CopyMem (mB, mA, 16); }
    if ((I & 3) == 1) { CopyMem (mB, mA, 8); }

    CopyMem (mRef, mA, 16);
    SseXmmPcmpGtq (mRef, mB);

    PcmpGtqRR (mA, mB, mHw);
    CheckBuf (401, mRef, mHw, 16);

    PcmpGtqRM (mA, mB, mHw);
    CheckBuf (402, mRef, mHw, 16);

    PcmpGtqRex (mA, mB, mHw);
    CheckBuf (403, mRef, mHw, 16);
  }
}

STATIC
VOID
GroupPcmpStr (
  VOID
  )
{
  UINTN  I;

  Mark (500);
  SerialStr ("[SSE] g5 pcmpstr\r\n");

  for (I = 0; I < 40; I++) {
    UINT32   Idx, RefIdx;
    UINT64   Fl, RefFl;
    UINT8    RefM[16];
    UINTN    K;

    Mark (500 + I);
    for (K = 0; K < 16; K++) {
      mA[K] = (UINT8)(Rnd () & 0x7F);
      mB[K] = (UINT8)(Rnd () & 0x7F);
    }
    //
    // Null-terminated shapes, which is what the implicit-length forms are for
    // and where the Equal-Ordered aggregation bug lived.
    //
    if ((I & 1) == 0) {
      mA[(I % 15) + 1] = 0;
      mB[(I % 13) + 1] = 0;
      CopyMem (mB, mA, 4);
    }

#define STR_CASE(IMM)                                                          \
    do {                                                                       \
      RefFl = 0; RefIdx = 0;                                                    \
      SseXmmPcmpStr (mA, mB, (IMM), FALSE, 0, 0, TRUE, &RefIdx, RefM, &RefFl);  \
      ISTRI (IMM, mA, mB, &Idx, &Fl);                                           \
      Check (5100 + (IMM), RefIdx, Idx);                                        \
      Check (5200 + (IMM), RefFl & 0x8D5, Fl & 0x8D5);                          \
      ISTRI_M (IMM, mA, mB, &Idx, &Fl);                                         \
      Check (5300 + (IMM), RefIdx, Idx);                                        \
      RefFl = 0;                                                                \
      ZeroMem (RefM, 16);                                                       \
      SseXmmPcmpStr (mA, mB, (IMM), FALSE, 0, 0, FALSE, &RefIdx, RefM, &RefFl);  \
      ISTRM (IMM, mA, mB, mHw, &Fl);                                            \
      CheckBuf (5400 + (IMM), RefM, mHw, 16);                                   \
      RefFl = 0; RefIdx = 0;                                                    \
      SseXmmPcmpStr (mA, mB, (IMM), TRUE, 5, 7, TRUE, &RefIdx, RefM, &RefFl);    \
      ESTRI (IMM, mA, mB, 5, 7, &Idx, &Fl);                                     \
      Check (5500 + (IMM), RefIdx, Idx);                                        \
    } while (0)

    STR_CASE (0x00);
    STR_CASE (0x04);
    STR_CASE (0x08);
    STR_CASE (0x0C);
    STR_CASE (0x18);
    STR_CASE (0x1A);
    STR_CASE (0x40);
    STR_CASE (0x44);
#undef STR_CASE
  }
}

//
// SSE4.1 runs NATIVELY on a Penryn - the hypervisor is told not to emulate a
// class the host already has - so here the hardware side really is silicon and
// this is a differential test on a microarchitecture the userspace suite has
// never run on.
//
STATIC
VOID
GroupSse41 (
  VOID
  )
{
  UINTN  I;

  Mark (600);
  SerialStr ("[SSE] g6 sse41 native-vs-emulator\r\n");

  for (I = 0; I < 48; I++) {
    UINTN  K, W;

    Mark (600 + I);
    for (K = 0; K < 16; K++) {
      mA[K] = (UINT8)Rnd ();
      mB[K] = (UINT8)Rnd ();
    }

    for (W = 0; W < 9; W++) {
      CopyMem (mRef, mA, 16);
      switch (W) {
        case 0: Sse41Pmulld     (mRef, mB); break;
        case 1: Sse41Pcmpeqq    (mRef, mB); break;
        case 2: Sse41Packusdw   (mRef, mB); break;
        case 3: Sse41MinMax     (mRef, mB, 4, TRUE,  FALSE); break;   // PMINSD
        case 4: Sse41MinMax     (mRef, mB, 4, FALSE, TRUE);  break;   // PMAXUD
        case 5: Sse41Phminposuw (mRef, mB); break;
        case 6: Sse41Pblendw    (mRef, mB, 0xA5); break;
        case 7: Sse41Mpsadbw    (mRef, mB, 0x03); break;
        case 8: Sse41Pmovx      (mRef, mB, 1, 2, TRUE); break;        // PMOVSXBW
        default: break;
      }
      Sse41RR (mA, mB, mHw, W);
      CheckBuf (610 + W, mRef, mHw, 16);
    }
  }
}

// ---------------------------------------------------------------------------

STATIC
VOID
Report (
  VOID
  )
{
  Paint (1, mRun);
  Paint (2, mFail);
  Paint (3, mFirst);
  Paint (4, (mFail != 0) ? (mExp & 0xFFFFFFFF) : 0);
  Paint (5, (mFail != 0) ? (mGot & 0xFFFFFFFF) : 0);
  Paint (6, 999999);                       // finished, did not reset

  SerialStr ("[SSE] run=");   SerialNum (mRun);
  SerialStr (" fail=");       SerialNum (mFail);
  SerialStr (" firstid=");    SerialNum (mFirst);
  SerialStr (" exp=");        SerialNum (mExp);
  SerialStr (" got=");        SerialNum (mGot);
  SerialStr (mFail == 0 ? "\r\n[SSE] RESULT: PASS\r\n" : "\r\n[SSE] RESULT: FAIL\r\n");
}

EFI_STATUS
EFIAPI
SseLiveMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  EFI_DEVICE_PATH_PROTOCOL   *Dp;
  EFI_HANDLE                 HvHandle;
  EFI_MEMORY_DESCRIPTOR      *Map;
  UINTN                      MapSize, MapKey, DescSize, Attempt;
  UINT32                     DescVer;
  UINT32                     Eax, Ebx, Ecx, Edx;

  Print (L"[SSE] loading hypervisor driver\r\n");

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Dp = FileDevicePath (Li->DeviceHandle, L"\\Core2Again.efi");
  if (Dp == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  HvHandle = NULL;
  Status   = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &HvHandle);
  FreePool (Dp);
  if (EFI_ERROR (Status)) {
    Print (L"[SSE] LoadImage: %r\r\n", Status);
    return Status;
  }
  Status = gBS->StartImage (HvHandle, NULL, NULL);
  Print (L"[SSE] driver StartImage: %r\r\n", Status);

  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  Print (L"[SSE] pre-EBS CPUID.1:ECX = %08x  (41=%d 42=%d popcnt=%d)\r\n",
         Ecx, (Ecx >> 19) & 1, (Ecx >> 20) & 1, (Ecx >> 23) & 1);

  {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
    EFI_GUID                       GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    if (!EFI_ERROR (gBS->LocateProtocol (&GopGuid, NULL, (VOID **)&Gop)) &&
        (Gop->Mode != NULL) && (Gop->Mode->FrameBufferBase != 0))
    {
      mFb    = (volatile UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
      mFbPps = Gop->Mode->Info->PixelsPerScanLine;
      mFbW   = Gop->Mode->Info->HorizontalResolution;
      mFbH   = Gop->Mode->Info->VerticalResolution;
    }
  }

  Print (L"[SSE] ExitBootServices - the hypervisor takes over here\r\n");

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
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      break;
    }
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DisableInterrupts ();

  //
  // From here every SSE4.2 / POPCNT instruction below raises #UD on this
  // silicon and is serviced by the driver we just loaded.  There is no IDT, so
  // an un-emulated one is a triple fault and a reset - which is why the marker
  // is painted before each case rather than after.
  //
  SerialStr ("\r\n[SSE] begin\r\n");

  GroupCpuid ();
  GroupPopcnt ();
  GroupCrc32 ();
  GroupPcmpGtq ();
  GroupPcmpStr ();
  GroupSse41 ();

  Report ();

  for (;;) {
    AsmCpuid (0, &Eax, &Ebx, &Ecx, &Edx);   // keep the heartbeat ticking
  }
}
