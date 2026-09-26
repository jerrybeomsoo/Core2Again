/* Test-only stub: just enough of the EDK II environment to build the pure
   emulator files in userspace for differential testing against hardware. */
#ifndef HV_TEST_STUB_H_
#define HV_TEST_STUB_H_
#include <stdint.h>
#include <string.h>

typedef void        VOID;
typedef uint8_t     UINT8;
typedef uint16_t    UINT16;
typedef uint32_t    UINT32;
typedef uint64_t    UINT64;
typedef int8_t      INT8;
typedef int16_t     INT16;
typedef int32_t     INT32;
typedef int64_t     INT64;
typedef uint64_t    UINTN;
typedef int64_t     INTN;
typedef unsigned char BOOLEAN;

#define TRUE   1
#define FALSE  0
#define IN
#define OUT
#define STATIC static

#define BIT0   0x1
#define BIT1   0x2
#define BIT2   0x4
#define BIT3   0x8
#define BIT4   0x10
#define BIT5   0x20
#define BIT6   0x40
#define BIT7   0x80
#define BIT11  0x800

#define CopyMem(d,s,n)  memcpy((d),(s),(n))
#define ZeroMem(d,n)    memset((d),0,(n))
#define SetMem(d,n,v)   memset((d),(v),(n))

VOID SseXmmPcmpGtq (UINT8 *Dst, UINT8 *Src);
VOID SseXmmPcmpStr (UINT8 *Op1, UINT8 *Op2, UINT8 Imm8, BOOLEAN ExplicitLen,
                    INT64 LenRax, INT64 LenRdx, BOOLEAN IndexForm,
                    UINT32 *OutIndex, UINT8 *OutXmm0, UINT64 *Rflags);
VOID Sse41Pmovx     (UINT8 *Dst, UINT8 *Src, UINTN SrcBytes, UINTN DstBytes, BOOLEAN Sign);
VOID Sse41Pcmpeqq   (UINT8 *Dst, UINT8 *Src);
VOID Sse41Pmulld    (UINT8 *Dst, UINT8 *Src);
VOID Sse41Pmuldq    (UINT8 *Dst, UINT8 *Src);
VOID Sse41Packusdw  (UINT8 *Dst, UINT8 *Src);
VOID Sse41MinMax    (UINT8 *Dst, UINT8 *Src, UINTN ElemBytes, BOOLEAN Signd, BOOLEAN IsMax);
VOID Sse41Phminposuw(UINT8 *Dst, UINT8 *Src);
VOID Sse41Ptest     (UINT8 *Dst, UINT8 *Src, UINT64 *Rflags);
VOID Sse41Blendv    (UINT8 *Dst, UINT8 *Src, UINT8 *Mask, UINTN ElemBytes);
VOID Sse41Pblendw   (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Insertps  (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Mpsadbw   (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Round     (UINT8 *Dst, UINT8 *Src, UINT8 Imm8, UINTN ElemBytes, BOOLEAN Scalar);
VOID Sse41Dpps      (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
VOID Sse41Dppd      (UINT8 *Dst, UINT8 *Src, UINT8 Imm8);
UINT64 Sse41GetElem (UINT8 *Src, UINTN Index, UINTN ElemBytes);
VOID Sse41SetElem   (UINT8 *Dst, UINTN Index, UINTN ElemBytes, UINT64 Val);
#endif
