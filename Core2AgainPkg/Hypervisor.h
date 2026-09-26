/** @file
  Hypervisor.h - Shared definitions for the Core2Again UEFI Type-1 VMM.

  A minimal VMX ("Intel VT-x") hypervisor whose only job is to trap #UD
  (invalid-opcode, vector 6) VM-exits caused by SSE4.2 / POPCNT instructions
  that Windows 11 24H2 emits on CPUs that predate those ISA extensions, to
  software-emulate the instruction, advance guest RIP, and VMRESUME.

  Target toolchain: EDK II + Visual Studio (MSFT) or CLANGPDB; assembly is
  MASM/ml64 (VmxAsm.asm). The layout of GUEST_REGS below is a hard contract
  with the push/pop sequence in AsmVmExitHandler - do not reorder fields.
**/

#ifndef CORE2AGAIN_H_
#define CORE2AGAIN_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/CpuLib.h>

//
// ---------------------------------------------------------------------------
// Model-specific registers used during VMX bring-up.
// ---------------------------------------------------------------------------
//
#define MSR_IA32_FEATURE_CONTROL       0x0000003A
#define   FEATURE_CONTROL_LOCK             BIT0
#define   FEATURE_CONTROL_VMXON_OUT_SMX    BIT2

#define MSR_IA32_VMX_BASIC             0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS     0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS    0x00000482
#define MSR_IA32_VMX_EXIT_CTLS         0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS        0x00000484
#define MSR_IA32_VMX_MISC              0x00000485
#define MSR_IA32_VMX_CR0_FIXED0        0x00000486
#define MSR_IA32_VMX_CR0_FIXED1        0x00000487
#define MSR_IA32_VMX_CR4_FIXED0        0x00000488
#define MSR_IA32_VMX_CR4_FIXED1        0x00000489
#define MSR_IA32_VMX_PROCBASED_CTLS2   0x0000048B
#define MSR_IA32_VMX_EPT_VPID_CAP      0x0000048C
#define MSR_IA32_VMX_TRUE_PINBASED     0x0000048D
#define MSR_IA32_VMX_TRUE_PROCBASED    0x0000048E
#define MSR_IA32_VMX_TRUE_EXIT         0x0000048F
#define MSR_IA32_VMX_TRUE_ENTRY        0x00000490

#define MSR_IA32_SYSENTER_CS           0x00000174
#define MSR_IA32_SYSENTER_ESP          0x00000175
#define MSR_IA32_SYSENTER_EIP          0x00000176
#define MSR_IA32_DEBUGCTL              0x000001D9
#define MSR_IA32_PAT                   0x00000277
#define MSR_IA32_BIOS_SIGN_ID          0x8B
#define MSR_IA32_EFER                  0xC0000080
#define MSR_IA32_FS_BASE               0xC0000100
#define MSR_IA32_GS_BASE               0xC0000101
#define MSR_IA32_KERNEL_GS_BASE        0xC0000102

//
// CR bits relevant to us.
//
#define CR0_PE_BIT                     BIT0
#define CR0_NE_BIT                     BIT5
#define CR0_PG_BIT                     BIT31
#define CR4_PAE                        BIT5
#define EFER_LME_BIT                   BIT8
#define EFER_LMA_BIT                   BIT10
#define CR4_VMXE                       BIT13
#define CR4_OSXSAVE                    BIT18

//
// ---------------------------------------------------------------------------
// VMCS field encodings (Intel SDM Vol. 3, Appendix B).  Only the ones this
// hypervisor touches are listed.
// ---------------------------------------------------------------------------
//
// 16-bit control / guest / host selectors
#define VMCS_GUEST_ES_SELECTOR         0x00000800
#define VMCS_GUEST_CS_SELECTOR         0x00000802
#define VMCS_GUEST_SS_SELECTOR         0x00000804
#define VMCS_GUEST_DS_SELECTOR         0x00000806
#define VMCS_GUEST_FS_SELECTOR         0x00000808
#define VMCS_GUEST_GS_SELECTOR         0x0000080A
#define VMCS_GUEST_LDTR_SELECTOR       0x0000080C
#define VMCS_GUEST_TR_SELECTOR         0x0000080E
#define VMCS_HOST_ES_SELECTOR          0x00000C00
#define VMCS_HOST_CS_SELECTOR          0x00000C02
#define VMCS_HOST_SS_SELECTOR          0x00000C04
#define VMCS_HOST_DS_SELECTOR          0x00000C06
#define VMCS_HOST_FS_SELECTOR          0x00000C08
#define VMCS_HOST_GS_SELECTOR          0x00000C0A
#define VMCS_HOST_TR_SELECTOR          0x00000C0C

// 64-bit control fields
#define VMCS_IO_BITMAP_A               0x00002000
#define VMCS_IO_BITMAP_B               0x00002002
#define VMCS_MSR_BITMAP                0x00002004
#define VMCS_EPT_POINTER               0x0000201A

// 64-bit guest fields
#define VMCS_LINK_POINTER              0x00002800
#define VMCS_GUEST_IA32_DEBUGCTL       0x00002802
#define VMCS_GUEST_IA32_PAT            0x00002804
#define VMCS_GUEST_IA32_EFER           0x00002806

// 64-bit host fields
#define VMCS_HOST_IA32_PAT             0x00002C00
#define VMCS_HOST_IA32_EFER            0x00002C02

