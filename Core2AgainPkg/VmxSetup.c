/** @file
  VmxSetup.c - VMCS programming, shared resources, and (optional) EPT.

  Capability-aware: on Nehalem+ we enable secondary controls + EPT; on pre-EPT
  parts such as the Core 2 / Q6600 there are no secondary processor-based
  controls at all, so the guest runs with pass-through paging (guest-physical ==
  host-physical, no second-level translation). Either way the guest is the live
  UEFI context, snapshotted so that VMLAUNCH resumes execution transparently.
**/

#include "Hypervisor.h"
#include <Register/Intel/Cpuid.h>

//
// Shared, read-only-after-init resources (built once on the BSP).
//
STATIC VOID    *gMsrBitmap            = NULL;   // 4 KiB, all zero => no MSR exits
STATIC UINT64   gEptp                 = 0;      // 0 when EPT is unavailable
//
// Private host page tables (identity, 2 MiB leaves).  Not static: the AP
// trampoline loads this same CR3 to reach long mode, so ApStartup.c needs it.
//
UINT64          gHostCr3              = 0;
STATIC UINT64   gPaeIdentityCr3       = 0;   // 4 GiB identity map, PAE format

//
// The revision the BOOTSTRAP processor currently reports.  Application
// processors are made to agree with it; see HandleRdmsr.
//
volatile UINT64  gHvBspMicroRev = 0;

//
// IA32_VMX_CR0/CR4_FIXED0/1.  Constant for the life of the machine, and read on
// every CR0 or CR4 write the guest makes; RDMSR is not free.
//
STATIC UINT64   gCr0Fixed0 = 0, gCr0Fixed1 = 0;
STATIC UINT64   gCr4Fixed0 = 0, gCr4Fixed1 = 0;

UINT64 VmxCr0Fixed0 (VOID) { return gCr0Fixed0; }
UINT64 VmxCr0Fixed1 (VOID) { return gCr0Fixed1; }
UINT64 VmxCr4Fixed0 (VOID) { return gCr4Fixed0; }
UINT64 VmxCr4Fixed1 (VOID) { return gCr4Fixed1; }
STATIC BOOLEAN  gSecondaryAvailable   = FALSE;
STATIC BOOLEAN  gEptAvailable         = FALSE;
STATIC BOOLEAN  gUnrestrictedGuest    = FALSE;

//
// What the *host* CPU natively supports.  An instruction class is emulated
// (i.e. trapped via #UD and decoded) only when the host lacks it; on a CPU that
// already has it, the instruction executes in hardware and never faults.
//   Q6600 (Kentsfield): SSE4.1 no,  SSE4.2 no,  POPCNT no  -> emulate all.
//   Penryn/Yorkfield  : SSE4.1 YES, SSE4.2 no,  POPCNT no  -> emulate SSE4.2/POPCNT only.
//   Nehalem+          : all yes                            -> emulate nothing.
//
STATIC BOOLEAN  gHostHasSse41         = FALSE;
STATIC BOOLEAN  gHostHasSse42         = FALSE;
STATIC BOOLEAN  gHostHasPopcnt        = FALSE;

#define HOST_CS_SEL   0x08
#define HOST_DS_SEL   0x10
#define HOST_TR_SEL   0x18

BOOLEAN VmxEptAvailable              (VOID) { return gEptAvailable; }
BOOLEAN VmxUnrestrictedGuestAvailable(VOID) { return gUnrestrictedGuest; }
UINT64  VmxPaeIdentityCr3(VOID)             { return gPaeIdentityCr3; }

/**
  Read the calling processor's microcode revision.

  IA32_BIOS_SIGN_ID does not hold the revision until a CPUID refreshes it, so
  the documented sequence is: zero the MSR, execute CPUID, read it back.

  Windows compares this value across processors as they come online and
  bugchecks 0x17E MICROCODE_REVISION_MISMATCH when they disagree, so when that
  bugcheck appears the first thing worth knowing is what each processor actually
  reports - which is not guessable and, until this existed, was not visible.
**/
UINT64
HvReadMicrocodeRevision (
  VOID
  )
{
  UINT32  Eax, Ebx, Ecx, Edx;

  AsmWriteMsr64 (MSR_IA32_BIOS_SIGN_ID, 0);
  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  return AsmReadMsr64 (MSR_IA32_BIOS_SIGN_ID);
}

//
// Per-class "should we emulate?" queries used by the decoder.
//
BOOLEAN HvEmulateSse41 (VOID) { return (BOOLEAN)(!gHostHasSse41); }
BOOLEAN HvEmulateSse42 (VOID) { return (BOOLEAN)(!gHostHasSse42); }
BOOLEAN HvEmulatePopcnt(VOID) { return (BOOLEAN)(!gHostHasPopcnt); }


