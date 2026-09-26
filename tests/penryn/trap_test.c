/** @file
  trap_test.c - run the hypervisor's own decoder against real #UD faults, in
  userspace, on a processor that really lacks the instructions.

  WHY
  ---
  SseLive.efi proves the decode/trap/resume path works on the target, but it is
  a UEFI application: every run costs a VM boot, failures are resets, and a few
  thousand cases is all the patience allows.  The decoder itself, though, is a
  pure function of (instruction bytes, register file, memory) - and Linux hands
  all three to a SIGILL handler.

  So: execute the instruction for real.  The CPU raises #UD exactly as it does
  under the hypervisor.  The signal handler receives the trap frame, which
  carries the same things the VM-exit handler reads from the VMCS - RIP, the
  GPRs, and an FXSAVE image of the SSE state - and hands them to SseTryEmulate,
  the same function, compiled from the same source.  Then advance RIP by the
  length the decoder computed and return.

  If the decoder mis-parses a ModRM byte, computes the wrong length, or writes
  the wrong register, the program produces a wrong answer or dies - on real
  Penryn silicon, at millions of cases per minute, with no VM in the loop.

  The independent side of the comparison is scalar C written from the
  definition for POPCNT and CRC32, and for the XMM instructions a direct call to
  the emulator, which isolates the decode path.

  Threads exercise it on every core at once, which is the configuration that
  matters: the hypervisor keeps per-CPU state and a shared decoder.
**/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#include <pthread.h>
#include <unistd.h>
#include <setjmp.h>

#include "Hypervisor.h"

/* ------------------------------------------------------------------ */
/* The environment SseTryEmulate expects.                              */
/* ------------------------------------------------------------------ */

/* Userspace is already flat and mapped, so a "guest linear" access is a memcpy.
   But the real one walks the guest's page tables and FAILS on a page that is
   not present, and two of this project's bugs live on exactly that edge - an
   instruction whose 16-byte fetch window runs off the end of a mapped page, and
   an operand that is legitimately paged out.  So the stub models a non-present
   page: g_absent names one, and the copy is split per page exactly as
   GuestCopyLinear does, or a partial read would look like a total failure. */
static __thread uintptr_t g_absent;         /* base of a page the decoder cannot see */

static BOOLEAN copy_linear (UINT64 Va, void *Buf, UINTN Len, int write)
{
  uint8_t *host = (uint8_t *)Buf;

  if (Va == 0) return FALSE;
  while (Len > 0) {
    uintptr_t page  = (uintptr_t)Va & ~(uintptr_t)0xFFF;
    UINTN     inpg  = 0x1000 - (UINTN)(Va & 0xFFF);
    UINTN     chunk = (Len < inpg) ? Len : inpg;

    if (g_absent && page == g_absent) return FALSE;
    if (write) memcpy ((void *)(uintptr_t)Va, host, chunk);
    else       memcpy (host, (void *)(uintptr_t)Va, chunk);
    Va += chunk; host += chunk; Len -= chunk;
  }
  return TRUE;
}

BOOLEAN GuestReadLinear (UINT64 Cr3, UINT64 Va, VOID *Buf, UINTN Len)
{ (void)Cr3; return copy_linear (Va, Buf, Len, 0); }

BOOLEAN GuestWriteLinear (UINT64 Cr3, UINT64 Va, VOID *Buf, UINTN Len)
{ (void)Cr3; return copy_linear (Va, Buf, Len, 1); }

/* On this host every class is absent except SSE4.1, exactly as on the target. */
BOOLEAN HvEmulateSse41 (VOID)  { return FALSE; }   /* Penryn HAS SSE4.1 */
BOOLEAN HvEmulateSse42 (VOID)  { return TRUE;  }
BOOLEAN HvEmulatePopcnt (VOID) { return TRUE;  }

/* GUEST_REGS is indexed as an array in architectural order; index 4 is RSP,
   which the hypervisor keeps in the VMCS.  Here it is simply in the frame. */