// 32-bit control fields
#define VMCS_PIN_BASED_CTLS            0x00004000
#define PIN_ACTIVATE_PREEMPT_TIMER    BIT6           // pin-based: VMX-preempt timer
#define VMCS_PREEMPTION_TIMER_VALUE   0x0000482E     // 32-bit guest field
#define VMCS_PROC_BASED_CTLS           0x00004002
#define VMCS_EXCEPTION_BITMAP          0x00004004
#define VMCS_PAGEFAULT_ERRCODE_MASK    0x00004006
#define VMCS_PAGEFAULT_ERRCODE_MATCH   0x00004008
#define VMCS_CR3_TARGET_COUNT          0x0000400A
#define VMCS_EXIT_CTLS                 0x0000400C
#define VMCS_EXIT_MSR_STORE_COUNT      0x0000400E
#define VMCS_EXIT_MSR_LOAD_COUNT       0x00004010
#define VMCS_ENTRY_CTLS                0x00004012
#define VMCS_ENTRY_MSR_LOAD_COUNT      0x00004014
#define VMCS_ENTRY_INTR_INFO           0x00004016
#define VMCS_ENTRY_EXCEPTION_ERRCODE   0x00004018
#define VMCS_ENTRY_INSTR_LEN           0x0000401A
#define VMCS_PROC_BASED_CTLS2          0x0000401E

// Natural-width read-only data field
#define VMCS_EXIT_QUALIFICATION        0x00006400

// 32-bit read-only data fields
#define VMCS_VM_INSTRUCTION_ERROR      0x00004400
#define VMCS_EXIT_REASON               0x00004402
#define VMCS_EXIT_INTR_INFO            0x00004404
#define VMCS_EXIT_INTR_ERRCODE         0x00004406
#define VMCS_IDT_VECTORING_INFO        0x00004408
#define VMCS_IDT_VECTORING_ERRCODE     0x0000440A
#define VMCS_EXIT_INSTR_LEN            0x0000440C
#define VMCS_EXIT_INSTR_INFO           0x0000440E

// 32-bit guest fields
#define VMCS_GUEST_ES_LIMIT            0x00004800
#define VMCS_GUEST_CS_LIMIT            0x00004802
#define VMCS_GUEST_SS_LIMIT            0x00004804
#define VMCS_GUEST_DS_LIMIT            0x00004806
#define VMCS_GUEST_FS_LIMIT            0x00004808
#define VMCS_GUEST_GS_LIMIT            0x0000480A
#define VMCS_GUEST_LDTR_LIMIT          0x0000480C
#define VMCS_GUEST_TR_LIMIT            0x0000480E
#define VMCS_GUEST_GDTR_LIMIT          0x00004810
#define VMCS_GUEST_IDTR_LIMIT          0x00004812
#define VMCS_GUEST_ES_AR               0x00004814
#define VMCS_GUEST_CS_AR               0x00004816
#define VMCS_GUEST_SS_AR               0x00004818
#define VMCS_GUEST_DS_AR               0x0000481A
#define VMCS_GUEST_FS_AR               0x0000481C
#define VMCS_GUEST_GS_AR               0x0000481E
#define VMCS_GUEST_LDTR_AR             0x00004820
#define VMCS_GUEST_TR_AR               0x00004822
#define VMCS_GUEST_INTERRUPTIBILITY    0x00004824
#define VMCS_GUEST_ACTIVITY_STATE      0x00004826
#define VMCS_GUEST_SYSENTER_CS         0x0000482A

// 32-bit host field
#define VMCS_HOST_SYSENTER_CS          0x00004C00

// Natural-width control fields
#define VMCS_CR0_GUEST_HOST_MASK       0x00006000
#define VMCS_CR4_GUEST_HOST_MASK       0x00006002
#define VMCS_CR0_READ_SHADOW           0x00006004
#define VMCS_CR4_READ_SHADOW           0x00006006

// Natural-width guest fields
#define VMCS_GUEST_CR0                 0x00006800
#define VMCS_GUEST_CR3                 0x00006802
#define VMCS_GUEST_CR4                 0x00006804
#define VMCS_GUEST_ES_BASE             0x00006806
#define VMCS_GUEST_CS_BASE             0x00006808
#define VMCS_GUEST_SS_BASE             0x0000680A
#define VMCS_GUEST_DS_BASE             0x0000680C
#define VMCS_GUEST_FS_BASE             0x0000680E
#define VMCS_GUEST_GS_BASE             0x00006810
#define VMCS_GUEST_LDTR_BASE           0x00006812
#define VMCS_GUEST_TR_BASE             0x00006814
#define VMCS_GUEST_GDTR_BASE           0x00006816
#define VMCS_GUEST_IDTR_BASE           0x00006818
#define VMCS_GUEST_DR7                 0x0000681A
#define VMCS_GUEST_RSP                 0x0000681C
#define VMCS_GUEST_RIP                 0x0000681E
#define VMCS_GUEST_RFLAGS              0x00006820
#define VMCS_GUEST_PENDING_DBG         0x00006822
#define VMCS_GUEST_SYSENTER_ESP        0x00006824
#define VMCS_GUEST_SYSENTER_EIP        0x00006826

// Natural-width host fields
#define VMCS_HOST_CR0                  0x00006C00
#define VMCS_HOST_CR3                  0x00006C02
#define VMCS_HOST_CR4                  0x00006C04
#define VMCS_HOST_FS_BASE              0x00006C06
#define VMCS_HOST_GS_BASE              0x00006C08
#define VMCS_HOST_TR_BASE              0x00006C0A
#define VMCS_HOST_GDTR_BASE            0x00006C0C
#define VMCS_HOST_IDTR_BASE            0x00006C0E
#define VMCS_HOST_SYSENTER_ESP         0x00006C10
#define VMCS_HOST_SYSENTER_EIP         0x00006C12
#define VMCS_HOST_RSP                  0x00006C14
#define VMCS_HOST_RIP                  0x00006C16

