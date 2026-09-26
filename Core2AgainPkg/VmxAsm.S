//------------------------------------------------------------------------------
// VmxAsm.S - GNU as (AT&T syntax) port of VmxAsm.asm, for the GCC / CLANGDWARF
// toolchains.  VmxAsm.asm (MASM/ml64) remains the source the MSFT toolchain
// builds; Core2Again.inf selects one per tool family, so the two files
// MUST be kept in step - any change to one belongs in the other.
//
// Calling convention: Microsoft x64.  Every function here is declared EFIAPI in
// Hypervisor.h, and under GCC/CLANG on X64 edk2 defines
//     EFIAPI = __attribute__((ms_abi))
// so the C side agrees with the register use below.  Args: RCX, RDX, R8, R9.
// Return in RAX.  Nonvolatile: RBX RBP RDI RSI R12-R15.
//
// AT&T reverses MASM's operand order, so MASM
//     vmwrite rcx, rdx      ; field = RCX, value = RDX
// is written here as
//     vmwrite %rdx, %rcx
// Likewise "vmread rax, rcx" (dest RAX, field RCX) becomes "vmread %rcx, %rax".
//
// The module is compiled -fpie -mcmodel=small, so the one absolute code address
// taken (GuestResume) is formed RIP-relative.
//------------------------------------------------------------------------------

#define VMCS_GUEST_RSP  0x681C
#define VMCS_GUEST_RIP  0x681E
#define FXSZ            512             // FXSAVE image size (guest x87/SSE state)

    .text

//
// Convert the flags left by a VMX instruction into a status code in RAX.
//   CF=1 -> 1 (VMfailInvalid), ZF=1 -> 2 (VMfailValid), else 0.
// Numeric local labels so the macro can expand many times in one file.
//
.macro VMX_STATUS
    jc      9f
    jz      8f
    xorq    %rax, %rax
    ret
8:  movq    $2, %rax
    ret
9:  movq    $1, %rax
    ret
.endm

// UINT8 AsmVmxOn (UINT64 *VmxonPhysAddr)   ; RCX -> pointer to phys addr
    .globl  AsmVmxOn
AsmVmxOn:
    vmxon   (%rcx)
    VMX_STATUS

// VOID AsmVmxOff (VOID)
    .globl  AsmVmxOff
AsmVmxOff:
    vmxoff
    ret

// UINT8 AsmVmClear (UINT64 *VmcsPhysAddr)
    .globl  AsmVmClear
AsmVmClear:
    vmclear (%rcx)
    VMX_STATUS

// UINT8 AsmVmPtrLd (UINT64 *VmcsPhysAddr)
    .globl  AsmVmPtrLd
AsmVmPtrLd:
    vmptrld (%rcx)
    VMX_STATUS

// UINT8 AsmVmWrite (UINTN Field, UINTN Value)   ; RCX=field, RDX=value
    .globl  AsmVmWrite
AsmVmWrite:
    vmwrite %rdx, %rcx
    VMX_STATUS

// UINT8 AsmVmRead (UINTN Field, UINTN *Value)   ; RCX=field, RDX=out ptr
    .globl  AsmVmRead
AsmVmRead:
    vmread  %rcx, %rax
    movq    %rax, (%rdx)            // MOV does not affect CF/ZF
    VMX_STATUS

// UINT8 AsmInvept (UINTN Type, VOID *Descriptor)  ; RCX=type, RDX=descriptor
    .globl  AsmInvept
AsmInvept:
    invept  (%rdx), %rcx
    VMX_STATUS

// UINT32 AsmLoadAr (UINT16 Selector)   ; access rights via LAR (0 if invalid)
    .globl  AsmLoadAr
AsmLoadAr:
    lar     %ecx, %eax
    jz      1f                      // LAR sets ZF=1 on success
    xorl    %eax, %eax
1:  ret

// UINT32 AsmLoadLimit (UINT16 Selector)  ; segment limit via LSL (0 if invalid)
    .globl  AsmLoadLimit
AsmLoadLimit:
    lsl     %ecx, %eax
    jz      1f                      // LSL sets ZF=1 on success
    xorl    %eax, %eax
1:  ret