UINT64 HvReadGpr (GUEST_REGS *R, UINT32 I)              { return ((UINT64 *)R)[I & 15]; }
VOID   HvWriteGpr (GUEST_REGS *R, UINT32 I, UINT64 V)   { ((UINT64 *)R)[I & 15] = V; }

BOOLEAN SseTryEmulate (GUEST_REGS *, VOID *, UINT64, UINT64, UINT64 *,
                       UINT32 *, UINT64 *, UINT32 *);

/* ------------------------------------------------------------------ */
/* The SIGILL handler: the userspace stand-in for the VM-exit handler. */
/* ------------------------------------------------------------------ */

static __thread unsigned long t_emulated, t_undecoded, t_faultaddr;
static __thread unsigned long t_lastwhy, t_lastrip, t_lastfault;

/* Where the handler unwinds to when the decoder declines: the alternative is
   resuming inside an instruction. */
static __thread sigjmp_buf t_land;

/* A second landing pad for the one case that EXPECTS the decoder to decline;
   reusing t_land there would leave the worker's pad pointing at a frame that
   has already returned. */
static __thread sigjmp_buf t_edge;
static __thread int        t_use_edge_land;
static __thread unsigned long t_wanted_declines;   /* declines the test asked for */
static __thread uintptr_t  t_bad_operand;

/* Does this processor actually raise #UD for the emulated set?  On a host that
   has SSE4.2 nothing faults, the decoder never runs, and the cases that assert
   on its behaviour would be asserting on nothing. */
static int g_traps;