//
// ---------------------------------------------------------------------------
// Control-bit helpers.
// ---------------------------------------------------------------------------
//
// Primary processor-based controls
#define PROCBASED_CR3_LOAD_EXITING     BIT15          // forced on Penryn; explicit
#define PROCBASED_CR3_STORE_EXITING    BIT16          //   so it also holds on Bochs
#define PROCBASED_MONITOR_TRAP_FLAG    BIT27          // single-step the guest
#define PROCBASED_USE_MSR_BITMAPS      BIT28
#define PROCBASED_ACTIVATE_SECONDARY   BIT31
// Secondary processor-based controls
#define PROCBASED2_ENABLE_EPT          BIT1
#define PROCBASED2_ENABLE_RDTSCP       BIT3
#define PROCBASED2_UNRESTRICTED_GUEST  BIT7
#define PROCBASED2_ENABLE_INVPCID      BIT12
#define PROCBASED2_ENABLE_XSAVES       BIT20
// VM-exit controls
#define EXIT_CTL_SAVE_DEBUG_CTLS       BIT2
#define EXIT_CTL_HOST_ADDR_SPACE_SIZE  BIT9
#define EXIT_CTL_SAVE_IA32_PAT         BIT18
#define EXIT_CTL_LOAD_IA32_PAT         BIT19
#define EXIT_CTL_SAVE_IA32_EFER        BIT20
#define EXIT_CTL_LOAD_IA32_EFER        BIT21
// VM-entry controls
#define ENTRY_CTL_LOAD_DEBUG_CTLS      BIT2
#define ENTRY_CTL_IA32E_MODE_GUEST     BIT9
#define ENTRY_CTL_LOAD_IA32_PAT        BIT14
#define ENTRY_CTL_LOAD_IA32_EFER       BIT15

// Exception vectors.  #UD is the one we service; #DF/#GP are trapped only for
// diagnosis of a guest fault cascade (a triple fault otherwise hides the cause).
#define EXCEPTION_VECTOR_DF            8
#define EXCEPTION_VECTOR_UD            6
#define EXCEPTION_VECTOR_PF            14
#define EXCEPTION_VECTOR_GP            13

//
// The exceptions we always intercept.  #UD is the emulator's whole reason for
// existing; #DF and #GP are diagnostic.  APs add #PF to this for a while (see
// HV_AP_PF_TRAP_EXITS) - the BSP never does, because on a machine with no EPT
// an exit per demand-paging fault is not a cost worth paying to watch a core
// that is working.
//
#define HV_EXC_BITMAP_BASE  ((1u << EXCEPTION_VECTOR_UD) | \
                             (1u << EXCEPTION_VECTOR_DF) | \
                             (1u << EXCEPTION_VECTOR_GP))

//
// How long an AP keeps trapping #PF.  An AP that has run this far is not the
// one dying in startup, and every fault after that is ordinary paging.
//
#define HV_AP_PF_TRAP_EXITS  4096

//
// How many times the SAME (RIP, CR2) may fault on an AP before we call it stuck
// and dump.  Demand paging never repeats an address at one RIP; a processor
// wedged on one instruction does nothing else.
//
#define HV_AP_PF_LOOP_LIMIT  64

// Basic exit reasons we care about (bits 15:0 of VMCS_EXIT_REASON).
#define EXIT_REASON_EXCEPTION_NMI      0
#define EXIT_REASON_TRIPLE_FAULT       2
#define EXIT_REASON_INIT_SIGNAL        3
#define EXIT_REASON_SIPI               4
#define EXIT_REASON_CPUID              10
#define EXIT_REASON_INVD               13
#define EXIT_REASON_CR_ACCESS          28
#define EXIT_REASON_MTF                37             // Monitor Trap Flag (single-step)
//
// A failed VM ENTRY is reported as an exit with bit 31 set in the exit-reason
// field.  It is not a guest event at all: the CPU examined the VMCS we built and
// refused to run it, so the interesting state is the guest state we programmed.
//
#define EXIT_REASON_ENTRY_FAILURE      BIT31
#define EXIT_REASON_ENTRY_INVALID_GUEST 33
#define EXIT_REASON_ENTRY_MSR_LOAD      34
#define EXIT_REASON_ENTRY_MACHINE_CHECK 41
#define EXIT_REASON_RDMSR              31
#define EXIT_REASON_WRMSR              32
#define EXIT_REASON_PREEMPT_TIMER      52
#define EXIT_REASON_XSETBV             55
#define EXIT_REASON_MASK               0xFFFF

//
// Control-register-access exit qualification (SDM Table 28-3).  On pre-"TRUE"-
// MSR parts (Core 2 / Penryn) CR3-load/store exiting are default-1 and cannot
// be turned off, so MOV to/from CR3 always exits here and MUST be serviced.
//
#define CR_ACCESS_CR_NUM(q)            ((UINT32)((q) & 0xF))          // bits 3:0
#define CR_ACCESS_TYPE(q)              ((UINT32)(((q) >> 4) & 0x3))   // bits 5:4
#define CR_ACCESS_GPR(q)               ((UINT32)(((q) >> 8) & 0xF))   // bits 11:8
#define CR_ACCESS_LMSW_SRC(q)          ((UINT32)(((q) >> 16) & 0xFFFF))
#define CR_ACCESS_TYPE_TO_CR           0
#define CR_ACCESS_TYPE_FROM_CR         1
#define CR_ACCESS_TYPE_CLTS            2
#define CR_ACCESS_TYPE_LMSW            3

// CPUID.01H:ECX feature bits we advertise so guest feature checks pass while
// the instructions themselves are trapped and emulated.
#define CPUID1_ECX_SSE41               BIT19
#define CPUID1_ECX_SSE42               BIT20
#define CPUID1_ECX_POPCNT              BIT23

