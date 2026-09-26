// AP startup trampoline: real mode (SIPI) -> protected mode -> long mode ->
// ApWakeEntry().
//
// Copied verbatim into a page-aligned page below 1 MiB; the SIPI vector is that
// page's frame number, so execution begins at offset 0 with CS = base>>4, IP = 0.
//
// Position independent by construction: the page's own physical base is
// recovered from CS (base = CS << 4) into EBX and every absolute address is
// formed from it, so nothing needs relocating.  Only the data area at the end is
// patched by the BSP (CR3, stack base/size, entry point).
//
// The mode transition deliberately goes real -> 32-bit protected -> long, the
// sequence in SDM Vol. 3A "Initializing IA-32e Mode", rather than setting
// CR0.PE and CR0.PG together and jumping straight from real mode to 64-bit.
// The shortcut leaves the instructions between MOV CR0 and the far transfer
// executing in IA-32e mode through a code-segment cache loaded by real mode,
// which is not an architecturally defined state - and this runs on a Core 2,
// where "works on my machine" is not evidence.
//
// It also clears CR0.CD and CR0.NW.  A processor coming out of INIT has both
// set, i.e. caches disabled, and nothing else would ever clear them: the AP
// would be virtualized with that CR0 and run every instruction uncached for the
// rest of the boot.
//
// Assembled by scripts/gen_ap_trampoline.sh; the resulting bytes live in
// ApStartup.c as gApTrampoline[].  Edit this file, re-run the script, paste.

#define DATA        0xF00
#define OFF_GDT     (DATA + 0x00)       // 5 descriptors, 40 bytes
#define OFF_GDTR    (DATA + 0x30)       // limit16 + base32
#define OFF_CR3     (DATA + 0x40)
#define OFF_STKBASE (DATA + 0x48)
#define OFF_STKSIZE (DATA + 0x50)
#define OFF_ENTRY   (DATA + 0x58)
#define OFF_SLOT    (DATA + 0x60)
#define OFF_PROG    (DATA + 0x68)       // how far this AP got (1..4)
#define OFF_STOPAT  (DATA + 0x69)       // halt when PROG reaches this; 0 = never
#define OFF_STKIDX  (DATA + 0x6A)       // word: next trampoline-stack index

// Selectors in the GDT built by ApStartup.c.
#define SEL_CODE64  0x08
#define SEL_DATA64  0x10
#define SEL_CODE32  0x18
#define SEL_DATA32  0x20

    .text
    .globl _start
_start:
    .code16
    cli
    cld
    movw    %cs, %ax
    movw    %ax, %ds
    movw    %ax, %ss
    movzwl  %ax, %ebx
    shll    $4, %ebx                // EBX = physical base of this page
    // Claim a private trampoline stack.
    //
    // Every AP runs this same code at the same time, and both mode switches are
    // a push/lret pair - so a single shared stack means two processors overwrite
    // each other's return address and one of them jumps somewhere arbitrary.
    // With four processors under QEMU that lost a random AP on roughly half the
    // runs.  Eight 256-byte stacks fit between the code (ends ~0x120) and the
    // data area at 0xF00.
    movw    $OFF_STKIDX, %si
    movw    $1, %ax
    lock xaddw %ax, (%si)           // AX = my index, claimed atomically
    andw    $7, %ax
    shlw    $8, %ax                 // index * 256
    movw    $0xE00, %dx
    subw    %ax, %dx                // 0xE00, 0xD00, 0xC00, ...
    movw    %dx, %sp
    movzwl  %dx, %ebp               // keep it for protected mode - NOT %esi,
                                    // which the GDTR setup below reloads

    // Leave a breadcrumb at each stage.  An AP that faults here cannot report
    // anything - there is no IDT yet, so any exception is a triple fault and a
    // machine reset - but the byte it last wrote survives in the page for the
    // BSP to read back.
    movw    $OFF_PROG, %si
    movb    $1, (%si)               // 1 = executing in real mode

    // GDTR.base = EBX + OFF_GDT   (the limit is pre-set in the data area)
    movl    %ebx, %eax
    addl    $OFF_GDT, %eax
    movw    $(OFF_GDTR + 2), %si
    movl    %eax, (%si)
    movw    $OFF_GDTR, %si
    lgdtl   (%si)

    // Real -> 32-bit protected mode: PE only, paging still off.
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    pushl   $SEL_CODE32
    movl    %ebx, %eax
    addl    $(prot32 - _start), %eax
    pushl   %eax
    lretl

    .code32