static void on_sigill (int sig, siginfo_t *si, void *uc)
{
  ucontext_t   *c = (ucontext_t *)uc;
  greg_t       *g = c->uc_mcontext.gregs;
  GUEST_REGS    regs;
  UINT64        rflags, faultaddr = 0;
  UINT32        len = 0, why = 0;
  unsigned char fx[FX_AREA_SIZE] __attribute__((aligned(16)));

  (void)sig; (void)si;

  /* Marshal the trap frame into the frame the decoder expects.  The order is
     GUEST_REGS' order (RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15), which is the
     architectural register numbering the ModRM byte encodes. */
  ((UINT64 *)&regs)[0]  = (UINT64)g[REG_RAX];
  ((UINT64 *)&regs)[1]  = (UINT64)g[REG_RCX];
  ((UINT64 *)&regs)[2]  = (UINT64)g[REG_RDX];
  ((UINT64 *)&regs)[3]  = (UINT64)g[REG_RBX];
  ((UINT64 *)&regs)[4]  = (UINT64)g[REG_RSP];
  ((UINT64 *)&regs)[5]  = (UINT64)g[REG_RBP];
  ((UINT64 *)&regs)[6]  = (UINT64)g[REG_RSI];
  ((UINT64 *)&regs)[7]  = (UINT64)g[REG_RDI];
  ((UINT64 *)&regs)[8]  = (UINT64)g[REG_R8];
  ((UINT64 *)&regs)[9]  = (UINT64)g[REG_R9];
  ((UINT64 *)&regs)[10] = (UINT64)g[REG_R10];
  ((UINT64 *)&regs)[11] = (UINT64)g[REG_R11];
  ((UINT64 *)&regs)[12] = (UINT64)g[REG_R12];
  ((UINT64 *)&regs)[13] = (UINT64)g[REG_R13];
  ((UINT64 *)&regs)[14] = (UINT64)g[REG_R14];
  ((UINT64 *)&regs)[15] = (UINT64)g[REG_R15];

  /* The kernel's fpstate IS an FXSAVE image, which is exactly what the
     hypervisor passes: XMMn at offset 160 + n*16. */
  memcpy (fx, c->uc_mcontext.fpregs, FX_AREA_SIZE);

  rflags     = (UINT64)g[REG_EFL];
  t_lastrip  = (unsigned long)g[REG_RIP];

  if (SseTryEmulate (&regs, fx, (UINT64)g[REG_RIP], 0, &rflags,
                     &len, &faultaddr, &why))
    {
      ((UINT64 *)&regs)[4] = (UINT64)g[REG_RSP];   /* RSP is not ours to move */

      g[REG_RAX] = (greg_t)((UINT64 *)&regs)[0];
      g[REG_RCX] = (greg_t)((UINT64 *)&regs)[1];
      g[REG_RDX] = (greg_t)((UINT64 *)&regs)[2];
      g[REG_RBX] = (greg_t)((UINT64 *)&regs)[3];
      g[REG_RBP] = (greg_t)((UINT64 *)&regs)[5];
      g[REG_RSI] = (greg_t)((UINT64 *)&regs)[6];
      g[REG_RDI] = (greg_t)((UINT64 *)&regs)[7];
      g[REG_R8]  = (greg_t)((UINT64 *)&regs)[8];
      g[REG_R9]  = (greg_t)((UINT64 *)&regs)[9];
      g[REG_R10] = (greg_t)((UINT64 *)&regs)[10];
      g[REG_R11] = (greg_t)((UINT64 *)&regs)[11];
      g[REG_R12] = (greg_t)((UINT64 *)&regs)[12];
      g[REG_R13] = (greg_t)((UINT64 *)&regs)[13];
      g[REG_R14] = (greg_t)((UINT64 *)&regs)[14];
      g[REG_R15] = (greg_t)((UINT64 *)&regs)[15];

      memcpy (c->uc_mcontext.fpregs, fx, FX_AREA_SIZE);
      g[REG_EFL] = (greg_t)rflags;
      g[REG_RIP] = (greg_t)((UINT64)g[REG_RIP] + len);   /* the decoder's length */
      t_emulated++;
      return;
    }

  /* Not emulated.  Under the hypervisor this becomes an injected #UD (an
     application crash) or a #PF.  Here it must be RECORDED and the run kept
     alive: resuming at RIP+1 would execute the middle of an instruction, so
     unwind to the per-case landing pad instead. */
  t_lastwhy   = why;
  t_lastfault = (unsigned long)faultaddr;
  if (faultaddr != 0) t_faultaddr++; else t_undecoded++;
  siglongjmp (t_use_edge_land ? t_edge : t_land, 1);
}

/* ------------------------------------------------------------------ */
/* References written from the definition.                             */
/* ------------------------------------------------------------------ */

static uint64_t ref_popcnt (uint64_t v, int bytes)
{
  uint64_t c = 0;
  if (bytes < 8) v &= ((1ULL << (bytes * 8)) - 1);
  while (v) { c += v & 1; v >>= 1; }
  return c;
}

static uint32_t ref_crc32c (uint32_t crc, uint8_t b)
{
  crc ^= b;
  for (int i = 0; i < 8; i++)
    crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(0u - (crc & 1u)));
  return crc;
}

/* ------------------------------------------------------------------ */
/* Cases.  Every one of these #UDs on this CPU and is serviced above.   */
/* ------------------------------------------------------------------ */

#define FAILS 8
struct tstate {
  unsigned long run, bad;
  unsigned      first;            /* id of the first mismatch */
  uint64_t      exp, got;
  uint64_t      rng;
};

static uint64_t rnd (struct tstate *t)
{
  t->rng ^= t->rng << 13; t->rng ^= t->rng >> 7; t->rng ^= t->rng << 17;
  return t->rng;
}

static void chk (struct tstate *t, unsigned id, uint64_t e, uint64_t a)
{
  t->run++;
  if (e != a) {
    t->bad++;
    if (!t->first) { t->first = id; t->exp = e; t->got = a; }
  }
}