//
// ---------------------------------------------------------------------------
// Per-processor bring-up: does this part have VMX, reserve its regions, and
// enter VMX root operation on it.
//
// These run on whichever processor is being virtualized - the BSP inside the
// ExitBootServices hook, or an AP that has just come out of the trampoline -
// so nothing here may allocate (VmxAllocRegions is the exception and runs only
// on the BSP, at driver load) or call anything that is not AP-reentrant.
// ---------------------------------------------------------------------------
//
/**
  CPUID gate: does this part expose VMX at all?  (Run on the BSP; all logical
  processors share the same feature set.)
**/
EFI_STATUS
VmxCheckSupport (
  VOID
  )
{
  UINT32  RegEcx;

  AsmCpuid (CPUID_VERSION_INFO, NULL, NULL, &RegEcx, NULL);
  if ((RegEcx & BIT5) == 0) {
    DEBUG ((DEBUG_ERROR, "[HV] CPU does not report VMX (CPUID.1:ECX.5)\n"));
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

/**
  Per-CPU allocation.  All buffers are runtime-typed so the OS cannot reclaim
  them after ExitBootServices.  Single-page allocations are 4 KiB aligned,
  which satisfies VMXON/VMCS.  Called on the BSP for every CPU before bring-up.
**/
EFI_STATUS
VmxAllocRegions (
  IN OUT VMX_VCPU  *Vcpu
  )
{
  UINT32  RevisionId;

  Vcpu->VmxonRegion = AllocateRuntimePages (1);
  Vcpu->VmcsRegion  = AllocateRuntimePages (1);
  Vcpu->HostGdt     = AllocateRuntimePages (1);
  Vcpu->HostTss     = AllocateRuntimePages (1);

  //
  // The host stack is aligned to its own size so that masking any address on it
  // yields the HV_PCPU block at its base - which is how the VM-exit handler
  // knows which processor it is running on without executing CPUID.
  //
  Vcpu->HostStack = AllocateAlignedRuntimePages (
                      EFI_SIZE_TO_PAGES (HV_HOST_STACK_SIZE),
                      HV_HOST_STACK_SIZE
                      );

  if ((Vcpu->VmxonRegion == NULL) || (Vcpu->VmcsRegion == NULL) ||
      (Vcpu->HostGdt == NULL) || (Vcpu->HostTss == NULL) ||
      (Vcpu->HostStack == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Vcpu->VmxonRegion, SIZE_4KB);
  ZeroMem (Vcpu->VmcsRegion,  SIZE_4KB);
  ZeroMem (Vcpu->HostStack,   HV_HOST_STACK_SIZE);

  //
  // Published (Magic set) only once the processor itself fills it in, in
  // VmxSetupVmcs - it is the one place that runs ON the target processor.
  //
  Vcpu->Pcpu = (HV_PCPU *)Vcpu->HostStack;

  RevisionId = (UINT32)(AsmReadMsr64 (MSR_IA32_VMX_BASIC) & 0x7FFFFFFF);
  *(UINT32 *)Vcpu->VmxonRegion = RevisionId;
  *(UINT32 *)Vcpu->VmcsRegion  = RevisionId;

  return EFI_SUCCESS;
}

/**
  Enter VMX root operation on the *current* logical processor: enable VMX in
  IA32_FEATURE_CONTROL (per-CPU MSR), satisfy the CR0/CR4 fixed-bit MSRs, set
  CR4.VMXE, then VMXON + VMCLEAR + VMPTRLD.  Safe to call from an AP context.
**/
EFI_STATUS
VmxEnterRootMode (
  IN OUT VMX_VCPU  *Vcpu
  )
{
  UINT64  FeatureControl;
  UINT64  Cr0, Cr4;
  UINT64  VmxonPhys, VmcsPhys;
  UINT8   Status;

  FeatureControl = AsmReadMsr64 (MSR_IA32_FEATURE_CONTROL);
  if ((FeatureControl & FEATURE_CONTROL_LOCK) == 0) {
    AsmWriteMsr64 (MSR_IA32_FEATURE_CONTROL,
                   FeatureControl | FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMXON_OUT_SMX);
  } else if ((FeatureControl & FEATURE_CONTROL_VMXON_OUT_SMX) == 0) {
    DEBUG ((DEBUG_ERROR, "[HV] VMX locked off by firmware on CPU %lu\n",
            (UINT64)Vcpu->CpuIndex));
    return EFI_SECURITY_VIOLATION;
  }

  Cr0  = AsmReadCr0 ();
  Cr0 |= AsmReadMsr64 (MSR_IA32_VMX_CR0_FIXED0);
  Cr0 &= AsmReadMsr64 (MSR_IA32_VMX_CR0_FIXED1);
  AsmWriteCr0 (Cr0);

  Cr4  = AsmReadCr4 () | CR4_VMXE;
  Cr4 |= AsmReadMsr64 (MSR_IA32_VMX_CR4_FIXED0);
  Cr4 &= AsmReadMsr64 (MSR_IA32_VMX_CR4_FIXED1);
  AsmWriteCr4 (Cr4);

  VmxonPhys = (UINT64)(UINTN)Vcpu->VmxonRegion;
  VmcsPhys  = (UINT64)(UINTN)Vcpu->VmcsRegion;

  Status = AsmVmxOn (&VmxonPhys);
  if (Status != VMX_OK) {
    DEBUG ((DEBUG_ERROR, "[HV] VMXON failed on CPU %lu (%u)\n", (UINT64)Vcpu->CpuIndex, Status));
    return EFI_DEVICE_ERROR;
  }

  if ((AsmVmClear (&VmcsPhys) != VMX_OK) || (AsmVmPtrLd (&VmcsPhys) != VMX_OK)) {
    DEBUG ((DEBUG_ERROR, "[HV] VMCLEAR/VMPTRLD failed on CPU %lu\n", (UINT64)Vcpu->CpuIndex));
    AsmVmxOff ();
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

//
// Combine a desired control word with the capability MSR: force "must-be-1"
// bits (low dword) and drop any bit not in "allowed-1" (high dword).
//
STATIC
UINT32
AdjustControls (
  IN UINT32  Desired,
  IN UINT32  CapabilityMsr
  )
{
  UINT64  Cap;

  Cap      = AsmReadMsr64 (CapabilityMsr);
  Desired |= (UINT32)(Cap & 0xFFFFFFFF);
  Desired &= (UINT32)(Cap >> 32);
  return Desired;
}

//
// Build a 1:1 identity map with 2 MiB leaves covering the first 512 GiB, and
// return the physical address of its top-level table (0 on failure).
//
// Both maps this hypervisor needs have exactly this shape and differ only in
// their entry encodings - EPT spells "readable, writeable, executable,
// write-back" where ordinary paging spells "present, read/write" - so they were
// two copies of the same 30 lines, which is one copy too many to keep correct.
//
//  @param  TableFlags  permission bits on the PML4/PDPT entries
//  @param  LeafFlags   permission bits (including the page-size bit) on the
//                      2 MiB leaves
//
STATIC
UINT64
BuildIdentityMap (
  IN UINT64  TableFlags,
  IN UINT64  LeafFlags
  )
{
  UINT64  *Pml4;
  UINT64  *Pdpt;
  UINT64  *PdPool;
  UINTN    I, J;

  Pml4   = AllocateRuntimePages (1);
  Pdpt   = AllocateRuntimePages (1);
  PdPool = AllocateRuntimePages (512);
  if ((Pml4 == NULL) || (Pdpt == NULL) || (PdPool == NULL)) {
    return 0;
  }

  ZeroMem (Pml4, SIZE_4KB);
  ZeroMem (Pdpt, SIZE_4KB);
  ZeroMem (PdPool, 512 * SIZE_4KB);

  Pml4[0] = ((UINT64)(UINTN)Pdpt) | TableFlags;
  for (I = 0; I < 512; I++) {
    UINT64  *Pd = &PdPool[I * 512];

    Pdpt[I] = ((UINT64)(UINTN)Pd) | TableFlags;
    for (J = 0; J < 512; J++) {
      Pd[J] = (((UINT64)I << 30) | ((UINT64)J << 21)) | LeafFlags;
    }
  }
  return (UINT64)(UINTN)Pml4;
}

//
// EPT: R|W|X on the tables, and R|W|X | write-back | 2 MiB page on the leaves.
//
#define EPT_TABLE_FLAGS  0x7ULL
#define EPT_LEAF_FLAGS   (0x7ULL | (6ULL << 3) | (1ULL << 7))

//
// Private host paging, so the host does not depend on firmware page tables that
// vanish at ExitBootServices: P|RW on the tables, P|RW|PS on the leaves.
//
/**
  Build a PAE identity map of the low 4 GiB with 2 MiB pages.

  This is the page table a guest runs on while it believes paging is off.  It
  must be PAE rather than 32-bit because we also force CR4.PAE during that
  window - one format instead of two, and the guest is about to enable PAE
  anyway on its way to long mode.

  A PAE PDPTE is not a normal table entry: bits 2:1 and 8:5 are reserved and
  must be zero, so it carries the address and the present bit only.  Writing
  0x3 there - the value every other level wants - faults the walk.

  @return  Physical address of the PDPT, or 0 on allocation failure.
**/
STATIC
UINT64
BuildPaeIdentityMap (
  VOID
  )
{
  UINT64                *Pdpt;
  UINT64                *PdPool;
  EFI_PHYSICAL_ADDRESS  Addr;
  UINTN                 I, J;

  //
  // Below 4 GiB, explicitly.  With PAE paging and LMA clear, VM entry requires
  // CR3 bits 63:32 to be zero, and a firmware whose memory straddles the 4 GiB
  // line will happily hand back a runtime page above it - which fails entry as
  // "invalid guest state", a verdict that names none of its twenty-odd checks.
  //
  Addr = 0xFFFFFFFFULL;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData,
                                     1, &Addr)))
  {
    return 0;
  }
  Pdpt = (UINT64 *)(UINTN)Addr;

  Addr = 0xFFFFFFFFULL;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData,
                                     4, &Addr)))
  {
    return 0;
  }
  PdPool = (UINT64 *)(UINTN)Addr;
  ZeroMem (Pdpt, SIZE_4KB);
  ZeroMem (PdPool, 4 * SIZE_4KB);

  for (I = 0; I < 4; I++) {
    UINT64  *Pd = &PdPool[I * 512];

    Pdpt[I] = ((UINT64)(UINTN)Pd) | 0x1ULL;          // present; nothing else legal
    for (J = 0; J < 512; J++) {
      Pd[J] = (((UINT64)I << 30) | ((UINT64)J << 21)) | 0x83ULL;   // P|RW|PS
    }
  }
  return (UINT64)(UINTN)Pdpt;
}