// Offset of XMM0 inside an FXSAVE image; XMMn is at FX_XMM0_OFFSET + n*16.
#define FX_XMM0_OFFSET                 160
#define FX_AREA_SIZE                   512

//
// ---------------------------------------------------------------------------
// On-screen diagnostics geometry (Display.c).
//
// Nine rows of large decimals under a checkpoint number, drawn straight into
// the linear framebuffer.  This is the whole instrument on the target machine:
// no debugger, no console after ExitBootServices, and DebugLib is the null
// instance, so a number on this display is the only thing a failing boot can
// say.  HvMarkN's Band argument is HV_BAND_BASE + row.
// ---------------------------------------------------------------------------
//
#define GLYPH_ROWS                     7      // font cell height, in font pixels
#define GLYPH_COLS                     5      // font cell width
#define GLYPH_SCALE                    6      // one font pixel -> 6x6 screen pixels
#define GLYPH_CELL                     ((GLYPH_COLS + 1) * GLYPH_SCALE)
#define HV_NUM_MAX_DIGITS              12     // fits any value we paint
#define HV_NUM_X                       40     // left edge, clear of the trail bars
#define HV_BAND_BASE                   15
#define HV_DIAG_ROWS                   9

// VM-entry / VM-exit interruption-information encoding.
#define INTR_INFO_VECTOR_MASK          0x000000FF
#define INTR_INFO_TYPE_SHIFT           8
#define INTR_INFO_TYPE_MASK            0x00000700
#define INTR_TYPE_HARDWARE_EXCEPTION   3
#define INTR_INFO_ERRCODE_VALID        BIT11
#define INTR_INFO_VALID                BIT31

//
// ---------------------------------------------------------------------------
// Guest general-purpose register frame.
//
// AsmVmExitHandler pushes registers so that this struct overlays the host
// stack with RAX at the lowest address.  DO NOT reorder.  The Rsp slot is a
// placeholder - the architectural guest RSP lives in VMCS_GUEST_RSP.
// ---------------------------------------------------------------------------
//
#pragma pack(push, 1)
typedef struct _GUEST_REGS {
  UINT64 Rax;   // 0x00
  UINT64 Rcx;   // 0x08
  UINT64 Rdx;   // 0x10
  UINT64 Rbx;   // 0x18
  UINT64 Rsp;   // 0x20  (placeholder; use VMCS_GUEST_RSP)
  UINT64 Rbp;   // 0x28
  UINT64 Rsi;   // 0x30
  UINT64 Rdi;   // 0x38
  UINT64 R8;    // 0x40
  UINT64 R9;    // 0x48
  UINT64 R10;   // 0x50
  UINT64 R11;   // 0x58
  UINT64 R12;   // 0x60
  UINT64 R13;   // 0x68
  UINT64 R14;   // 0x70
  UINT64 R15;   // 0x78
} GUEST_REGS;
#pragma pack(pop)

//
// Per-logical-processor VMX context.  One is allocated per CPU on the BSP and
// each is brought into VMX root operation via EFI_MP_SERVICES_PROTOCOL (Mp.c).
// Per-CPU regions are unique; the MSR bitmap, EPT and host page tables are
// shared read-only across CPUs and therefore live as globals in VmxSetup.c.
//
//
// ---------------------------------------------------------------------------
// Per-processor host state, and how the VM-exit handler finds it.
//
// The host stack is allocated aligned to its own size, and this block sits at
// the base of it (the lowest address; the stack grows down from the top, and
// the host's call depth is a few hundred bytes).  Masking any address on that
// stack therefore names the processor in two ALU operations.
//
// It used to be a CPUID on every VM-exit, comparing the local APIC ID against
// the BSP's.  CPUID is serializing, and Windows takes thousands of CR3 exits a
// second; this is the same answer for the cost of an AND.
//
// Magic is written LAST during setup and checked before the block is trusted,
// so a mis-derived pointer degrades to the slow path instead of corrupting
// whatever it landed on.
// ---------------------------------------------------------------------------
//
#define HV_HOST_STACK_SIZE     SIZE_16KB
#define HV_PCPU_MAGIC          0x48565043u        // 'HVPC'
#define HV_ENTRY_CTLS_UNKNOWN  ((UINTN)-1)

//
// Hot-path counters.  One set per processor, summed only when the heartbeat is
// painted (once per 16384 exits), so no VM-exit ever writes a cache line
// another processor owns.  They were shared globals incremented without
// atomics, which cost a cache-line transfer per exit on every core AND still
// lost counts.
//
typedef struct _HV_COUNTERS {
  UINTN  Exits;          // every exit, whatever the reason
  UINTN  Init;           // INIT exits: Windows starting a processor
  UINTN  Sipi;
  UINTN  SseEmul;        // instructions the decoder emulated
  UINTN  Gp;             // guest #GPs reflected
  UINTN  PfInject;       // #PFs synthesized for a non-resident operand
  UINTN  UdFail;         // #UDs we could NOT emulate (an app crashed)
} HV_COUNTERS;

