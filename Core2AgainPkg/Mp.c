/** @file
  Mp.c - Bring every logical processor into VMX root operation.

  APs are still parked in the firmware's MP wait loop at DXE time, so
  EFI_MP_SERVICES_PROTOCOL.StartupThisAP wakes them via the existing loop (a
  memory hand-off, NOT INIT/SIPI) to run our bring-up procedure.  Each AP
  snapshots its own context and VMLAUNCHes, "returning" into the firmware wait
  loop now virtualized.  The BSP virtualizes itself last, after all APs, so it
  is never a guest while orchestrating.

  Per-CPU regions are pre-allocated on the BSP; the AP procedure performs no
  UEFI allocations (UEFI boot services are not AP-reentrant) - only MSR/CR/VMX
  operations that touch this CPU's own state.
**/

#include "Hypervisor.h"
#include <Protocol/MpService.h>

STATIC VMX_VCPU  gVcpus[HV_MAX_CPUS];

/**
  The per-CPU state for one logical processor, or NULL if the index is out of
  range.  ApStartup.c reaches an AP's regions through this rather than sharing
  the array, so the allocation policy stays in one place.
**/
VMX_VCPU *
HvGetVcpu (
  IN UINTN  Index
  )
{
  return (Index < HV_MAX_CPUS) ? &gVcpus[Index] : NULL;
}

//
// Runs on the target CPU (BSP or AP).  Never allocates; never calls gBS.
//
VOID
HvBringUpThisCpu (
  IN VMX_VCPU  *Vcpu
  )
{
  UINTN  LaunchResult;
  UINTN  VmError;

  HvMark (13, '<');                        // before VMXON
  if (EFI_ERROR (VmxEnterRootMode (Vcpu))) {
    HvMark (18, '!');                      // VMXON/root failed
    return;
  }
  HvMark (14, 'R');                        // in VMX root
  if (EFI_ERROR (VmxSetupVmcs (Vcpu))) {
    HvMark (18, '?');                      // VMCS setup failed
    AsmVmxOff ();
    return;
  }
  HvMark (15, 'M');                        // VMCS programmed

  HvMark (16, 'G');                        // about to VMLAUNCH
  LaunchResult = AsmVmxLaunch ();
  if (LaunchResult != 0) {
    VmError = 0;
    AsmVmRead (VMCS_VM_INSTRUCTION_ERROR, &VmError);
    HvMark  (18, 'F');                     // VMLAUNCH failed
    HvMarkN (HV_BAND_BASE + 3, (UINTN)VmError);   // row 3: VM-instruction error
    AsmVmxOff ();
    return;
  }

  //
  // From here down we are the guest.  If the exit path is broken, band 17 may
  // not appear (the first exit faults before this line runs virtualized).
  //
  HvMark (17, 'g');                        // launched; running as guest
  Vcpu->Launched = TRUE;
  AsmAtomicInc (&gHvCpusLaunched);
}

STATIC UINTN  gBspIndex = 0;
STATIC UINTN  gNumCpus  = 1;

//
// Incremented by each CPU that completes VMLAUNCH (see BringUpThisCpu).
//
volatile UINTN  gHvCpusLaunched = 0;

volatile UINT32  gHvBspApicId = 0;

/**
  Driver-load phase: allocate every CPU's regions and virtualize the APs only.

  The BSP is deliberately LEFT un-virtualized here and deferred to
  ExitBootServices (VmxVirtualizeBspNow). A BIOS-hosted UEFI environment may
  perform real-mode BIOS thunks (VBE/INT13) to draw and read disks. This CPU has
  no "unrestricted guest", so a virtualized BSP cannot enter real mode. By
  virtualizing the BSP only when the OS loader leaves boot services, firmware
  runs natively and Windows runs virtualized.

  APs idle in the firmware wait loop (no thunks), so virtualizing them now is
  safe; they take their INIT/SIPI exits later when the OS starts them.
**/
EFI_STATUS
VmxPrepareAndVirtualizeAps (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_MP_SERVICES_PROTOCOL  *Mp;
  UINTN                      NumEnabled;
  UINT32                     RegEax, RegEbx, RegEcx, RegEdx;

  //
  // We are the BSP here (driver load runs on it).  Remember which APIC ID that
  // is so the exit handler can tell "this is the BSP" apart from an AP later.
  //
  AsmCpuid (1, &RegEax, &RegEbx, &RegEcx, &RegEdx);
  gHvBspApicId = RegEbx >> 24;

  Status = gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp);
  if (EFI_ERROR (Status)) {
    //
    // Uniprocessor / no MP services: only the BSP exists; allocate it and let
    // ExitBootServices virtualize it.
    //
    DEBUG ((DEBUG_WARN, "[HV] No MP services; BSP-only (deferred).\n"));
    gNumCpus  = 1;
    gBspIndex = 0;
    gVcpus[0].CpuIndex = 0;
    return VmxAllocRegions (&gVcpus[0]);
  }

  Mp->GetNumberOfProcessors (Mp, &gNumCpus, &NumEnabled);
  Mp->WhoAmI (Mp, &gBspIndex);

  //
  // The BSP always occupies vCPU slot 0 regardless of the index MP services
  // reports for it.  These slots are ours, not the firmware's: ApStartup.c hands
  // APs slots 1..n, and a BSP sitting at, say, slot 3 because WhoAmI said so
  // would collide with the third AP.
  //
  DEBUG ((DEBUG_INFO, "[HV] MP services report BSP index %lu; using slot 0.\n",
          (UINT64)gBspIndex));
  gBspIndex = 0;
  if (gNumCpus > HV_MAX_CPUS) {
    gNumCpus = HV_MAX_CPUS;
  }
  DEBUG ((DEBUG_INFO, "[HV] %lu logical processors (%lu enabled), BSP=%lu\n",
          (UINT64)gNumCpus, (UINT64)NumEnabled, (UINT64)gBspIndex));

  //
  // Allocate only the BSP's regions.
  //
  gVcpus[gBspIndex].CpuIndex = gBspIndex;
  Status = VmxAllocRegions (&gVcpus[gBspIndex]);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // APs are deliberately NOT started through MP services.  When they were, a
  // virtualized AP "returned" into the firmware's wait loop - boot-services
  // memory the OS reclaims and overwrites, after which the AP executed whatever
  // Windows had written there, crashing at random low addresses.  ApStartup.c
  // wakes them with our own INIT-SIPI-SIPI instead, onto a stack and a park
  // loop we own, at ExitBootServices.
  //
  return EFI_SUCCESS;
}

/**
  ExitBootServices phase: virtualize the BSP.

  Runs as the OS loader's BSP, in long mode, with no BIOS thunks ahead.  Performs
  NO memory allocation (everything was pre-allocated at load), so the loader's
  memory-map key stays valid across this call.
**/
VOID
VmxVirtualizeBspNow (
  VOID
  )
{
  HvBringUpThisCpu (&gVcpus[gBspIndex]);
}