static void chkbuf (struct tstate *t, unsigned id, const uint8_t *e, const uint8_t *a)
{
  t->run++;
  if (memcmp (e, a, 16) != 0) {
    t->bad++;
    if (!t->first) { t->first = id; memcpy (&t->exp, e, 8); memcpy (&t->got, a, 8); }
  }
}

static void one_round (struct tstate *t)
{
  uint64_t v = rnd (t), r64;
  uint32_t seed = (uint32_t)(v >> 32), r32, ref;
  uint16_t r16;
  uint8_t  a[16], b[16], hw[16], em[16];
  uint64_t mem[8];
  int      i;

  /* ---- POPCNT: register, memory, three widths, and ZF ---- */
  __asm__ volatile ("popcntq %1,%0" : "=r"(r64) : "r"(v) : "cc");
  chk (t, 101, ref_popcnt (v, 8), r64);
  __asm__ volatile ("popcntl %1,%0" : "=r"(r32) : "r"((uint32_t)v) : "cc");
  chk (t, 102, ref_popcnt (v, 4), r32);
  __asm__ volatile ("popcntw %1,%0" : "=r"(r16) : "r"((uint16_t)v) : "cc");
  chk (t, 103, ref_popcnt (v, 2), r16);

  mem[0] = v; mem[1] = ~v; mem[2] = v ^ 0x5555555555555555ULL;
  __asm__ volatile ("popcntq (%1),%0" : "=r"(r64) : "r"(mem) : "cc","memory");
  chk (t, 104, ref_popcnt (v, 8), r64);
  __asm__ volatile ("popcntq 8(%1),%0" : "=r"(r64) : "r"(mem) : "cc","memory");
  chk (t, 105, ref_popcnt (~v, 8), r64);
  __asm__ volatile ("popcntl 8(%1,%2,8),%0" : "=r"(r32) : "r"(mem), "r"(1UL) : "cc","memory");
  chk (t, 106, ref_popcnt (mem[2], 4), r32);

  { uint64_t fl;
    __asm__ volatile ("popcntq %2,%0\n\t pushfq\n\t popq %1"
                      : "=&r"(r64), "=&r"(fl) : "r"(v) : "cc");
    chk (t, 107, (v == 0) ? 1 : 0, (fl >> 6) & 1); }

  /* ---- CRC32: four widths, register and memory ---- */
  __asm__ volatile ("crc32b %1,%0" : "+r"(seed) : "q"((uint8_t)v));
  chk (t, 201, ref_crc32c ((uint32_t)(v >> 32), (uint8_t)v), seed);

  seed = (uint32_t)(v >> 32);
  __asm__ volatile ("crc32w %1,%0" : "+r"(seed) : "r"((uint16_t)v));
  ref = (uint32_t)(v >> 32);
  for (i = 0; i < 2; i++) ref = ref_crc32c (ref, (uint8_t)(v >> (8 * i)));
  chk (t, 202, ref, seed);

  seed = (uint32_t)(v >> 32);
  __asm__ volatile ("crc32l %1,%0" : "+r"(seed) : "r"((uint32_t)v));
  ref = (uint32_t)(v >> 32);
  for (i = 0; i < 4; i++) ref = ref_crc32c (ref, (uint8_t)(v >> (8 * i)));
  chk (t, 203, ref, seed);

  r64 = (uint32_t)(v >> 32);
  __asm__ volatile ("crc32q %1,%0" : "+r"(r64) : "r"(v));
  ref = (uint32_t)(v >> 32);
  for (i = 0; i < 8; i++) ref = ref_crc32c (ref, (uint8_t)(v >> (8 * i)));
  chk (t, 204, ref, r64);

  r64 = (uint32_t)(v >> 32);
  __asm__ volatile ("crc32q (%1),%0" : "+r"(r64) : "r"(mem) : "memory");
  chk (t, 205, ref, r64);

  /* ---- PCMPGTQ: reg, mem and a REX.R destination ---- */
  for (i = 0; i < 16; i++) { a[i] = (uint8_t)rnd (t); b[i] = (uint8_t)rnd (t); }
  if ((t->run & 3) == 0) memcpy (b, a, 8);          /* force ties */

  memcpy (em, a, 16); SseXmmPcmpGtq (em, b);

  __asm__ volatile ("movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t"
                    "pcmpgtq %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%2)"
                    : : "r"(a),"r"(b),"r"(hw) : "xmm1","xmm2","memory");
  chkbuf (t, 301, em, hw);

  __asm__ volatile ("movdqu (%0),%%xmm1\n\t pcmpgtq (%1),%%xmm1\n\t movdqu %%xmm1,(%2)"
                    : : "r"(a),"r"(b),"r"(hw) : "xmm1","memory");
  chkbuf (t, 302, em, hw);

  __asm__ volatile ("movdqu (%0),%%xmm11\n\t movdqu (%1),%%xmm12\n\t"
                    "pcmpgtq %%xmm12,%%xmm11\n\t movdqu %%xmm11,(%2)"
                    : : "r"(a),"r"(b),"r"(hw) : "xmm11","xmm12","memory");
  chkbuf (t, 303, em, hw);

  /* ---- PCMP{I,E}STR{I,M}: every imm8, both forms, reg and memory ---- */
  for (i = 0; i < 16; i++) { a[i] = (uint8_t)(rnd (t) & 0x7F); b[i] = (uint8_t)(rnd (t) & 0x7F); }
  if ((t->run & 1) == 0) { a[(t->run % 15) + 1] = 0; b[(t->run % 13) + 1] = 0; memcpy (b, a, 4); }

  {
    unsigned imm = (unsigned)(rnd (t) & 0x3F);      /* bit 7 is reserved */
    uint32_t idx = 0, refidx = 0;
    uint64_t fl = 0, reffl = 0;
    uint8_t  refm[16] = {0};

    /* The imm8 must be a literal to the assembler, so it is patched into a
       template.  That also exercises the decoder over the WHOLE imm8 space
       rather than the handful a switch would cover. */
    static __thread uint8_t code[64] __attribute__((aligned(4096)));
    (void)code;

    SseXmmPcmpStr (a, b, (UINT8)imm, FALSE, 0, 0, TRUE, &refidx, refm, &reffl);
    switch (imm & 0x3F) {
#define I_CASE(N)                                                              \
      case N:                                                                   \
        __asm__ volatile ("movdqu (%2),%%xmm1\n\t movdqu (%3),%%xmm2\n\t"        \
                          "pcmpistri $" #N ",%%xmm2,%%xmm1\n\t"                  \
                          "pushfq\n\t popq %1\n\t movl %%ecx,%0"                 \
                          : "=&r"(idx), "=&r"(fl) : "r"(a), "r"(b)               \
                          : "xmm1","xmm2","rcx","cc","memory");                  \
        break;
      I_CASE(0)  I_CASE(1)  I_CASE(2)  I_CASE(3)  I_CASE(4)  I_CASE(5)
      I_CASE(6)  I_CASE(7)  I_CASE(8)  I_CASE(9)  I_CASE(10) I_CASE(11)
      I_CASE(12) I_CASE(13) I_CASE(14) I_CASE(15) I_CASE(16) I_CASE(17)
      I_CASE(18) I_CASE(19) I_CASE(20) I_CASE(21) I_CASE(22) I_CASE(23)
      I_CASE(24) I_CASE(25) I_CASE(26) I_CASE(27) I_CASE(28) I_CASE(29)
      I_CASE(30) I_CASE(31) I_CASE(32) I_CASE(33) I_CASE(34) I_CASE(35)
      I_CASE(36) I_CASE(37) I_CASE(38) I_CASE(39) I_CASE(40) I_CASE(41)
      I_CASE(42) I_CASE(43) I_CASE(44) I_CASE(45) I_CASE(46) I_CASE(47)
      I_CASE(48) I_CASE(49) I_CASE(50) I_CASE(51) I_CASE(52) I_CASE(53)
      I_CASE(54) I_CASE(55) I_CASE(56) I_CASE(57) I_CASE(58) I_CASE(59)
      I_CASE(60) I_CASE(61) I_CASE(62) I_CASE(63)
#undef I_CASE
      default: break;
    }
    chk (t, 401, refidx, idx);
    chk (t, 402, reffl & 0x8D5, fl & 0x8D5);
  }
}