typedef struct _HV_PCPU {
  UINT32       Magic;
  BOOLEAN      IsBsp;
  UINT32       ApicId;
  UINTN        CpuIndex;
  //
  // Last value written to VMCS_ENTRY_CTLS by this processor, so the per-exit
  // mode sync can skip its VMREAD and, when nothing changed, its VMWRITE.  We
  // are the only writer of that field; anything else that writes it must store
  // HV_ENTRY_CTLS_UNKNOWN here (RealModeStartAp does, via HandleSipi).
  //
  UINTN        EntryCtls;

  //
  // Shadowed paging state.  Without unrestricted guest a VMX guest must have
  // CR0.PE and CR0.PG set, so a guest that has not turned paging on yet is run
  // on OUR identity map with CR0.PG hidden behind the read shadow.  These three
  // are that guest's real intent: whether it believes paging is on, the CR3 it
  // asked for (applied only once it does), and its IA32_EFER (for LME, which
  // decides IA-32e mode and cannot be read back off a shared MSR).
  //
  BOOLEAN      GuestPaging;
  UINT64       GuestCr3;
  UINT64       GuestEfer;

  //
  // This processor's microcode revision, refreshed occasionally rather than
  // once: the guest loads its own update after the processor is already
  // running, so a value read at setup time is the BIOS's, not the one Windows
  // will compare.
  //
  UINT64       MicroRev;

  //
  // Last value written to VMCS_GUEST_IA32_EFER by SyncEntryControls.  We are
  // its only writer once a processor is running, so a cached copy turns the
  // steady state - every exit where nothing about the guest's mode changed -
  // from a VMWRITE into a compare.
  //
  UINT64       LastEfer;
  BOOLEAN      LastEferValid;

  //
  // This processor's most recent exit reason.  Per-CPU for the same reason the
  // counters are: a global written on every exit by every processor is a cache
  // line dragged between cores forever.
  //
  UINTN        LastExitReason;

  HV_COUNTERS  Count;
} HV_PCPU;

//
// The per-CPU block for any address on a host stack.
//
#define HV_PCPU_OF(AddrInStack)  \
  ((HV_PCPU *)(((UINTN)(AddrInStack)) & ~(UINTN)(HV_HOST_STACK_SIZE - 1)))

typedef struct _VMX_VCPU {
  VOID     *VmxonRegion;    // 4 KiB, VMXON region (virtual == physical in UEFI)
  VOID     *VmcsRegion;     // 4 KiB, current VMCS
  VOID     *HostStack;      // HV_HOST_STACK_SIZE, aligned to its own size
  HV_PCPU  *Pcpu;           // == HostStack; named so the heartbeat can walk them
  VOID     *HostGdt;        // per-CPU host GDT (avoids TSS busy-bit sharing)
  VOID     *HostTss;        // per-CPU host TSS
  UINTN     CpuIndex;
  BOOLEAN   Launched;
} VMX_VCPU;

//
// Slots we will ever fill: the BSP plus AP_MAX_STARTED (ApStartup.c) APs.  This
// is a runtime driver, so an oversized table is memory the OS never gets back.
//
#define HV_MAX_CPUS  8

//
// Status codes returned by the VMX instruction wrappers in VmxAsm.asm:
//   0 = success, 1 = VMfailInvalid (CF=1), 2 = VMfailValid (ZF=1).
//
#define VMX_OK             0
#define VMX_FAIL_INVALID   1
#define VMX_FAIL_VALID     2

//
// ---------------------------------------------------------------------------
// Assembly helpers implemented in VmxAsm.asm.
// ---------------------------------------------------------------------------
//
UINT8   EFIAPI AsmVmxOn      (IN UINT64 *VmxonPhysAddr);
VOID    EFIAPI AsmVmxOff     (VOID);
UINT8   EFIAPI AsmVmClear    (IN UINT64 *VmcsPhysAddr);
UINT8   EFIAPI AsmVmPtrLd    (IN UINT64 *VmcsPhysAddr);
UINT8   EFIAPI AsmVmWrite    (IN UINTN Field, IN UINTN Value);
UINT8   EFIAPI AsmVmRead     (IN UINTN Field, OUT UINTN *Value);
UINT8   EFIAPI AsmInvept     (IN UINTN Type, IN VOID *Descriptor);

// Segment-selector reads (16-bit selectors returned zero-extended).
UINT16  EFIAPI AsmReadCs     (VOID);
UINT16  EFIAPI AsmReadSs     (VOID);
UINT16  EFIAPI AsmReadDs     (VOID);
UINT16  EFIAPI AsmReadEs     (VOID);
UINT16  EFIAPI AsmReadFs     (VOID);
UINT16  EFIAPI AsmReadGs     (VOID);
UINT16  EFIAPI AsmReadTr     (VOID);
UINT16  EFIAPI AsmReadLdtr   (VOID);

UINT32  EFIAPI AsmLoadAr     (IN UINT16 Selector);   // LAR-derived access rights
UINT32  EFIAPI AsmLoadLimit  (IN UINT16 Selector);   // LSL-derived segment limit
UINTN   EFIAPI AsmGetRsp     (VOID);
UINTN   EFIAPI AsmGetRflags  (VOID);
VOID    EFIAPI AsmXsetbv     (IN UINT32 Index, IN UINT64 Value);

// On-the-fly launch: writes GUEST_RSP / GUEST_RIP so the guest resumes right
// after this call.  Returns 0 when the guest is now running, (UINTN)-1 on
// failure (VMLAUNCH did not enter the guest).
UINTN   EFIAPI AsmVmxLaunch  (VOID);

// Host VM-exit entry point (value stored into VMCS_HOST_RIP).
VOID    EFIAPI AsmVmExitHandler (VOID);

//
// ---------------------------------------------------------------------------
// C entry points.
// ---------------------------------------------------------------------------
//
// Diagnostic: write a marker char to VGA text memory (0xB8000), row 0, column
// Col.  Independent of the UEFI console, so it shows even during early load.
UINTN      EFIAPI AsmAtomicInc (IN volatile UINTN *Counter);
VOID       HvCalibrateStall  (VOID);   // TSC vs firmware Stall; boot services only
//
// HV_DIAG_ENABLED is set by the DSC: 1 for DEBUG, 0 for RELEASE.  A RELEASE
// image does no drawing, keeps no counters and records nothing - the shipping
// hypervisor spends its exits on the guest, not on instrumenting itself.
// HV_AP_DEBUG turns it back on whatever the target, because that build exists
// to be read.
//
#ifndef HV_DIAG_ENABLED
  #define HV_DIAG_ENABLED  1
