;------------------------------------------------------------------------------
; VmxAsm.asm - MASM/ml64 low-level support for Core2Again.
;
;   * Thin wrappers around the VMX instructions (VMXON/VMCLEAR/VMPTRLD/VMREAD/
;     VMWRITE/INVEPT/VMXOFF), returning 0 / 1 / 2 for OK / VMfailInvalid /
;     VMfailValid.
;   * Segment-register and LAR/LSL readers used to snapshot guest state.
;   * AsmVmxLaunch - performs the "launch that returns into the guest" trick.
;   * AsmVmExitHandler - the HOST_RIP target: saves guest GPRs, calls the C
;     router, restores GPRs, VMRESUME.
;
; Calling convention: Microsoft x64 (EFIAPI on X64).  Args: RCX, RDX, R8, R9.
; Return in RAX.  Nonvolatile: RBX RBP RDI RSI R12-R15.
;------------------------------------------------------------------------------

OPTION CASEMAP:NONE

VMCS_GUEST_RSP EQU 0681Ch
VMCS_GUEST_RIP EQU 0681Eh
FXSZ           EQU 512            ; FXSAVE image size (guest x87/SSE state)

EXTERN HandleVmExit:PROC
EXTERN HvVmResumeFailed:PROC

PUBLIC AsmVmxOn
PUBLIC AsmVmxOff
PUBLIC AsmVmClear
PUBLIC AsmVmPtrLd
PUBLIC AsmVmWrite
PUBLIC AsmVmRead
PUBLIC AsmInvept
PUBLIC AsmLoadAr
PUBLIC AsmLoadLimit
PUBLIC AsmGetRsp
PUBLIC AsmGetRflags
PUBLIC AsmXsetbv
PUBLIC AsmAtomicInc
PUBLIC AsmVmxLaunch
PUBLIC AsmVmExitHandler

.CODE

;
; Convert the flags left by a VMX instruction into a status code in RAX.
;   CF=1 -> 1 (VMfailInvalid), ZF=1 -> 2 (VMfailValid), else 0.
;
VMX_STATUS MACRO
    LOCAL inv, val
    jc   inv
    jz   val
    xor  rax, rax
    ret
inv:
    mov  rax, 1
    ret
val:
    mov  rax, 2
    ret
ENDM

; UINT8 AsmVmxOn (UINT64 *VmxonPhysAddr)   ; RCX -> pointer to phys addr
AsmVmxOn PROC
    vmxon   qword ptr [rcx]
    VMX_STATUS
AsmVmxOn ENDP

; VOID AsmVmxOff (VOID)
AsmVmxOff PROC
    vmxoff
    ret
AsmVmxOff ENDP

; UINT8 AsmVmClear (UINT64 *VmcsPhysAddr)
AsmVmClear PROC
    vmclear qword ptr [rcx]
    VMX_STATUS
AsmVmClear ENDP

; UINT8 AsmVmPtrLd (UINT64 *VmcsPhysAddr)
AsmVmPtrLd PROC
    vmptrld qword ptr [rcx]
    VMX_STATUS
AsmVmPtrLd ENDP

; UINT8 AsmVmWrite (UINTN Field, UINTN Value)   ; RCX=field, RDX=value
AsmVmWrite PROC
    vmwrite rcx, rdx
    VMX_STATUS
AsmVmWrite ENDP

; UINT8 AsmVmRead (UINTN Field, UINTN *Value)   ; RCX=field, RDX=out ptr
AsmVmRead PROC
    vmread  rax, rcx
    mov     qword ptr [rdx], rax     ; MOV does not affect CF/ZF
    VMX_STATUS
AsmVmRead ENDP

; UINT8 AsmInvept (UINTN Type, VOID *Descriptor)  ; RCX=type, RDX=descriptor
AsmInvept PROC
    invept  rcx, oword ptr [rdx]
    VMX_STATUS
AsmInvept ENDP

;

; UINT32 AsmLoadAr (UINT16 Selector)   ; access rights via LAR (0 if invalid)
AsmLoadAr PROC
    lar eax, ecx
    jz  arok            ; LAR sets ZF=1 on success
    xor eax, eax
arok:
    ret
AsmLoadAr ENDP