prot32:
    //
    // Reload the data segments FIRST, before touching memory.  Setting CR0.PE
    // does not touch the hidden parts of DS/ES/SS: they still hold what real
    // mode cached, which is base = selector << 4 and - fatally - limit 0xFFFF.
    // This page lives at 0x97000, so the very first EBX-relative store is to
    // offset ~0x97F68, outside that stale 64 KiB limit: #GP, and with no IDT
    // yet a #GP is a triple fault and a silent machine reset.
    //
    // A breadcrumb placed above these four instructions is not a diagnostic,
    // it is the crash.  That is exactly what the previous revision did.
    //
    movl    $SEL_DATA32, %eax
    movl    %eax, %ds
    movl    %eax, %es
    movl    %eax, %ss
    leal    (%ebx,%ebp), %esp       // this AP's own stack, now a flat address
    movb    $2, OFF_PROG(%ebx)      // 2 = 32-bit protected mode reached

    // PAE, plus OSFXSR/OSXMMEXCPT, then the host page tables (guaranteed
    // < 4 GiB by the caller).
    //
    // A processor out of INIT has CR4 = 0, so OSFXSR is clear and every SSE
    // instruction on this core would #UD - with no IDT, a triple fault.  The C
    // code the trampoline is about to call is compiled for x86-64, whose
    // baseline includes SSE2, so the compiler is free to emit one at any time.
    // It also matters after bring-up: with OSFXSR clear, the FXSAVE/FXRSTOR
    // pair in the VM-exit stub silently skips the XMM registers, and the SSE4
    // emulators write their results into exactly that image.  The BSP already
    // runs with these set; the AP has to match it.
    movl    %cr4, %eax
    orl     $0x620, %eax            // PAE | OSFXSR | OSXMMEXCPT
    movl    %eax, %cr4
    movl    OFF_CR3(%ebx), %eax
    movl    %eax, %cr3

    // EFER.LME
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x100, %eax
    wrmsr

    // Paging on -> long mode.  Clear CD/NW in the same write: INIT leaves both
    // set, and this is the only place that would ever turn the caches back on.
    movl    %cr0, %eax
    andl    $0x9FFFFFFF, %eax       // ~(CD | NW)
    orl     $0x80000000, %eax       // PG
    movl    %eax, %cr0

    pushl   $SEL_CODE64
    movl    %ebx, %eax
    addl    $(entry64 - _start), %eax
    pushl   %eax
    lret

    .code64
entry64:
    //
    // Re-establish RBX as a 64-bit value.  EBX has carried the page base since
    // real mode and its low 32 bits survive the mode switches, but the UPPER 32
    // bits of RBX are architecturally undefined until something in 64-bit mode
    // writes them.  Every access below is RBX-relative, so inheriting garbage
    // up there sends the very first one to a wild linear address - which faults,
    // and with no IDT yet a fault is a triple fault and a silent machine reset.
    // A 32-bit move zero-extends, which is precisely what is wanted.
    //
    movl    %ebx, %ebx

    movb    $3, OFF_PROG(%rbx)          // 3 = long mode reached

    //
    // Optional stop: the BSP sets OFF_STOPAT in diagnostic builds so an AP can
    // be halted at a known stage rather than running on into whatever is
    // broken.  A halted AP can be reported; a faulted one resets the machine.
    //
    movzbl  OFF_STOPAT(%rbx), %eax
    cmpl    $3, %eax
    je      2f

    movl    $SEL_DATA64, %eax
    movl    %eax, %ds
    movl    %eax, %es
    movl    %eax, %ss

    // Claim a slot; each AP gets its own stack, so they need no lock.
    movq    $1, %rax
    lock xaddq %rax, OFF_SLOT(%rbx)     // RAX = my 0-based slot
    movq    %rax, %r8

    movq    OFF_STKBASE(%rbx), %rdx
    movq    OFF_STKSIZE(%rbx), %rcx
    leaq    1(%r8), %rax
    imulq   %rcx, %rax
    addq    %rdx, %rax
    andq    $-16, %rax
    movq    %rax, %rsp                  // top of this AP's stack

    movq    %r8, %rcx                   // arg0 = slot index (Microsoft x64)
    subq    $0x20, %rsp                 // shadow space
    movb    $4, OFF_PROG(%rbx)          // 4 = about to enter C
    movq    OFF_ENTRY(%rbx), %rax
    call    *%rax

    // ApWakeEntry only returns if bring-up failed: stop harmlessly.
2:  cli
    hlt
    jmp     2b