#endif
#if defined (HV_AP_DEBUG) && !HV_DIAG_ENABLED
  #undef  HV_DIAG_ENABLED
  #define HV_DIAG_ENABLED  1
#endif

#if HV_DIAG_ENABLED
VOID       HvMark            (IN UINTN Col, IN CHAR8 Ch);
VOID       HvMarkN           (IN UINTN Band, IN UINTN N);   // N yellow squares
#else
//
// Macros, not empty functions: the arguments must not be evaluated either.
// Several call sites compute a value - a VMREAD, a page walk - for no other
// purpose than to paint it.
//
#define HvMark(Col, Ch)    ((VOID)0)
#define HvMarkN(Band, N)   ((VOID)0)
#endif

EFI_STATUS VmxCheckSupport   (VOID);
EFI_STATUS VmxAllocRegions   (IN OUT VMX_VCPU *Vcpu);
EFI_STATUS VmxEnterRootMode  (IN OUT VMX_VCPU *Vcpu);
EFI_STATUS VmxSetupVmcs      (IN OUT VMX_VCPU *Vcpu);

// Build shared resources (MSR bitmap, EPT if available, host page tables) once
// on the BSP; must run before any VmxSetupVmcs.  Also reports capability flags.
EFI_STATUS VmxInitShared     (VOID);
BOOLEAN    VmxEptAvailable   (VOID);
BOOLEAN    VmxUnrestrictedGuestAvailable (VOID);
UINT64     VmxPaeIdentityCr3 (VOID);
UINT64     HvReadMicrocodeRevision (VOID);
UINT64     VmxCr0Fixed0 (VOID);
UINT64     VmxCr0Fixed1 (VOID);
UINT64     VmxCr4Fixed0 (VOID);
UINT64     VmxCr4Fixed1 (VOID);
extern volatile UINT64  gHvBspMicroRev;

// Per-ISA-class emulation decisions (TRUE => host lacks it, so trap+emulate).
BOOLEAN    HvEmulateSse41    (VOID);
BOOLEAN    HvEmulateSse42    (VOID);
BOOLEAN    HvEmulatePopcnt   (VOID);

// Mp.c - bring every logical processor into VMX root operation.
EFI_STATUS VmxPrepareAndVirtualizeAps (VOID);
VOID       VmxVirtualizeBspNow        (VOID);

//
// How many logical processors actually reached VMLAUNCH and are running as
// guests.  Shown on the heartbeat: if this is 1 on a multi-core machine, the
// APs are running un-virtualized and Windows must be held to numproc=1, because
// an un-virtualized AP reports its true CPUID and Windows bugchecks 0x5D
// (UNSUPPORTED_PROCESSOR) the moment it sees processors disagree about SSE4.2.
//
extern volatile UINTN   gHvCpusLaunched;

//
// Local APIC ID of the bootstrap processor, captured at driver load.  Used to
// let exactly one CPU own the on-screen display.
//
extern volatile UINT32  gHvBspApicId;

//
// ---------------------------------------------------------------------------
// Latched diagnostics (Diagnostics.c).
//
// Counters live per-processor in HV_PCPU; these are the "what went wrong"
// fields, where last-one-wins is the right rule and the cost is paid only on a
// failure path.
//
// They exist because a processor that dies cannot report it.  Markers are
// painted into one shared display and the BSP repaints the whole thing every
// 16384 exits, so a white 23 painted by a failing AP is erased within
// milliseconds and the screen shows a healthy heartbeat over a machine that
// has lost a core.  These stick, and the BSP publishes them.
// ---------------------------------------------------------------------------
//
typedef struct _HV_DIAG {
  UINTN   LastEvent;        // 0 none, 1 SIPI, 2 INIT - which path an entry failure followed
  UINTN   LastExitReason;   // basic reason of the most recent exit, any processor
  UINTN   ApResult;         // last RealModeStartAp: 1 = resumed, 2 = un-modeled opcode
  UINT64  LastGpRip;        // guest RIP of the most recent #GP

  //
  // An un-emulated #UD: the single most useful thing this hypervisor can
  // report.  Reaching it means the guest executed an instruction we advertised
  // in CPUID but could not decode, so it saw STATUS_ILLEGAL_INSTRUCTION - an
  // application crashed because of us.  The RIP, opcode bytes and reason code
  // turn "an app crashed" into "implement this opcode".
  //
  UINT64  UdRip;
  UINT32  UdBytes[2];       // first 8 bytes at that RIP
  UINT32  UdWhy;            // why the decoder gave up (see SseTryEmulate)

  //
  // The fate of an application processor, which cannot paint for itself.
  //
  UINTN   ApEntryFailErr;   // VM-instruction error from an AP's rejected VM entry
  UINTN   ApLastReason;     // last exit reason seen on a non-BSP processor
  UINTN   ApLastExcVec;     // last exception vector on an AP
  UINTN   ApLastExcErr;     //   ...its error code
  UINT64  ApLastExcRip;     //   ...guest RIP when it hit

  //
  // The last exception WE synthesized, so a later #DF can name what preceded
  // it rather than only where it landed.
  //
  UINTN   InjVec;
  UINTN   InjErr;
  UINT64  InjCr2;

  //
  // The AP's last exception that was NOT the fatal one.  A #DF says only where
  // a cascade landed; the fault before it is the one to fix, and until this
  // existed we had no record of it at all - the exception bitmap did not even
  // include #PF, so an AP's first fault happened entirely inside the guest.
  //
  UINTN   ApPrevExcVec;
  UINTN   ApPrevExcErr;
  UINT64  ApPrevExcCr2;
  UINT64  ApPrevExcRip;

  //
  // Inj* as they stood AT the #DF.  Those fields are global and live: every
  // processor writes them on every injection, so by the time a #DF is read off
  // a screen they describe some later BSP injection.  Read literally, that says
  // a #DF was ours when it was not - a mistake this snapshot exists to prevent.
  //
  UINTN   DfInjVec;
  UINTN   DfInjErr;
  UINT64  DfInjCr2;

  //
  // Repeat detector for an AP wedged on one faulting instruction.
  //
  UINT64  ApPfLoopRip;
  UINT64  ApPfLoopCr2;
  UINTN   ApPfLoopCount;

  //
  // Exactly what the interpreter handed to hardware, and how far it got before
  // doing so.  An AP that dies with no stack and no KPCR either was given none,
  // or was given them and lost them; nothing else distinguishes those.
  //
  UINT64  ApHandoffRip;
  UINT64  ApHandoffRsp;
  UINT64  ApHandoffGs;
  UINT64  ApHandoffCr3;
  UINTN   ApHandoffSteps;
  BOOLEAN ApHandoffLong;      // committed as 64-bit, or as 32-bit paged

  //
  // The guest state a rejected VM entry was actually holding.  "Invalid guest
  // state" names none of its checks, so the only way forward is to read back
  // what the CPU refused and compare it against them by hand.
  //
  UINTN   EfCr0, EfCr3, EfCr4, EfCsAr, EfTrAr, EfEfer;
  BOOLEAN EfGuestPaging;
  UINT64  EfGuestCr3;
  UINT64  EfGuestEfer;
} HV_DIAG;