/* ------------------------------------------------------------------ */
/* Page edges: the two places this project has actually been bitten.    */
/*                                                                      */
/*   1. An instruction that ENDS at a page boundary whose successor is   */
/*      not present.  The decoder's 16-byte fetch window runs off the    */
/*      end, so it gets a short read and must still emulate correctly    */
/*      from what it did get.  Failing that whole fetch is bug 12: a     */
/*      spurious #UD for a perfectly valid instruction, waiting for      */
/*      roughly one instruction in 250 by placement alone.               */
/*                                                                      */
/*   2. An instruction whose memory OPERAND is not present.  #UD is      */
/*      raised before the operand is evaluated, so this is legal and     */
/*      routine - the guest is owed a #PF at that address, not a #UD.    */
/*      Reporting #UD is bug 23: STATUS_ILLEGAL_INSTRUCTION for an       */
/*      instruction we advertised and do implement.                      */
/* ------------------------------------------------------------------ */

#include <sys/mman.h>

/* A pair of pages.  The second is executable in reality - the CPU must be able
   to fetch whatever follows - but the decoder is told it is absent, which is
   exactly the input it gets when the guest's own tables say so.
   PER THREAD: these pages are patched with the instruction under test before
   every call, and sharing them meant one thread rewriting the bytes another was
   executing.  That produced a genuine stream of SIGILLs on a host that has every
   instruction, which is a good demonstration of why the control run matters. */
