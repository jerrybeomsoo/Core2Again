/** @file
  Display.c - the only instrument this hypervisor has.

  There is no debugger on the target, DebugLib is the null instance, and after
  ExitBootServices there is no console either.  What remains is the linear
  framebuffer: a physical address that stays valid once the OS takes over, which
  the host can write to from inside a VM-exit without calling anything.

  Everything here is therefore deliberately dependency-free after
  initialisation - the GOP geometry is cached as plain scalars while boot
  services are alive, and nothing below touches gBS again.

  Two primitives are exported:
    HvMark  (Col, Ch)   a checkpoint: one bar in the left-hand trail, the
                        number painted large at the top, and a VGA text cell
                        for the legacy case.
    HvMarkN (Band, N)   a value, as a large decimal, on one of nine rows under
                        the checkpoint number.
  The heartbeat (Diagnostics.c) is nine HvMarkN calls; every failure path in the
  hypervisor is an HvMark plus a few HvMarkN.
**/

#include "Hypervisor.h"
#include <Protocol/GraphicsOutput.h>

#if HV_DIAG_ENABLED


STATIC EFI_GUID  mGopGuid = { 0x9042a9de, 0x23dc, 0x4a38,
                              { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

//
// Framebuffer geometry, cached the first time we draw (while boot services are
// alive).  After ExitBootServices the GOP protocol struct may be freed, but the
// linear framebuffer physical address stays valid, so the exit handler keeps
// drawing from these scalars without touching gBS.
//
STATIC volatile UINT32  *mFbBase  = NULL;
STATIC UINTN             mFbW     = 0;
STATIC UINTN             mFbH     = 0;
STATIC UINTN             mFbPps   = 0;
STATIC BOOLEAN           mFbReady = FALSE;
STATIC BOOLEAN           mFbTried = FALSE;

//
// 5x7 bitmap font for the decimal digits.  Each row uses the low 5 bits; bit
// 0x10 is the leftmost pixel.  Scaled up so a phone photo of the panel is
// legible, which is how these numbers are actually read.
//
STATIC CONST UINT8  mFont[10][GLYPH_ROWS] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   // 0
  { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   // 1
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   // 2
  { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   // 3
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   // 4
  { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   // 5
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   // 6
  { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   // 7
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   // 8
  { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   // 9
};

//
// Resolve the GOP linear framebuffer, once.  FALSE when there is no usable
// direct-write framebuffer (a Blt-only GOP, or no GOP at all), in which case
// every drawing routine below becomes a no-op and the VGA text cell in HvMark
// is all that remains.
//
STATIC
BOOLEAN
FbReady (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  if (mFbReady) {
    return TRUE;
  }
  if (mFbTried) {
    return FALSE;
  }

  //
  // One attempt only.  This runs from the VM-exit path too, and after
  // ExitBootServices gBS is gone - a second LocateProtocol there would fault in
  // host context, where nothing can catch it.
  //
  mFbTried = TRUE;
  Gop      = NULL;
  gBS->LocateProtocol (&mGopGuid, NULL, (VOID **)&Gop);

  if ((Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL) ||
      (Gop->Mode->FrameBufferBase == 0) ||
      (Gop->Mode->Info->PixelFormat == PixelBltOnly)) {
    return FALSE;
  }

  mFbBase  = (volatile UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
  mFbW     = Gop->Mode->Info->HorizontalResolution;
  mFbH     = Gop->Mode->Info->VerticalResolution;
  mFbPps   = Gop->Mode->Info->PixelsPerScanLine;
  mFbReady = TRUE;
  return TRUE;
}

//
// Fill a rectangle, clipped once against the screen rather than per pixel.
//
// The heartbeat repaints nine numbers over a ~360x44 strip each, so this is the
// hot loop of the whole display; hoisting the bounds test out of it is the
// difference between two compares per pixel and none.
//
STATIC
VOID
FillRect (
  IN UINTN   X0,
  IN UINTN   Y0,
  IN UINTN   Wid,
  IN UINTN   Hgt,
  IN UINT32  Color
  )
{
  volatile UINT32  *Row;
  UINTN             X, Y, X1, Y1;

  if (!FbReady () || (X0 >= mFbW) || (Y0 >= mFbH)) {
    return;
  }

  X1 = X0 + Wid;
  Y1 = Y0 + Hgt;
  if (X1 > mFbW) { X1 = mFbW; }
  if (Y1 > mFbH) { Y1 = mFbH; }

  Row = mFbBase + Y0 * mFbPps;
  for (Y = Y0; Y < Y1; Y++, Row += mFbPps) {
    for (X = X0; X < X1; X++) {
      Row[X] = Color;
    }
  }
}

//
// Draw one digit at (X0,Y0), scaled GLYPH_SCALE-fold.
//
// The pixels are written directly rather than through one FillRect per lit font
// pixel.  That called FbReady and redid the clipping arithmetic about seventeen
// times per digit, to emit runs of GLYPH_SCALE pixels each - all of it setup
// for a handful of stores.  The heartbeat paints up to ninety digits.
//
STATIC
VOID
DrawDigit (
  IN UINTN   X0,
  IN UINTN   Y0,
  IN UINTN   Digit,
  IN UINT32  Color
  )
{
  UINTN  R, C, Sx, Sy;

  if ((Digit > 9) || !FbReady ()) {
    return;
  }

  for (R = 0; R < GLYPH_ROWS; R++) {
    UINT8  Bits = mFont[Digit][R];

    if (Bits == 0) {
      continue;
    }
    for (Sy = 0; Sy < GLYPH_SCALE; Sy++) {
      volatile UINT32  *Row;
      UINTN             Y = Y0 + R * GLYPH_SCALE + Sy;

      if (Y >= mFbH) {
        return;                       // rows only increase; nothing left to draw
      }
      Row = mFbBase + Y * mFbPps;

      for (C = 0; C < GLYPH_COLS; C++) {
        if ((Bits & (0x10 >> C)) == 0) {
          continue;
        }
        for (Sx = 0; Sx < GLYPH_SCALE; Sx++) {
          UINTN  X = X0 + C * GLYPH_SCALE + Sx;

          if (X < mFbW) {
            Row[X] = Color;
          }
        }
      }
    }
  }
}

//
// Draw an unsigned decimal at (X0,Y0), clearing the strip first so a smaller
// number never leaves stale digits behind it.
//
STATIC
VOID
DrawNumber (
  IN UINTN   X0,
  IN UINTN   Y0,
  IN UINTN   Value,
  IN UINT32  Color
  )
{
  UINT8  Digits[HV_NUM_MAX_DIGITS];
  UINTN  Count, I, X;

  if (!FbReady ()) {
    return;
  }
  FillRect (X0, Y0, GLYPH_CELL * HV_NUM_MAX_DIGITS, GLYPH_ROWS * GLYPH_SCALE + 2,
            0x00000000);

  Count = 0;
  do {
    Digits[Count++] = (UINT8)(Value % 10);
    Value /= 10;
  } while ((Value > 0) && (Count < HV_NUM_MAX_DIGITS));

  X = X0;
  for (I = Count; I > 0; I--) {
    DrawDigit (X, Y0, Digits[I - 1], Color);
    X += GLYPH_CELL;
  }
}

/**
  Record progress at checkpoint Col.

  Three things happen, because which of them is visible depends on where the
  boot has got to: a VGA text cell (legacy text mode, if active), one bar in a
  trail down the left edge (how far we got), and the checkpoint number painted
  large near the top.  The big number updates in place, so whatever it shows
  when the machine freezes is the last checkpoint reached.
**/
VOID
HvMark (
  IN UINTN  Col,
  IN CHAR8  Ch
  )
{
  volatile UINT16  *Vga = (volatile UINT16 *)(UINTN)0xB8000;

  Vga[Col] = (UINT16)(0x2F00 | (UINT8)Ch);

  FillRect (0, Col * 22, 28, 20, 0x00FFFFFF);          // trail bar
  DrawNumber (HV_NUM_X, 6, Col, 0x00FFFFFF);           // current checkpoint
}

/**
  Show a value as a large yellow decimal on row (Band - 15), under the
  checkpoint number.  Nine rows are available; the heartbeat uses all of them
  and the failure paths reuse them with their own meanings.
**/
VOID
HvMarkN (
  IN UINTN  Band,
  IN UINTN  N
  )
{
  UINTN  Row = (Band >= HV_BAND_BASE) ? (Band - HV_BAND_BASE) : 0;

  if (Row >= HV_DIAG_ROWS) {
    Row = HV_DIAG_ROWS - 1;
  }
  DrawNumber (HV_NUM_X, 60 + Row * (GLYPH_ROWS * GLYPH_SCALE + 4), N, 0x0000FFFF);
}

#endif // HV_DIAG_ENABLED