#if HV_DIAG_ENABLED
extern volatile HV_DIAG  gHvDiag;
#else
//
// Still declared, so the recording sites compile unchanged, but no longer
// volatile and with every reader compiled away - which makes the stores dead
// and lets the optimiser drop them.  The instance costs a few hundred bytes of
// BSS and nothing at run time.
//
extern HV_DIAG  gHvDiag;
#endif
extern HV_PCPU           gHvPcpuFallback;

//
// Paint the heartbeat (white 30 plus nine rows of live state).  Called from the
// exit handler on the BSP only; see Diagnostics.c for what each row means.
//
VOID       HvHeartbeat       (VOID);
VOID       HvDiagSumCounters (OUT HV_COUNTERS *Total, OUT UINTN *ApExits,
                              OUT UINT64 *BspMicro, OUT UINT64 *ApMicro);

//
// How many APs reached our C entry point.  Compared against gHvCpusLaunched it
// separates "the trampoline worked but VMX bring-up failed" from "the AP never
// arrived at all".
//
extern volatile UINTN   gHvApArrived;

//
// Why AP startup declined, 0 if it did not.  Surfaced on the heartbeat because
// every one of these paths used to fail with nothing but a DEBUG print, and
// DebugLib is the null instance on the machines this runs on.
//
#define HV_AP_FAIL_NONE        0
#define HV_AP_FAIL_NO_LOWPAGE  1   // no free page below 1 MiB for the trampoline
#define HV_AP_FAIL_NO_STACKS   2   // could not allocate the AP stacks
#define HV_AP_FAIL_NO_REGIONS  3   // a per-AP VMX region allocation failed
#define HV_AP_FAIL_CR3_HIGH    4   // host page tables above 4 GiB
#define HV_AP_FAIL_NO_APIC     5   // no usable xAPIC ICR (x2APIC or disabled)
#define HV_AP_FAIL_DISABLED    6   // built with AP startup compiled out
#define HV_AP_FAIL_NO_RESPONSE 7   // IPIs sent, but no AP ever checked in
extern volatile UINT32  gHvApFailReason;

//
// Host page tables, shared with the AP trampoline (see ApStartup.c).
//
extern UINT64           gHostCr3;
extern volatile UINT64  gHvRmFailAddr;    // RealMode.c: last instruction attempted
extern volatile UINT32  gHvRmFailBytes;   //   ...its first four bytes

//
// Mp.c
//
VMX_VCPU * HvGetVcpu        (IN UINTN Index);
VOID       HvBringUpThisCpu (IN VMX_VCPU *Vcpu);

//
// ApStartup.c - application-processor bring-up.  HvPrepareApStartup runs at
// driver load (it allocates); HvStartAps runs inside the ExitBootServices hook
// and returns the number of APs that reported in.
//
VOID       HvPrepareApStartup (VOID);
VOID       HvApReport         (IN UINTN Mark);   // diagnostic builds only
UINTN      HvStartAps         (VOID);
VOID       EFIAPI ApWakeEntry (IN UINTN Slot);

//
// VmxExitHandler.c - called from the assembly when VMRESUME returns (VM entry
// rejected).  Reports the VM-instruction error on screen; the caller halts.
//
VOID       EFIAPI HvVmResumeFailed (VOID);

// RealMode.c - AP startup for CPUs WITHOUT "unrestricted guest".  On a SIPI,
// interpret the guest's real-mode trampoline prologue until it enters protected
// mode, then write protected-mode guest VMCS state so hardware resumes it.
// Returns TRUE if the VMCS was set up to resume; FALSE if the prologue used an
// opcode the interpreter does not model (caller then parks the core).  Regs is
// the guest GPR frame, updated with the interpreted register values.
BOOLEAN    RealModeStartAp   (IN UINT8 SipiVector, IN UINT64 GuestCr3,
                              IN OUT GUEST_REGS *Regs, IN OUT HV_PCPU *Pcpu);