#define HOST_TABLE_FLAGS  0x3ULL
#define HOST_LEAF_FLAGS   0x83ULL

/**
  One-time shared setup on the BSP: detect capabilities, allocate the MSR
  bitmap, build EPT (if available) and the private host page tables.
**/
EFI_STATUS
VmxInitShared (
  VOID
  )
{
  UINT64  ProcCap;
  UINT64  Ctls2Cap;
  UINT32  Ecx;

  HvMark (8, '1');   // entered VmxInitShared

  //
  // Probe native ISA support so the decoder only emulates absent classes.
  //
  AsmCpuidEx (CPUID_VERSION_INFO, 0, NULL, NULL, &Ecx, NULL);
  gHostHasSse41  = (Ecx & CPUID1_ECX_SSE41)  != 0;
  gHostHasSse42  = (Ecx & CPUID1_ECX_SSE42)  != 0;
  gHostHasPopcnt = (Ecx & CPUID1_ECX_POPCNT) != 0;

  HvMark (9, '2');   // CPUID caps read

  gMsrBitmap = AllocateRuntimePages (1);
  if (gMsrBitmap == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem (gMsrBitmap, SIZE_4KB);
  //
  // Intercept writes to IA32_EFER (0xC0000080).  Without the VMCS EFER controls
  // - Penryn has none - EFER is one register shared with the host, whose LME is
  // always 1 because the host is in long mode.  So the guest's LME cannot be
  // read back from anywhere, and LME is what decides whether enabling paging
  // means long mode.  Reads are left alone: the trampoline only ORs into what
  // it reads, and passing the real MSR through keeps NXE and SCE intact.
  //
  // Layout: 0x800 begins the write bitmap for 0x00000000-0x00001FFF, 0xC00 the
  // write bitmap for 0xC0000000-0xC0001FFF.  EFER is index 0x80 of the latter.
  //
  ((UINT8 *)gMsrBitmap)[0xC00 + (0x80 / 8)] |= (UINT8)(1u << (0x80 % 8));

  //
  // Intercept reads of IA32_BIOS_SIGN_ID (0x8B) so an application processor can
  // be made to agree with the bootstrap processor's microcode revision; see
  // HandleRdmsr.  Writes pass through - zeroing it is half the read protocol.
  //
  // 0x000 begins the read bitmap for 0x00000000-0x00001FFF.
  //
  ((UINT8 *)gMsrBitmap)[0x000 + (0x8B / 8)] |= (UINT8)(1u << (0x8B % 8));


  HvMark (10, '3');  // MSR bitmap allocated; about to read VMX MSR

  //
  // "Activate secondary controls" is primary proc-based bit 31; only if its
  // allowed-1 bit is set may we read IA32_VMX_PROCBASED_CTLS2 and use EPT etc.
  // Core 2 / Q6600 report this as unavailable -> pass-through, no EPT.
  //
  ProcCap = AsmReadMsr64 (MSR_IA32_VMX_PROCBASED_CTLS);
  gSecondaryAvailable = ((ProcCap >> 32) & PROCBASED_ACTIVATE_SECONDARY) != 0;

  HvMark (11, '4');  // VMX capability MSR read OK; about to build page tables

  if (gSecondaryAvailable) {
    Ctls2Cap = AsmReadMsr64 (MSR_IA32_VMX_PROCBASED_CTLS2) >> 32;
    gEptAvailable      = (Ctls2Cap & PROCBASED2_ENABLE_EPT) != 0;
#ifdef HV_FORCE_NO_EPT
    //
    // Test build: pretend EPT is absent, so a development host exercises the
    // pass-through path the TARGET actually runs.  Every Penryn boot takes this
    // path and no development machine ever did, which made it the largest piece
    // of shipping code with no coverage at all.
    //
    gEptAvailable = FALSE;
#endif
    //
    // Unrestricted guest is only usable WITH EPT - VM entry rejects the pair
    // "UG set, EPT clear" outright - so a CPU that merely supports it does not
    // get it.  Gating on capability alone also made HV_FORCE_NO_EPT a fiction:
    // it cleared EPT and left UG on, so a development host went on running real
    // mode in hardware and never entered the interpreter the target depends on.
    // The one path that build exists to exercise was the one it skipped.
    //
    gUnrestrictedGuest = gEptAvailable &&
                         ((Ctls2Cap & PROCBASED2_UNRESTRICTED_GUEST) != 0);
    if (gEptAvailable) {
      gEptp = BuildIdentityMap (EPT_TABLE_FLAGS, EPT_LEAF_FLAGS);
      if (gEptp == 0) {
        gEptAvailable = FALSE;   // allocation failed; fall back to pass-through
        gUnrestrictedGuest = FALSE; // unrestricted guest requires EPT
      } else {
        gEptp |= 6ULL | (3ULL << 3);        // write-back | walk length - 1 = 3
      }
    }
  }

  gCr0Fixed0 = AsmReadMsr64 (MSR_IA32_VMX_CR0_FIXED0);
  gCr0Fixed1 = AsmReadMsr64 (MSR_IA32_VMX_CR0_FIXED1);
  gCr4Fixed0 = AsmReadMsr64 (MSR_IA32_VMX_CR4_FIXED0);
  gCr4Fixed1 = AsmReadMsr64 (MSR_IA32_VMX_CR4_FIXED1);

  gPaeIdentityCr3 = BuildPaeIdentityMap ();
  gHostCr3 = BuildIdentityMap (HOST_TABLE_FLAGS, HOST_LEAF_FLAGS);
  if (gHostCr3 == 0) {
    gHostCr3 = AsmReadCr3 ();     // keep the firmware's; AP startup will decline
  }

  HvMark (12, '5');  // host page tables built; VmxInitShared done

  DEBUG ((DEBUG_INFO,
    "[HV] vmx caps: secondary=%d ept=%d unrestricted=%d\n",
    gSecondaryAvailable, gEptAvailable, gUnrestrictedGuest));
  DEBUG ((DEBUG_INFO,
    "[HV] host isa: sse41=%d sse42=%d popcnt=%d -> emulate sse41=%d sse42=%d popcnt=%d\n",
    gHostHasSse41, gHostHasSse42, gHostHasPopcnt,
    !gHostHasSse41, !gHostHasSse42, !gHostHasPopcnt));

  return EFI_SUCCESS;
}

//
// Build this CPU's private host GDT (flat code/data + one TSS) and load its
// base/selectors into the VMCS host area.
//
STATIC
VOID
SetupHostDescriptors (
  IN VMX_VCPU  *Vcpu,
  IN UINT64     HostStackTop
  )
{
  UINT64  *Gdt = (UINT64 *)Vcpu->HostGdt;
  UINTN    TssBase;
  UINT64   TssLow;

  ZeroMem (Gdt, SIZE_4KB);
  ZeroMem (Vcpu->HostTss, SIZE_4KB);

  //
  // Give the TSS real stacks.
  //
  // The GUEST's TR points at this same TSS from the moment we virtualize until
  // the OS executes its own LTR, and an x64 Windows IDT routes several vectors
  // through IST stacks.  With RSP0 and IST1..IST7 left at zero, delivering any
  // such fault loads RSP = 0, the push faults, and a fault taken while
  // delivering a fault is a #DF.
  //
  // The stacks live in the tail of this page: the TSS structure is 0x68 bytes
  // and the page is 4 KiB, so eight 448-byte stacks fit above it and nothing
  // else touches them.  Each pointer is the top of its own region.
  //
  {
    UINT8  *Tss = (UINT8 *)Vcpu->HostTss;
    UINT64  Top = (UINT64)(UINTN)Vcpu->HostTss + SIZE_4KB;
    UINTN   I;

    *(UINT64 *)(Tss + 0x04) = Top;                       // RSP0
    for (I = 1; I <= 7; I++) {
      *(UINT64 *)(Tss + 0x24 + (I - 1) * 8) = Top - I * 448;   // IST1..IST7
    }
    *(UINT16 *)(Tss + 0x66) = 0xFFFF;                    // I/O map base past limit
  }

  Gdt[1] = 0x00AF9A000000FFFFULL;   // 0x08 : 64-bit code
  Gdt[2] = 0x00CF92000000FFFFULL;   // 0x10 : data

  TssBase = (UINTN)Vcpu->HostTss;
  TssLow  = (UINT64)0x0067;
  TssLow |= ((UINT64)(TssBase & 0xFFFF)) << 16;
  TssLow |= ((UINT64)((TssBase >> 16) & 0xFF)) << 32;
  TssLow |= ((UINT64)0x89) << 40;                        // P=1, avail 64-bit TSS
  TssLow |= ((UINT64)((TssBase >> 24) & 0xFF)) << 56;
  Gdt[3]  = TssLow;
  Gdt[4]  = (UINT64)(TssBase >> 32);

  AsmVmWrite (VMCS_HOST_GDTR_BASE, (UINTN)Gdt);
  AsmVmWrite (VMCS_HOST_TR_BASE,   TssBase);

  AsmVmWrite (VMCS_HOST_CS_SELECTOR, HOST_CS_SEL);
  AsmVmWrite (VMCS_HOST_SS_SELECTOR, HOST_DS_SEL);
  AsmVmWrite (VMCS_HOST_DS_SELECTOR, HOST_DS_SEL);
  AsmVmWrite (VMCS_HOST_ES_SELECTOR, HOST_DS_SEL);
  AsmVmWrite (VMCS_HOST_FS_SELECTOR, HOST_DS_SEL);
  AsmVmWrite (VMCS_HOST_GS_SELECTOR, HOST_DS_SEL);
  AsmVmWrite (VMCS_HOST_TR_SELECTOR, HOST_TR_SEL);

  AsmVmWrite (VMCS_HOST_FS_BASE, 0);
  AsmVmWrite (VMCS_HOST_GS_BASE, 0);
  AsmVmWrite (VMCS_HOST_RSP, HostStackTop);
  AsmVmWrite (VMCS_HOST_RIP, (UINTN)AsmVmExitHandler);
}

STATIC
VOID
FillGuestSegment (
  IN UINT16  Selector,
  IN UINT32  SelField,
  IN UINT32  BaseField,
  IN UINT32  LimitField,
  IN UINT32  ArField,
  IN UINT64  Base
  )
{
  UINT32  Ar;

  AsmVmWrite (SelField,  Selector);
  AsmVmWrite (BaseField, (UINTN)Base);

  if ((Selector & 0xFFF8) == 0) {
    AsmVmWrite (LimitField, 0);
    AsmVmWrite (ArField,    0x10000);   // unusable
    return;
  }

  AsmVmWrite (LimitField, AsmLoadLimit (Selector));
  Ar = (AsmLoadAr (Selector) >> 8) & 0xF0FF;
  AsmVmWrite (ArField, Ar);
}

STATIC
VOID
SetupGuestState (
  IN VMX_VCPU  *Vcpu
  )
{
  IA32_DESCRIPTOR  Gdtr, Idtr;

  AsmReadGdtr (&Gdtr);
  AsmReadIdtr (&Idtr);

  AsmVmWrite (VMCS_GUEST_CR0, AsmReadCr0 ());
  AsmVmWrite (VMCS_GUEST_CR3, AsmReadCr3 ());
  AsmVmWrite (VMCS_GUEST_CR4, AsmReadCr4 ());
  AsmVmWrite (VMCS_GUEST_DR7, 0x400);
  AsmVmWrite (VMCS_GUEST_RFLAGS, AsmGetRflags ());
  // GUEST_RSP / GUEST_RIP are written by AsmVmxLaunch.

  FillGuestSegment (AsmReadEs (), VMCS_GUEST_ES_SELECTOR, VMCS_GUEST_ES_BASE,
                    VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, 0);
  FillGuestSegment (AsmReadCs (), VMCS_GUEST_CS_SELECTOR, VMCS_GUEST_CS_BASE,
                    VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, 0);
  FillGuestSegment (AsmReadSs (), VMCS_GUEST_SS_SELECTOR, VMCS_GUEST_SS_BASE,
                    VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, 0);
  FillGuestSegment (AsmReadDs (), VMCS_GUEST_DS_SELECTOR, VMCS_GUEST_DS_BASE,
                    VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, 0);
  FillGuestSegment (AsmReadFs (), VMCS_GUEST_FS_SELECTOR, VMCS_GUEST_FS_BASE,
                    VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR,
                    AsmReadMsr64 (MSR_IA32_FS_BASE));
  FillGuestSegment (AsmReadGs (), VMCS_GUEST_GS_SELECTOR, VMCS_GUEST_GS_BASE,
                    VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR,
                    AsmReadMsr64 (MSR_IA32_GS_BASE));
  FillGuestSegment (AsmReadLdtr (), VMCS_GUEST_LDTR_SELECTOR, VMCS_GUEST_LDTR_BASE,
                    VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, 0);

  //
  // Synthesize a usable guest TR from this CPU's TSS (firmware often leaves
  // TR = 0, which VM-entry rejects).  The hidden descriptor is authoritative.
  //
  AsmVmWrite (VMCS_GUEST_TR_SELECTOR, HOST_TR_SEL);
  AsmVmWrite (VMCS_GUEST_TR_BASE,     (UINTN)Vcpu->HostTss);
  AsmVmWrite (VMCS_GUEST_TR_LIMIT,    0x67);
  AsmVmWrite (VMCS_GUEST_TR_AR,       0x008B);   // P=1, busy 64-bit TSS

  AsmVmWrite (VMCS_GUEST_GDTR_BASE,  (UINTN)Gdtr.Base);
  AsmVmWrite (VMCS_GUEST_GDTR_LIMIT, Gdtr.Limit);
  AsmVmWrite (VMCS_GUEST_IDTR_BASE,  (UINTN)Idtr.Base);
  AsmVmWrite (VMCS_GUEST_IDTR_LIMIT, Idtr.Limit);

  AsmVmWrite (VMCS_GUEST_IA32_DEBUGCTL, AsmReadMsr64 (MSR_IA32_DEBUGCTL));
  AsmVmWrite (VMCS_GUEST_IA32_PAT,      AsmReadMsr64 (MSR_IA32_PAT));
  AsmVmWrite (VMCS_GUEST_IA32_EFER,     AsmReadMsr64 (MSR_IA32_EFER));
  AsmVmWrite (VMCS_GUEST_SYSENTER_CS,   AsmReadMsr64 (MSR_IA32_SYSENTER_CS));
  AsmVmWrite (VMCS_GUEST_SYSENTER_ESP,  AsmReadMsr64 (MSR_IA32_SYSENTER_ESP));
  AsmVmWrite (VMCS_GUEST_SYSENTER_EIP,  AsmReadMsr64 (MSR_IA32_SYSENTER_EIP));

  AsmVmWrite (VMCS_GUEST_INTERRUPTIBILITY, 0);
  AsmVmWrite (VMCS_GUEST_ACTIVITY_STATE,   0);
  AsmVmWrite (VMCS_GUEST_PENDING_DBG,      0);
  AsmVmWrite (VMCS_LINK_POINTER,           0xFFFFFFFFFFFFFFFFULL);
}

STATIC
VOID
SetupHostControlState (
  VOID
  )
{
  IA32_DESCRIPTOR  Idtr;

  AsmReadIdtr (&Idtr);

  AsmVmWrite (VMCS_HOST_CR0, AsmReadCr0 ());
  AsmVmWrite (VMCS_HOST_CR3, (UINTN)gHostCr3);
  AsmVmWrite (VMCS_HOST_CR4, AsmReadCr4 ());
  AsmVmWrite (VMCS_HOST_IDTR_BASE, (UINTN)Idtr.Base);

  AsmVmWrite (VMCS_HOST_IA32_PAT,     AsmReadMsr64 (MSR_IA32_PAT));
  AsmVmWrite (VMCS_HOST_IA32_EFER,    AsmReadMsr64 (MSR_IA32_EFER));
  AsmVmWrite (VMCS_HOST_SYSENTER_CS,  AsmReadMsr64 (MSR_IA32_SYSENTER_CS));
  AsmVmWrite (VMCS_HOST_SYSENTER_ESP, AsmReadMsr64 (MSR_IA32_SYSENTER_ESP));
  AsmVmWrite (VMCS_HOST_SYSENTER_EIP, AsmReadMsr64 (MSR_IA32_SYSENTER_EIP));
}

/**
  Program the whole VMCS for this CPU.  VmxInitShared must have run first.
**/
EFI_STATUS
VmxSetupVmcs (
  IN OUT VMX_VCPU  *Vcpu
  )
{
  UINT64   Basic;
  BOOLEAN  UseTrue;
  UINT32   Pin, Proc, Proc2, Exit, Entry;
  UINT32   Ctls2Cap;
  UINTN    HostStackTop;

  Basic   = AsmReadMsr64 (MSR_IA32_VMX_BASIC);
  UseTrue = (Basic & BIT55) != 0;

  //
  // Activate the VMX-preemption timer so a guest that stops taking other exits
  // (a spin/halt) still exits periodically - this is how we sample where an
  // otherwise-silent guest is stuck.  AdjustControls drops the bit if the part
  // does not support it, in which case the timer value below is simply ignored.
  //
  Pin = PIN_ACTIVATE_PREEMPT_TIMER;
  Pin = AdjustControls (Pin, UseTrue ? MSR_IA32_VMX_TRUE_PINBASED
                                     : MSR_IA32_VMX_PINBASED_CTLS);

  //
  // CR3-load/store exiting is requested EXPLICITLY, and it is tempting to stop:
  // with no EPT the guest walks its own tables, and HandleCrAccess's CR3 case
  // writes the value straight into VMCS_GUEST_CR3, which is exactly what the
  // hardware would have done unassisted.  Every one of those exits looks like
  // pure overhead, and an OS switches address space thousands of times a second.
  //
  // Measured, on a nested guest where the bits are optional rather than
  // default-1: dropping them took the guest from ~600 K exits to 11.2 M, of
  // which 11.07 M were reinjected #GPs, with RIP wandering in low memory.  The
  // guest gets further with us in the way than without.  Left on deliberately.
  //
  Proc = PROCBASED_USE_MSR_BITMAPS |
         PROCBASED_CR3_LOAD_EXITING | PROCBASED_CR3_STORE_EXITING;
  if (gSecondaryAvailable) {
    Proc |= PROCBASED_ACTIVATE_SECONDARY;
  }
  Proc = AdjustControls (Proc, UseTrue ? MSR_IA32_VMX_TRUE_PROCBASED
                                       : MSR_IA32_VMX_PROCBASED_CTLS);

  if (gSecondaryAvailable) {
    Ctls2Cap = (UINT32)(AsmReadMsr64 (MSR_IA32_VMX_PROCBASED_CTLS2) >> 32);
    Proc2 = 0;
    if (gEptAvailable)                            Proc2 |= PROCBASED2_ENABLE_EPT;
    if (Ctls2Cap & PROCBASED2_ENABLE_RDTSCP)      Proc2 |= PROCBASED2_ENABLE_RDTSCP;
    if (Ctls2Cap & PROCBASED2_ENABLE_INVPCID)     Proc2 |= PROCBASED2_ENABLE_INVPCID;
    if (Ctls2Cap & PROCBASED2_ENABLE_XSAVES)      Proc2 |= PROCBASED2_ENABLE_XSAVES;
    if (gUnrestrictedGuest)                       Proc2 |= PROCBASED2_UNRESTRICTED_GUEST;
    Proc2 = AdjustControls (Proc2, MSR_IA32_VMX_PROCBASED_CTLS2);
    AsmVmWrite (VMCS_PROC_BASED_CTLS2, Proc2);
    if (gEptAvailable) {
      AsmVmWrite (VMCS_EPT_POINTER, (UINTN)gEptp);
    }
  }

  // SAVE_IA32_EFER makes the guest's EFER (hence LMA) readable in the VMCS after
  // each exit; the exit handler uses it to keep the IA-32e-mode-guest entry
  // control in sync as an AP transitions real -> protected -> long mode.
  Exit = EXIT_CTL_HOST_ADDR_SPACE_SIZE | EXIT_CTL_SAVE_DEBUG_CTLS |
         EXIT_CTL_LOAD_IA32_PAT | EXIT_CTL_LOAD_IA32_EFER | EXIT_CTL_SAVE_IA32_EFER;
  Exit = AdjustControls (Exit, UseTrue ? MSR_IA32_VMX_TRUE_EXIT
                                       : MSR_IA32_VMX_EXIT_CTLS);

  Entry = ENTRY_CTL_IA32E_MODE_GUEST | ENTRY_CTL_LOAD_DEBUG_CTLS |
          ENTRY_CTL_LOAD_IA32_PAT | ENTRY_CTL_LOAD_IA32_EFER;
  Entry = AdjustControls (Entry, UseTrue ? MSR_IA32_VMX_TRUE_ENTRY
                                         : MSR_IA32_VMX_ENTRY_CTLS);

  AsmVmWrite (VMCS_PIN_BASED_CTLS,  Pin);
  AsmVmWrite (VMCS_PROC_BASED_CTLS, Proc);
  AsmVmWrite (VMCS_EXIT_CTLS,       Exit);
  AsmVmWrite (VMCS_ENTRY_CTLS,      Entry);

  //
  // Preemption-timer countdown reloaded on every VM-entry (we do not set the
  // "save VMX-preemption timer" exit control, so the VMCS value is unchanged by
  // exits).  ~0x40000 ticks keeps the sampling frequent without much overhead.
  //
  if ((Pin & PIN_ACTIVATE_PREEMPT_TIMER) != 0) {
    AsmVmWrite (VMCS_PREEMPTION_TIMER_VALUE, 0x40000);
  }

  //
  // Trap #UD (vector 6) - the instructions we emulate - plus, for diagnosis,
  // #DF (8) and #GP (13) so a guest fault cascade is caught one step before it
  // becomes an opaque triple fault.  An AP widens this to include #PF on its
  // first exit; see SyncApPfTrap.
  //
  AsmVmWrite (VMCS_EXCEPTION_BITMAP, HV_EXC_BITMAP_BASE);
  AsmVmWrite (VMCS_PAGEFAULT_ERRCODE_MASK,  0);
  AsmVmWrite (VMCS_PAGEFAULT_ERRCODE_MATCH, 0);

  AsmVmWrite (VMCS_MSR_BITMAP, (UINTN)gMsrBitmap);
  AsmVmWrite (VMCS_CR3_TARGET_COUNT, 0);

  //
  // CR4.VMXE must stay set in the *actual* guest CR4 (IA32_VMX_CR4_FIXED0
  // requires it), but the guest OS keeps VMXE clear in its own view and would
  // otherwise clear it on a MOV-to-CR4, breaking the next VM-entry.  Own VMXE
  // via the guest/host mask and present it as 0 in the read shadow.
  //
  //
  // Own PE and PG.  The guest reads them from the read shadow and any write
  // that changes them exits, which is what lets a guest believing paging is off
  // run on our identity map.
  //
  AsmVmWrite (VMCS_CR0_GUEST_HOST_MASK, CR0_PE_BIT | CR0_PG_BIT);
  AsmVmWrite (VMCS_CR4_GUEST_HOST_MASK, CR4_VMXE);
  AsmVmWrite (VMCS_CR0_READ_SHADOW, AsmReadCr0 ());
  AsmVmWrite (VMCS_CR4_READ_SHADOW, AsmReadCr4 () & ~(UINT64)CR4_VMXE);

  HostStackTop = (UINTN)Vcpu->HostStack + HV_HOST_STACK_SIZE;
  SetupHostDescriptors (Vcpu, HostStackTop);
  SetupHostControlState ();
  SetupGuestState (Vcpu);

  //
  // Fill in this processor's block, at the base of the stack whose top we just
  // handed the CPU.  This function runs ON the target processor - the BSP
  // inside the ExitBootServices hook, or an AP out of the trampoline - so the
  // APIC ID read here is genuinely ours.
  //
  // Magic goes last: until it is set, HvPcpu treats the block as untrustworthy
  // and falls back to CPUID, so a half-built block is never acted on.
  //
  {
    HV_PCPU  *Pcpu = Vcpu->Pcpu;
    UINT32    RegEax, RegEbx, RegEcx, RegEdx;

    ZeroMem (Pcpu, sizeof (HV_PCPU));
    AsmCpuid (1, &RegEax, &RegEbx, &RegEcx, &RegEdx);
    Pcpu->ApicId    = RegEbx >> 24;
    Pcpu->IsBsp     = (BOOLEAN)(Pcpu->ApicId == gHvBspApicId);
    Pcpu->CpuIndex  = Vcpu->CpuIndex;
    Pcpu->EntryCtls = Entry;                   // exactly what we wrote above
    //
    // We hook a processor that is already running paged, in long mode.  Only an
    // AP coming out of SIPI starts with paging shadowed off.
    //
    Pcpu->GuestPaging = TRUE;
    Pcpu->GuestCr3    = AsmReadCr3 ();
    Pcpu->GuestEfer   = AsmReadMsr64 (MSR_IA32_EFER);
    Pcpu->MicroRev    = HvReadMicrocodeRevision ();
    if (Pcpu->IsBsp) {
      gHvBspMicroRev = Pcpu->MicroRev;
    }
    Pcpu->Magic     = HV_PCPU_MAGIC;
  }

  return EFI_SUCCESS;
}
