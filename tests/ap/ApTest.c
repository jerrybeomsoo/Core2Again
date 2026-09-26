/** @file
  ApTest.c - exercise the AP startup trampoline where a fault can be seen.

  WHY THIS EXISTS
  ---------------
  The trampoline in scripts/ap_trampoline.S runs on a processor with no IDT, so
  every mistake in it is a triple fault, and a triple fault is a silent machine
  reset: no bugcheck, no log, nothing on screen.  Debugging it on the target
  machine costs one reboot per hypothesis and returns one bit of information.

  This application performs exactly the sequence HvStartAps performs - stamp a
  low page with the trampoline, INIT-SIPI-SIPI to all-excluding-self, wait for
  the AP to arrive in C - but as a plain UEFI application, so it can be run
  under QEMU.  There a triple fault is a `qemu: fatal:` line naming the
  exception, the faulting CS:EIP and every register, and an iteration costs
  thirty seconds instead of a reboot.

  It deliberately does NOT bring VMX up on the AP.  The trampoline is what has
  historically been broken and it is what this isolates; the arrival count and
  the progress breadcrumb tell you which of the two it was.

  The trampoline bytes and the page layout come from the generated
  Core2AgainPkg/ApTrampoline.h - the same header the driver includes - so
  this cannot end up testing a different trampoline from the one that ships.

  Run it with tests/ap/run.sh.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CpuLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#include "../../Core2AgainPkg/ApTrampoline.h"

#define AP_MAX_STARTED  7
#define AP_STACK_SIZE   SIZE_8KB

#define APIC_BASE_MSR   0x0000001B
#define   APIC_BASE_EXTD    BIT10
#define   APIC_BASE_ENABLE  BIT11
#define APIC_ICR_LOW    0x300
#define   ICR_DELIVERY_PENDING  BIT12

//
// Written by APs, read by the BSP.  Volatile because the only thing that
// updates them is another processor.
//
//
// One byte per slot, written only by the processor that owns that slot, so
// there is no shared read-modify-write to lose.
//
// This used to be `mArrived++` and `mSlotMask |= 1 << Slot`, both of which race:
// at four processors it reported "arrived 2, slot mask 7" - three APs had
// plainly arrived and set their own bit, while two increments landed on top of
// each other.  That looked exactly like an AP dying in the trampoline and sent
// me looking for a hypervisor bug that was not there.  It is the same race as
// bug 15 in the driver, which was fixed there and not here.
//
#define AP_SEEN_MAX  16
STATIC volatile UINT8  mSeen[AP_SEEN_MAX];

/**
  What an AP calls once the trampoline has it in long mode on its own stack.
  Microsoft x64 convention, because that is what the trampoline emits.
**/
STATIC
VOID
EFIAPI
ApTestEntry (
  IN UINTN  Slot
  )
{
  if (Slot < AP_SEEN_MAX) {
    mSeen[Slot] = 1;
  }

  //
  // Park.  Returning would fall off the end of the trampoline.
  //
  for (;;) {
    DisableInterrupts ();
    CpuSleep ();
  }
}

STATIC
volatile UINT32 *
ApicIcr (
  VOID
  )
{
  UINT64  Base = AsmReadMsr64 (APIC_BASE_MSR);

  if ((Base & APIC_BASE_EXTD) != 0) {
    return NULL;                      // x2APIC: the MMIO window is dead
  }

  if ((Base & APIC_BASE_ENABLE) == 0) {
    AsmWriteMsr64 (APIC_BASE_MSR, Base | APIC_BASE_ENABLE);
    Base = AsmReadMsr64 (APIC_BASE_MSR);
    if ((Base & APIC_BASE_ENABLE) == 0) {
      return NULL;
    }
  }

  return (volatile UINT32 *)(UINTN)((Base & 0x000FFFFFFFFFF000ULL) + APIC_ICR_LOW);
}

STATIC
VOID
ApicSendIpi (
  IN volatile UINT32  *Icr,
  IN UINT32            Command
  )
{
  UINTN  Spin;

  for (Spin = 0; (Spin < 1000000) && ((*Icr & ICR_DELIVERY_PENDING) != 0); Spin++) {
    CpuPause ();
  }
  *Icr = Command;
  for (Spin = 0; (Spin < 1000000) && ((*Icr & ICR_DELIVERY_PENDING) != 0); Spin++) {
    CpuPause ();
  }
}

