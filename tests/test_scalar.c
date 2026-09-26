#include <stdio.h>
#include <stdint.h>
#include <string.h>
/* Pull the decoder in with its statics exposed, stubbing the VMX environment. */
#define CONST const
#define SIZE_4KB 0x1000
typedef struct { uint64_t r[16]; } GUEST_REGS;
#define FX_XMM0_OFFSET 160
static inline uint8_t AsmVmRead(uint64_t f, uint64_t *v){ (void)f; *v=0; return 0; }
static inline uint8_t AsmVmWrite(uint64_t f, uint64_t v){ (void)f; (void)v; return 0; }
#define VMCS_GUEST_RSP 0x681C
static inline uint8_t HvEmulateSse41(void){ return 1; }
static inline uint8_t HvEmulateSse42(void){ return 1; }
static inline uint8_t HvEmulatePopcnt(void){ return 1; }
#include "Hypervisor.h"

/* SseDecoder.c reaches the guest through GuestMem.c, which needs a live VMCS
   and guest page tables.  Nothing this test calls goes near them: it exercises
   Crc32cByte and PopCount, the two pure scalar helpers.  Stub the rest so the
   translation unit links, and make every guest access fail loudly rather than
   quietly appearing to work. */
static inline UINT64 HvReadGpr (GUEST_REGS *R, UINT32 I) { return R->r[I & 15]; }
static inline VOID   HvWriteGpr (GUEST_REGS *R, UINT32 I, UINT64 V) { R->r[I & 15] = V; }
static inline BOOLEAN GuestReadLinear (UINT64 Cr3, UINT64 Va, VOID *B, UINTN N)
{ (void)Cr3; (void)Va; (void)B; (void)N; return FALSE; }
static inline BOOLEAN GuestWriteLinear (UINT64 Cr3, UINT64 Va, VOID *B, UINTN N)
{ (void)Cr3; (void)Va; (void)B; (void)N; return FALSE; }

#undef STATIC
#define STATIC
#include "SseDecoder.c"

int main(void){
  uint64_t s = 0x853C49E6748FEA9BULL;
  long bad = 0, n = 0;

  /* CRC32 - reflected CRC-32C, one byte at a time */
  for (int i = 0; i < 200000; i++){
    s ^= s<<13; s ^= s>>7; s ^= s<<17;
    uint32_t crc = (uint32_t)(s>>32);
    uint8_t  by  = (uint8_t)s;
    uint32_t emu = Crc32cByte(crc, by);
    uint32_t hw;
    __asm__ volatile("crc32b %2,%0" : "=r"(hw) : "0"(crc), "r"(by));
    n++; if (emu != hw){ if(bad++<3) printf("CRC32 mismatch crc=%08x byte=%02x emu=%08x hw=%08x\n",crc,by,emu,hw); }
  }

  /* POPCNT over each operand width */
  for (int i = 0; i < 200000; i++){
    s ^= s<<13; s ^= s>>7; s ^= s<<17;
    for (int w = 0; w < 3; w++){
      uint64_t bytes = (w==0)?2:((w==1)?4:8);
      uint64_t emu = PopCount(s, bytes);
      uint64_t hw;
      if (bytes==2){ uint16_t v=(uint16_t)s; uint32_t r; __asm__("popcntw %1,%w0":"=r"(r):"r"(v)); hw=(uint16_t)r; }
      else if (bytes==4){ uint32_t v=(uint32_t)s, r; __asm__("popcntl %1,%0":"=r"(r):"r"(v)); hw=r; }
      else { uint64_t v=s, r; __asm__("popcntq %1,%0":"=r"(r):"r"(v)); hw=r; }
      n++; if (emu != hw){ if(bad++<3) printf("POPCNT mismatch w=%llu val=%016llx emu=%llu hw=%llu\n",
                                (unsigned long long)bytes,(unsigned long long)s,
                                (unsigned long long)emu,(unsigned long long)hw); }
    }
  }
  printf("%ld / %ld mismatched\n", bad, n);
  return bad!=0;
}
