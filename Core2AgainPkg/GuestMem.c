/** @file
  GuestMem.c - reaching into the guest's address space from the host.

  The host runs identity-mapped (that is what UEFI hands us and what
  BuildHostCr3 preserves), so a guest-PHYSICAL address is simply dereferenced.
  A guest-LINEAR address has to be walked through the guest's own four-level
  page tables first, which is what everything here is for: the #UD decoder needs
  the instruction bytes and its memory operands, and the exit handler needs the
  opcode bytes of an instruction it could not emulate.

  Every walk can legitimately fail.  A guest page may simply be paged out - #UD
  is raised BEFORE the operand is evaluated, so an instruction we are asked to
  emulate can perfectly well name an absent operand.  Callers must treat FALSE
  as "the guest is owed a #PF at this address", never as "bad instruction".
**/

#include "Hypervisor.h"

#define GUEST_CR3_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_ADDR_MASK        0x000FFFFFFFFFF000ULL
#define PTE_PRESENT          BIT0
#define PTE_PAGE_SIZE        BIT7                  // 1 GiB or 2 MiB leaf

/**
  Walk the guest's page tables.  Handles 1 GiB and 2 MiB leaves as well as
  4 KiB pages; returns FALSE at the first non-present level.
**/
STATIC
BOOLEAN
GuestVaToPa (
  IN  UINT64  Cr3,
  IN  UINT64  Va,
  OUT UINT64  *Pa
  )
{
  UINT64  Table = Cr3 & GUEST_CR3_ADDR_MASK;
  UINT64  Entry;
  UINTN   Level;

  //
  // PML4 -> PDPT -> PD -> PT.  Shift 39, 30, 21, 12; the middle two levels may
  // terminate early in a large-page leaf.
  //
  for (Level = 0; Level < 4; Level++) {
    UINTN  Shift = 39 - Level * 9;

    Entry = *(volatile UINT64 *)(UINTN)(Table + ((Va >> Shift) & 0x1FF) * 8);
    if ((Entry & PTE_PRESENT) == 0) {
      return FALSE;
    }

    if (Level == 3) {
      break;                                       // 4 KiB PTE
    }
    if ((Level > 0) && ((Entry & PTE_PAGE_SIZE) != 0)) {
      UINT64  PageMask = (1ULL << Shift) - 1;      // 1 GiB or 2 MiB

      *Pa = (Entry & PTE_ADDR_MASK & ~PageMask) | (Va & PageMask);
      return TRUE;
    }
    Table = Entry & PTE_ADDR_MASK;
  }

  *Pa = (Entry & PTE_ADDR_MASK) | (Va & 0xFFF);
  return TRUE;
}

//
// Copy between a guest-linear range and a host buffer, one page span at a time.
// Read and write differ only in the direction of the CopyMem, and having had
// them as two near-identical loops was one bug fix away from drifting apart.
//
STATIC
BOOLEAN
GuestCopyLinear (
  IN UINT64   Cr3,
  IN UINT64   Va,
  IN VOID    *Buffer,
  IN UINTN    Length,
  IN BOOLEAN  Write
  )
{
  UINT8  *Host = (UINT8 *)Buffer;

  while (Length > 0) {
    UINT64  Pa;
    UINTN   InPage, Chunk;

    if (!GuestVaToPa (Cr3, Va, &Pa)) {
      return FALSE;
    }

    InPage = SIZE_4KB - (UINTN)(Va & (SIZE_4KB - 1));
    Chunk  = (Length < InPage) ? Length : InPage;

    if (Write) {
      CopyMem ((VOID *)(UINTN)Pa, Host, Chunk);
    } else {
      CopyMem (Host, (VOID *)(UINTN)Pa, Chunk);
    }

    Va     += Chunk;
    Host   += Chunk;
    Length -= Chunk;
  }
  return TRUE;
}

BOOLEAN
GuestReadLinear (
  IN  UINT64  Cr3,
  IN  UINT64  Va,
  OUT VOID   *Buffer,
  IN  UINTN   Length
  )
{
  return GuestCopyLinear (Cr3, Va, Buffer, Length, FALSE);
}

BOOLEAN
GuestWriteLinear (
  IN UINT64  Cr3,
  IN UINT64  Va,
  IN VOID   *Buffer,
  IN UINTN   Length
  )
{
  return GuestCopyLinear (Cr3, Va, Buffer, Length, TRUE);
}

//
// ---------------------------------------------------------------------------
// Guest general-purpose registers.
//
// GUEST_REGS overlays the frame AsmVmExitHandler pushed, in architectural
// register-number order, so it indexes as an array.  Index 4 is the exception:
// the frame holds a placeholder there and the architectural guest RSP lives in
// the VMCS.
// ---------------------------------------------------------------------------
//
UINT64
HvReadGpr (
  IN GUEST_REGS  *Regs,
  IN UINT32       Index
  )
{
  UINTN  Rsp;

  if ((Index & 0xF) == 4) {
    AsmVmRead (VMCS_GUEST_RSP, &Rsp);
    return (UINT64)Rsp;
  }
  return ((UINT64 *)Regs)[Index & 0xF];
}

VOID
HvWriteGpr (
  IN OUT GUEST_REGS  *Regs,
  IN     UINT32       Index,
  IN     UINT64       Value
  )
{
  if ((Index & 0xF) == 4) {
    AsmVmWrite (VMCS_GUEST_RSP, (UINTN)Value);
    return;
  }
  ((UINT64 *)Regs)[Index & 0xF] = Value;
}