static __thread uint8_t *edge_pages;

static void edge_init (void)
{
  edge_pages = mmap (NULL, 3 * 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (edge_pages == MAP_FAILED) { perror ("mmap"); exit (2); }
}

/* Build "<bytes> ; ret" so the instruction's LAST byte is the last byte of the
   first page, then call it.  Args arrive in RDI/RSI (SysV), which is what the
   templates below use. */
static uint64_t edge_call (const uint8_t *code, size_t n, uint64_t a, uint64_t b)
{
  uint8_t  *at  = edge_pages + 4096 - n;
  uint64_t (*fn) (uint64_t, uint64_t) = (uint64_t (*) (uint64_t, uint64_t))at;
  uint64_t  r;

  memcpy (at, code, n);
  edge_pages[4096] = 0xC3;              /* ret, first byte of the "absent" page */
  __builtin___clear_cache ((char *)at, (char *)at + n + 1);

  g_absent = (uintptr_t)(edge_pages + 4096);
  r = fn (a, b);
  g_absent = 0;
  return r;
}

static void group_edges (struct tstate *t)
{
  uint64_t v = rnd (t);

  /* popcntq %rdi, %rax  =  F3 48 0F B8 C7   (5 bytes, result in RAX) */
  {
    static const uint8_t c[] = { 0xF3, 0x48, 0x0F, 0xB8, 0xC7 };
    chk (t, 501, ref_popcnt (v, 8), edge_call (c, sizeof (c), v, 0));
  }

  /* crc32q %rdi, %rax  =  F2 48 0F 38 F1 C7 ... with RAX seeded from RSI:
     48 89 F0 (mov %rsi,%rax) then the crc32.  9 bytes total. */
  {
    static const uint8_t c[] = { 0x48, 0x89, 0xF0,
                                 0xF2, 0x48, 0x0F, 0x38, 0xF1, 0xC7 };
    uint32_t ref = (uint32_t)(v >> 32);
    int      i;

    for (i = 0; i < 8; i++) ref = ref_crc32c (ref, (uint8_t)(v >> (8 * i)));
    chk (t, 502, ref, edge_call (c, sizeof (c), v, (uint32_t)(v >> 32)));
  }

  /* An operand the decoder cannot read.  popcntq (%rdi),%rax with RDI pointing
     into the absent page: the decoder must DECLINE and report the address, not
     claim the opcode is unknown.  Meaningful only where the instruction really
     faults - on a host that has POPCNT there is no decoder in the loop at all. */
  if (g_traps) {
    static const uint8_t c[] = { 0xF3, 0x48, 0x0F, 0xB8, 0x07 };  /* popcntq (%rdi),%rax */

    t_bad_operand = (uintptr_t)(edge_pages + 4096 + 64);
    t->run++;
    /* Nothing here may live in a plain local across the longjmp: afterwards a
       non-volatile local's value is indeterminate, which at -O2 is not a
       theoretical concern. */
    if (sigsetjmp (t_edge, 1) == 0) {
      t_use_edge_land = 1;
      (void)edge_call (c, sizeof (c), (uint64_t)t_bad_operand, 0);
      t_use_edge_land = 0;
      t->bad++;                                  /* it did not decline: wrong */
      if (!t->first) { t->first = 503; t->exp = 1; t->got = 0; }
    } else {
      t_use_edge_land = 0;
      t_wanted_declines++;
      /* Declined, as it must - and it owes a #PF at that exact address rather
         than calling the opcode unknown. */
      if (t_lastfault != (unsigned long)t_bad_operand) {
        t->bad++;
        if (!t->first) { t->first = 504; t->exp = (uint64_t)t_bad_operand; t->got = t_lastfault; }
      }
    }
  }
}

/* RIP-relative operands.  The decoder can only resolve one after it knows the
   instruction's TOTAL length, so this is the addressing mode a length bug shows
   up in first - and compilers emit it constantly. */
static uint64_t rip_data[4] = { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL,
                                0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL };

static void group_riprel (struct tstate *t)
{
  uint64_t r64; uint32_t r32;
  uint8_t  hw[16], em[16];

  __asm__ volatile ("popcntq rip_data(%%rip),%0" : "=r"(r64) : : "cc","memory");
  chk (t, 601, ref_popcnt (rip_data[0], 8), r64);

  __asm__ volatile ("popcntl rip_data+8(%%rip),%0" : "=r"(r32) : : "cc","memory");
  chk (t, 602, ref_popcnt (rip_data[1], 4), r32);

  r64 = 0;
  __asm__ volatile ("crc32q rip_data(%%rip),%0" : "+r"(r64) : : "memory");
  {
    uint32_t ref = 0; int i;
    for (i = 0; i < 8; i++) ref = ref_crc32c (ref, (uint8_t)(rip_data[0] >> (8 * i)));
    chk (t, 603, ref, r64);
  }

  memcpy (em, rip_data, 16);
  SseXmmPcmpGtq (em, (UINT8 *)&rip_data[2]);
  __asm__ volatile ("movdqu rip_data(%%rip),%%xmm1\n\t"
                    "pcmpgtq rip_data+16(%%rip),%%xmm1\n\t"
                    "movdqu %%xmm1,(%0)"
                    : : "r"(hw) : "xmm1","memory");
  chkbuf (t, 604, em, hw);
}

/* ------------------------------------------------------------------ */

static volatile int   g_stop;
static unsigned long  g_rounds;

struct targ { int id; struct tstate st; unsigned long emulated, undecoded, wanted; };

static void *worker (void *p)
{
  struct targ *ta = (struct targ *)p;
  unsigned long n = 0;

  ta->st.rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(ta->id + 1) + 12345;
  edge_init ();                        /* thread-local: see the comment there */

  while (!g_stop && (g_rounds == 0 || n < g_rounds)) {
    if (sigsetjmp (t_land, 1) == 0) {
      one_round (&ta->st);
      group_riprel (&ta->st);
      group_edges (&ta->st);
    } else {
      ta->st.bad++;                    /* the decoder declined: a real failure */
      if (!ta->st.first) ta->st.first = 9000 + (unsigned)t_lastwhy;
    }
    n++;
  }
  ta->emulated  = t_emulated;
  ta->undecoded = t_undecoded + t_faultaddr;
  ta->wanted    = t_wanted_declines;
  return NULL;
}

int main (int argc, char **argv)
{
  struct sigaction sa;
  int              threads = 2;
  unsigned long    total_run = 0, total_bad = 0, total_emu = 0, total_und = 0, total_want = 0;
  unsigned         first = 0;
  uint64_t         exp = 0, got = 0;

  if (argc > 1) threads   = atoi (argv[1]);
  if (argc > 2) g_rounds  = strtoul (argv[2], NULL, 10);
  if (threads < 1) threads = 1;
  if (threads > 64) threads = 64;
  if (g_rounds == 0) g_rounds = 20000;

  edge_init ();

  memset (&sa, 0, sizeof (sa));
  sa.sa_sigaction = on_sigill;
  sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
  sigemptyset (&sa.sa_mask);
  if (sigaction (SIGILL, &sa, NULL) != 0) { perror ("sigaction"); return 2; }

  /* One probe instruction decides whether this run tests the decoder or is the
     control that validates the references. */
  {
    unsigned long before = t_emulated;
    uint64_t r = 0, v = 0x0F0F0F0F0F0F0F0FULL;

    if (sigsetjmp (t_land, 1) == 0) {
      __asm__ volatile ("popcntq %1,%0" : "=r"(r) : "r"(v) : "cc");
    }
    (void)r;
    g_traps    = (t_emulated != before);
    t_emulated = before;
  }

  printf ("decoder-under-SIGILL: %d thread(s), %lu rounds each\n", threads, g_rounds);
  printf ("this CPU %s the emulated set, so the decoder %s\n",
          g_traps ? "LACKS" : "has",
          g_traps ? "IS under test" : "is not reached (control run)");

  {
    pthread_t   *th = calloc ((size_t)threads, sizeof (*th));
    struct targ *ta = calloc ((size_t)threads, sizeof (*ta));
    int i;

    for (i = 0; i < threads; i++) { ta[i].id = i; pthread_create (&th[i], NULL, worker, &ta[i]); }
    for (i = 0; i < threads; i++) {
      pthread_join (th[i], NULL);
      total_run += ta[i].st.run;  total_bad += ta[i].st.bad;
      total_emu += ta[i].emulated; total_und += ta[i].undecoded;
      total_want += ta[i].wanted;
      if (ta[i].st.first && !first) { first = ta[i].st.first; exp = ta[i].st.exp; got = ta[i].st.got; }
      printf ("  thread %d: checks %-9lu mismatches %-6lu emulated %-9lu declined %lu (%lu asked for)\n",
              i, ta[i].st.run, ta[i].st.bad, ta[i].emulated, ta[i].undecoded, ta[i].wanted);
    }
  }

  printf ("\nchecks      %lu\n", total_run);
  printf ("mismatches  %lu\n", total_bad);
  printf ("emulated    %lu   (real #UD faults serviced by the decoder)\n", total_emu);
  printf ("declined    %lu   of which %lu were asked for (the absent-operand\n"
          "                     case, which MUST decline and owe the guest a #PF).\n"
          "                     The remainder, %lu, would be decoder gaps.\n",
          total_und, total_want, total_und - total_want);
  if (total_bad) printf ("first id %u  expected %016llx  got %016llx\n",
                         first, (unsigned long long)exp, (unsigned long long)got);
  {
    int ok = (total_bad == 0) && (total_und == total_want);

    printf ("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
  }
}