// UINTN AsmGetRsp (VOID)  ; caller's RSP (above our return address)
    .globl  AsmGetRsp
AsmGetRsp:
    leaq    8(%rsp), %rax
    ret

// UINTN AsmGetRflags (VOID)
    .globl  AsmGetRflags
AsmGetRflags:
    pushfq
    popq    %rax
    ret

// VOID AsmXsetbv (UINT32 Index, UINT64 Value)   ; RCX=index, RDX=value
    .globl  AsmXsetbv
AsmXsetbv:
    movq    %rdx, %rax              // EAX = value[31:0]
    shrq    $32, %rdx               // EDX = value[63:32]
    xsetbv                          // ECX already holds the XCR index
    ret

// UINTN AsmAtomicInc (volatile UINTN *Counter)   ; RCX=pointer, returns new value
//
// The AP counters are written by every processor at once.  A plain ++ is a
// read-modify-write, so concurrent increments lose updates - observed as three
// vCPUs reported on a four-processor machine, which is precisely the number
// someone reads to decide whether multi-core worked.
    .globl  AsmAtomicInc
AsmAtomicInc:
    movq    $1, %rax
    lock xaddq %rax, (%rcx)         // RAX = old value
    incq    %rax                    // ...return the new one
    ret

//
// UINTN AsmVmxLaunch (VOID)
//
// Writes GUEST_RSP = current RSP and GUEST_RIP = GuestResume, then VMLAUNCH.
// On success the guest begins at GuestResume with the same stack, so control
// "returns" to the C caller (RAX = 0) but now virtualized.  On failure the
// host path returns (UINTN)-1.
//
    .globl  AsmVmxLaunch
AsmVmxLaunch:
    movq    $VMCS_GUEST_RSP, %rax
    movq    %rsp, %rdx
    vmwrite %rdx, %rax

    movq    $VMCS_GUEST_RIP, %rax
    leaq    GuestResume(%rip), %rdx
    vmwrite %rdx, %rax

    vmlaunch
    // Reached only if VMLAUNCH failed to enter the guest.
    xorq    %rax, %rax
    decq    %rax                    // RAX = (UINTN)-1
    ret

GuestResume:
    // First instructions executed by the guest.  RSP already points at the
    // caller's return address.
    xorq    %rax, %rax
    ret

//
// VOID AsmVmExitHandler (VOID)   ; HOST_RIP
//
// Layout pushed here MUST match struct GUEST_REGS (RAX at the lowest address).
//
    .globl  AsmVmExitHandler
AsmVmExitHandler:
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %r11
    pushq   %r10
    pushq   %r9
    pushq   %r8
    pushq   %rdi
    pushq   %rsi
    pushq   %rbp
    subq    $8, %rsp                // placeholder for the Rsp field (guest RSP is in VMCS)
    pushq   %rbx
    pushq   %rdx
    pushq   %rcx
    pushq   %rax                    // 128 bytes pushed total -> RSP 16-byte aligned

    subq    $FXSZ, %rsp             // 512-byte, 16-aligned FXSAVE buffer
    fxsave  (%rsp)                  // capture guest x87/SSE (XMM) state

    leaq    FXSZ(%rsp), %rcx        // arg0 = GUEST_REGS * (points at saved RAX)
    movq    %rsp, %rdx              // arg1 = FXSAVE image (XMM operands live here)
    subq    $0x20, %rsp             // Win64 shadow space (keeps 16-byte alignment)
    call    HandleVmExit
    addq    $0x20, %rsp

    fxrstor (%rsp)                  // restore possibly-modified SSE state to the guest
    addq    $FXSZ, %rsp

    popq    %rax
    popq    %rcx
    popq    %rdx
    popq    %rbx
    addq    $8, %rsp                // discard Rsp placeholder
    popq    %rbp
    popq    %rsi
    popq    %rdi
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15

    vmresume

    //
    // VMRESUME returns only when VM entry failed - the VMCS describes a guest
    // the CPU will not accept.  Every push has been undone, so RSP is the host
    // stack top and a normal call is safe: say which check failed instead of
    // halting silently.
    //
resume_fail:
    subq    $0x20, %rsp             // Win64 shadow space
    call    HvVmResumeFailed
1:  cli
    hlt
    jmp     1b