// VmxExitHandler.c - called from AsmVmExitHandler with the GPR frame and the
// FXSAVE image (for XMM-operand SSE4.2 instructions).  EFIAPI is mandatory: the
// assembly passes the arguments in RCX/RDX with Win64 shadow space, which is the
// MSFT convention by default but only applies under GCC/CLANG when the function
// is ms_abi.  Without it a GCC build would read the arguments from RDI/RSI and
// fault on the first VM-exit.
VOID       EFIAPI HandleVmExit (IN OUT GUEST_REGS *Regs, IN OUT VOID *FxArea);

// SseDecoder.c - returns TRUE if it fully emulated the faulting instruction
// and set *InstrLen to its byte length; FALSE if the opcode is not one we
// handle (in which case the caller re-injects #UD).  FxArea is the guest
// FXSAVE image so XMM-based SSE4.2 ops can read/write XMM state.
//
// *FaultAddr is set non-zero when the instruction WAS recognised but its memory
// operand could not be read.  That is not an unknown opcode: the caller owes the
// guest a #PF at that address, not a #UD.
BOOLEAN    SseTryEmulate     (IN OUT GUEST_REGS *Regs,
                              IN OUT VOID       *FxArea,
                              IN     UINT64      GuestRip,
                              IN     UINT64      GuestCr3,
                              IN OUT UINT64     *Rflags,
                              OUT    UINT32     *InstrLen,
                              OUT    UINT64     *FaultAddr,
                              OUT    UINT32     *Why);

// Sse41.c - the SSE4.1 XMM emulators (operate on 16-byte little-endian images).
VOID       Sse41Pmovx        (IN OUT UINT8 *Dst, IN UINT8 *Src,
                              IN UINTN SrcBytes, IN UINTN DstBytes, IN BOOLEAN Sign);
VOID       Sse41Pcmpeqq      (IN OUT UINT8 *Dst, IN UINT8 *Src);
VOID       Sse41Pmulld       (IN OUT UINT8 *Dst, IN UINT8 *Src);
VOID       Sse41Pmuldq       (IN OUT UINT8 *Dst, IN UINT8 *Src);
VOID       Sse41Packusdw     (IN OUT UINT8 *Dst, IN UINT8 *Src);
VOID       Sse41MinMax       (IN OUT UINT8 *Dst, IN UINT8 *Src,
                              IN UINTN ElemBytes, IN BOOLEAN Signd, IN BOOLEAN IsMax);
VOID       Sse41Phminposuw   (IN OUT UINT8 *Dst, IN UINT8 *Src);
VOID       Sse41Ptest        (IN UINT8 *Dst, IN UINT8 *Src, IN OUT UINT64 *Rflags);
VOID       Sse41Blendv       (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 *Mask, IN UINTN ElemBytes);
VOID       Sse41Pblendw      (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8);
VOID       Sse41Insertps     (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8);
VOID       Sse41Mpsadbw      (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8);
VOID       Sse41Round        (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8,
                              IN UINTN ElemBytes, IN BOOLEAN Scalar);
VOID       Sse41Dpps         (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8);
VOID       Sse41Dppd         (IN OUT UINT8 *Dst, IN UINT8 *Src, IN UINT8 Imm8);
UINT64     Sse41GetElem      (IN UINT8 *Src, IN UINTN Index, IN UINTN ElemBytes);
VOID       Sse41SetElem      (IN OUT UINT8 *Dst, IN UINTN Index, IN UINTN ElemBytes, IN UINT64 Val);

// SseString.c - the XMM SSE4.2 emulators.
VOID       SseXmmPcmpGtq     (IN OUT UINT8 *Dst, IN UINT8 *Src);   // signed qword >
VOID       SseXmmPcmpStr     (IN     UINT8   *Op1,        // xmm1 (reg)
                              IN     UINT8   *Op2,        // xmm2/m128 (r/m)
                              IN     UINT8    Imm8,
                              IN     BOOLEAN  ExplicitLen,// TRUE=PCMPESTR*
                              IN     INT64    LenRax,     // explicit length of Op1
                              IN     INT64    LenRdx,     // explicit length of Op2
                              IN     BOOLEAN  IndexForm,  // TRUE=...STRI, FALSE=...STRM
                              OUT    UINT32  *OutIndex,   // ECX result (index form)
                              OUT    UINT8   *OutXmm0,    // XMM0 result (mask form)
                              IN OUT UINT64  *Rflags);

//
// ---------------------------------------------------------------------------
// GuestMem.c - reaching into the guest.
//
// The host runs identity-mapped, so a guest-physical address is dereferenced
// directly; a guest-LINEAR address has to be walked through the guest's own
// page tables first.  Both directions can legitimately fail (the page may be
// paged out), and the caller then owes the guest a #PF rather than a #UD.
// ---------------------------------------------------------------------------
//
BOOLEAN    GuestReadLinear   (IN  UINT64 GuestCr3,
                              IN  UINT64 GuestVa,
                              OUT VOID  *Buffer,
                              IN  UINTN  Length);

BOOLEAN    GuestWriteLinear  (IN UINT64 GuestCr3,
                              IN UINT64 GuestVa,
                              IN VOID  *Buffer,
                              IN UINTN  Length);

//
// Guest GPR access by architectural register number (0=RAX..15=R15).
// GUEST_REGS is laid out in exactly that order, so it indexes as an array -
// except index 4 (RSP), which is a placeholder there and lives in the VMCS.
//
UINT64     HvReadGpr         (IN GUEST_REGS *Regs, IN UINT32 Index);
VOID       HvWriteGpr        (IN OUT GUEST_REGS *Regs, IN UINT32 Index, IN UINT64 Value);

#endif // CORE2AGAIN_H_