; UINT32 AsmLoadLimit (UINT16 Selector)  ; segment limit via LSL (0 if invalid)
AsmLoadLimit PROC
    lsl eax, ecx
    jz  lslok           ; LSL sets ZF=1 on success
    xor eax, eax
lslok:
    ret
AsmLoadLimit ENDP

; UINTN AsmGetRsp (VOID)  ; caller's RSP (above our return address)
AsmGetRsp PROC
    lea rax, [rsp+8]
    ret
AsmGetRsp ENDP

; UINTN AsmGetRflags (VOID)
AsmGetRflags PROC
    pushfq
    pop rax
    ret
AsmGetRflags ENDP

; VOID AsmXsetbv (UINT32 Index, UINT64 Value)   ; RCX=index, RDX=value
AsmXsetbv PROC
    mov rax, rdx            ; EAX = value[31:0]
    shr rdx, 32             ; EDX = value[63:32]
    ; ECX already holds the XCR index
    xsetbv
    ret
AsmXsetbv ENDP

; UINTN AsmAtomicInc (volatile UINTN *Counter)   ; RCX=pointer, returns new value
;
; The AP counters are written by every processor at once.  A plain ++ is a
; read-modify-write, so concurrent increments lose updates - observed as three
; vCPUs reported on a four-processor machine, which is precisely the number
; someone reads to decide whether multi-core worked.
AsmAtomicInc PROC
    mov  rax, 1
    lock xadd qword ptr [rcx], rax  ; RAX = old value
    inc  rax                        ; ...return the new one
    ret
AsmAtomicInc ENDP

;
; UINTN AsmVmxLaunch (VOID)
;
; Writes GUEST_RSP = current RSP and GUEST_RIP = GuestResume, then VMLAUNCH.
; On success the guest begins at GuestResume with the same stack, so control
; "returns" to the C caller (RAX = 0) but now virtualized.  On failure the
; host path returns (UINTN)-1.
;
AsmVmxLaunch PROC
    mov  rax, VMCS_GUEST_RSP
    mov  rdx, rsp
    vmwrite rax, rdx

    mov  rax, VMCS_GUEST_RIP
    lea  rdx, GuestResume
    vmwrite rax, rdx

    vmlaunch
    ; Reached only if VMLAUNCH failed to enter the guest.
    xor  rax, rax
    dec  rax                ; RAX = (UINTN)-1
    ret

GuestResume:
    ; First instructions executed by the guest.  RSP already points at the
    ; caller's return address.
    xor  rax, rax
    ret
AsmVmxLaunch ENDP

;
; VOID AsmVmExitHandler (VOID)   ; HOST_RIP
;
; Layout pushed here MUST match struct GUEST_REGS (RAX at the lowest address).
;
AsmVmExitHandler PROC
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    sub  rsp, 8             ; placeholder for the Rsp field (guest RSP is in VMCS)
    push rbx
    push rdx
    push rcx
    push rax                ; 128 bytes pushed total -> RSP 16-byte aligned

    sub  rsp, FXSZ          ; 512-byte, 16-aligned FXSAVE buffer
    fxsave [rsp]            ; capture guest x87/SSE (XMM) state

    lea  rcx, [rsp + FXSZ]  ; arg0 = GUEST_REGS * (points at saved RAX)
    mov  rdx, rsp           ; arg1 = FXSAVE image (XMM operands live here)
    sub  rsp, 20h           ; Win64 shadow space (keeps 16-byte alignment)
    call HandleVmExit
    add  rsp, 20h

    fxrstor [rsp]           ; restore possibly-modified SSE state to the guest
    add  rsp, FXSZ

    pop  rax
    pop  rcx
    pop  rdx
    pop  rbx
    add  rsp, 8             ; discard Rsp placeholder
    pop  rbp
    pop  rsi
    pop  rdi
    pop  r8
    pop  r9
    pop  r10
    pop  r11
    pop  r12
    pop  r13
    pop  r14
    pop  r15

    vmresume

    ; VMRESUME returns only when VM entry failed - the VMCS describes a guest
    ; the CPU will not accept.  Every push has been undone, so RSP is the host
    ; stack top and a normal call is safe: say which check failed instead of
    ; halting silently.
resume_fail:
    sub  rsp, 20h                   ; Win64 shadow space
    call HvVmResumeFailed
halt_forever:
    cli
    hlt
    jmp  halt_forever
AsmVmExitHandler ENDP

END