EFI_STATUS
EFIAPI
ApTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  volatile UINT32       *Icr;
  UINT8                 *Page;
  VOID                  *Stacks;
  UINT64                Cr3;
  UINT32                Vector;
  UINTN                 I;

  Print (L"\r\n=== AP trampoline test ===\r\n");

  //
  // The SIPI vector *is* the page frame number and it is eight bits, so the
  // trampoline has to live below 1 MiB.
  //
  Addr   = 0x00100000;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesCode, 1, &Addr);
  if (EFI_ERROR (Status)) {
    Print (L"RESULT: FAIL no page below 1MB (%r)\r\n", Status);
    return Status;
  }

  Stacks = AllocatePages (EFI_SIZE_TO_PAGES ((AP_MAX_STARTED + 1) * AP_STACK_SIZE));
  if (Stacks == NULL) {
    Print (L"RESULT: FAIL no stacks\r\n");
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (Stacks, (AP_MAX_STARTED + 1) * AP_STACK_SIZE);

  Cr3 = AsmReadCr3 ();
  if (Cr3 >= BASE_4GB) {
    Print (L"RESULT: FAIL CR3 %lx is above 4GB\r\n", Cr3);
    return EFI_UNSUPPORTED;
  }

  Icr = ApicIcr ();
  if (Icr == NULL) {
    Print (L"RESULT: FAIL no usable xAPIC ICR\r\n");
    return EFI_UNSUPPORTED;
  }

  Page   = (UINT8 *)(UINTN)Addr;
  Vector = (UINT32)(Addr >> 12);

  //
  // StopAt 0: run the whole path, including the call into C.  Nothing here can
  // reset a virtual machine in a way that costs anything.
  //
  HvApStampPage (
    Page,
    Cr3,
    (UINT64)(UINTN)Stacks,
    AP_STACK_SIZE,
    (UINT64)(UINTN)ApTestEntry,
    0
    );

  Print (L"page      %lx  (SIPI vector %02x)\r\n", Addr, Vector);
  Print (L"cr3       %lx\r\n", Cr3);
  Print (L"stacks    %lx\r\n", (UINT64)(UINTN)Stacks);
  Print (L"entry     %lx\r\n", (UINT64)(UINTN)ApTestEntry);
  Print (L"apic base %lx\r\n", AsmReadMsr64 (APIC_BASE_MSR));
  Print (L"first 4   %08x  (expect c88cfcfa)\r\n", *(UINT32 *)Page);
  Print (L"sending INIT-SIPI-SIPI...\r\n");

  ApicSendIpi (Icr, 0x000C4500);                   // INIT, assert, all-excl-self
  gBS->Stall (10000);
  ApicSendIpi (Icr, 0x000C4600 | Vector);          // SIPI
  gBS->Stall (200);
  ApicSendIpi (Icr, 0x000C4600 | Vector);          // SIPI (retry)

  //
  // Wait for the arrival count to stop moving rather than for a fixed time.
  //
  // A fixed 500 ms was not enough under nested virtualization with four
  // processors: an AP would claim its slot in the trampoline - so the slot mask
  // showed it - and reach C after the count had already been read, which looked
  // like an AP that had died in the last thirty bytes of the trampoline.  If one
  // really does fault, the machine is gone and QEMU says why, so waiting longer
  // costs nothing.
  //
  {
    UINTN  Seen = 0, Settled = 0, J;

    for (I = 0; I < 500; I++) {              // up to 5 s
      UINTN  Now = 0;

      gBS->Stall (10000);
      for (J = 0; J < AP_SEEN_MAX; J++) {
        Now += mSeen[J];
      }
      if (Now != Seen) {
        Seen = Now;
        Settled = 0;
      } else if ((Seen > 0) && (++Settled > 50)) {
        break;                               // 500 ms with no new arrival
      }
    }
  }

  Print (L"\r\nprogress  %d   (0 never ran, 1 real, 2 prot32, 3 long, 4 into C)\r\n",
         (UINTN)*(Page + AP_OFF_PROG));
  Print (L"slot next %ld\r\n", *(UINT64 *)(Page + AP_OFF_SLOT));
  {
    UINTN  Count = 0, Mask = 0, J;

    for (J = 0; J < AP_SEEN_MAX; J++) {
      if (mSeen[J] != 0) {
        Count++;
        Mask |= (UINTN)1 << J;
      }
    }
    Print (L"arrived   %ld  (slot mask %lx)\r\n", (UINT64)Count, (UINT64)Mask);

    if (Count > 0) {
      Print (L"RESULT: PASS %ld AP(s) reached C\r\n", (UINT64)Count);
    } else {
      Print (L"RESULT: FAIL no AP arrived, progress %d\r\n", (UINTN)*(Page + AP_OFF_PROG));
    }
  }

  //
  // Power off rather than returning to the boot manager: this runs as the
  // removable-media boot option, so returning just leaves QEMU idling at a menu
  // until the harness times out.
  //
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  return EFI_SUCCESS;
}
