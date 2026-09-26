#!/usr/bin/env python3
"""
Emit the two generated differential-test programs.

Both compare an emulator against the real instruction executed by this host.
The hardware side needs the imm8 as a literal, so each instruction becomes a
256-way (or 128-way) switch - hence generating the C rather than writing it.

    gen.py <output-dir>
"""
import sys, os

OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# --------------------------------------------------------------------------
# SSE4.2 string instructions: PCMPISTRI/M, PCMPESTRI/M
# --------------------------------------------------------------------------
STR_HDR = r"""
/* Differential test: emulator vs real hardware, SSE4.2 string instructions. */
#include <stdio.h>
#include <stdlib.h>
#include "Hypervisor.h"

#define FLMASK (BIT0|BIT2|BIT4|BIT6|BIT7|BIT11)   /* CF PF AF ZF SF OF */

static uint64_t rng_s = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void){ rng_s ^= rng_s<<13; rng_s ^= rng_s>>7; rng_s ^= rng_s<<17; return rng_s; }

#define HW_ISTRI(I) do { uint32_t _i; uint64_t _f; __asm__ volatile ( \
    "movdqu (%2),%%xmm1\n\t movdqu (%3),%%xmm2\n\t" \
    "pcmpistri $" #I ",%%xmm2,%%xmm1\n\t" \
    "pushfq\n\t pop %1\n\t mov %%ecx,%0\n\t" \
    : "=&r"(_i), "=&r"(_f) : "r"(a), "r"(b) \
    : "xmm1","xmm2","rcx","cc","memory"); *idx=_i; *fl=_f; } while(0)

#define HW_ISTRM(I) do { uint64_t _f; __asm__ volatile ( \
    "movdqu (%1),%%xmm1\n\t movdqu (%2),%%xmm2\n\t" \
    "pcmpistrm $" #I ",%%xmm2,%%xmm1\n\t" \
    "pushfq\n\t pop %0\n\t movdqu %%xmm0,(%3)\n\t" \
    : "=&r"(_f) : "r"(a), "r"(b), "r"(m) \
    : "xmm0","xmm1","xmm2","cc","memory"); *fl=_f; } while(0)

#define HW_ESTRI(I) do { uint32_t _i; uint64_t _f; __asm__ volatile ( \
    "movdqu (%2),%%xmm1\n\t movdqu (%3),%%xmm2\n\t" \
    "pcmpestri $" #I ",%%xmm2,%%xmm1\n\t" \
    "pushfq\n\t pop %1\n\t mov %%ecx,%0\n\t" \
    : "=&r"(_i), "=&r"(_f) : "r"(a), "r"(b), "a"(la), "d"(ld) \
    : "xmm1","xmm2","rcx","cc","memory"); *idx=_i; *fl=_f; } while(0)

#define HW_ESTRM(I) do { uint64_t _f; __asm__ volatile ( \
    "movdqu (%1),%%xmm1\n\t movdqu (%2),%%xmm2\n\t" \
    "pcmpestrm $" #I ",%%xmm2,%%xmm1\n\t" \
    "pushfq\n\t pop %0\n\t movdqu %%xmm0,(%3)\n\t" \
    : "=&r"(_f) : "r"(a), "r"(b), "r"(m), "a"(la), "d"(ld) \
    : "xmm0","xmm1","xmm2","cc","memory"); *fl=_f; } while(0)
"""

STR_MAIN = r"""
static void mkvec(uint8_t *v, int mode){
  for (int i=0;i<16;i++) v[i] = (uint8_t)(rnd()>>13);
  if (mode==1) { for (int i=0;i<16;i++) v[i] = (uint8_t)('a' + (v[i]%26)); v[rnd()%16]=0; }
  if (mode==2) { for (int i=0;i<16;i+=2){ v[i]=(uint8_t)('a'+(v[i]%26)); v[i+1]=0; }
                 int z=(rnd()%8)*2; v[z]=0; v[z+1]=0; }
  if (mode==3) { for (int i=0;i<16;i++) v[i]=0; }
}

int main(void){
  uint8_t a[16],b[16],hm[16],em[16];
  long fails=0, tot=0;
  int first=1;

  for (int iter=0; iter<4000; iter++){
    int mode = iter % 4;
    mkvec(a,mode); mkvec(b,mode);
    for (int imm=0; imm<0x80; imm++){
      uint32_t hi=0, ei=0; uint64_t hf=0, ef=0x202;
      uint8_t eo[16];

      hw_istri(a,b,imm,&hi,&hf);
      ef=0x202; SseXmmPcmpStr(a,b,(UINT8)imm,FALSE,0,0,TRUE,&ei,eo,&ef);
      tot++;
      if (hi!=ei || (hf&FLMASK)!=(ef&FLMASK)) {
        fails++;
        if (first){ first=0;
          printf("FIRST MISMATCH  PCMPISTRI imm=0x%02x\n", imm);
          printf("  op1 :"); for(int k=0;k<16;k++) printf(" %02x",a[k]); printf("\n");
          printf("  op2 :"); for(int k=0;k<16;k++) printf(" %02x",b[k]); printf("\n");
          printf("  hw  : idx=%u flags=%03llx\n", hi, (unsigned long long)(hf&FLMASK));
          printf("  emu : idx=%u flags=%03llx\n", ei, (unsigned long long)(ef&FLMASK));
        }
      }

      hw_istrm(a,b,imm,hm,&hf);
      ef=0x202; SseXmmPcmpStr(a,b,(UINT8)imm,FALSE,0,0,FALSE,&ei,em,&ef);
      tot++;
      if (memcmp(hm,em,16)!=0 || (hf&FLMASK)!=(ef&FLMASK)) fails++;

      for (int t=0;t<3;t++){
        int32_t la=(int32_t)(rnd()%20)-2, ld=(int32_t)(rnd()%20)-2;
        hw_estri(a,b,imm,la,ld,&hi,&hf);
        ef=0x202; SseXmmPcmpStr(a,b,(UINT8)imm,TRUE,la,ld,TRUE,&ei,eo,&ef);
        tot++;
        if (hi!=ei || (hf&FLMASK)!=(ef&FLMASK)) fails++;

        hw_estrm(a,b,imm,la,ld,hm,&hf);
        ef=0x202; SseXmmPcmpStr(a,b,(UINT8)imm,TRUE,la,ld,FALSE,&ei,em,&ef);
        tot++;
        if (memcmp(hm,em,16)!=0 || (hf&FLMASK)!=(ef&FLMASK)) fails++;
      }
    }
  }
  printf("PCMP*STR* : %ld / %ld mismatched\n", fails, tot);
  return fails!=0;
}
"""

def switch(name, sig, macro, lo=0, hi=0x80, mask="0x7f"):
    out = [f"static void {name}({sig}) {{", f"  switch (imm & {mask}) {{"]
    out += [f"  case {i}: {macro}({i}); break;" for i in range(lo, hi)]
    out += ["  }", "}"]
    return "\n".join(out)

with open(os.path.join(OUT, "test_str.c"), "w") as f:
    f.write(STR_HDR)
    f.write(switch("hw_istri","const uint8_t*a,const uint8_t*b,int imm,uint32_t*idx,uint64_t*fl","HW_ISTRI")+"\n")
    f.write(switch("hw_istrm","const uint8_t*a,const uint8_t*b,int imm,uint8_t*m,uint64_t*fl","HW_ISTRM")+"\n")
    f.write(switch("hw_estri","const uint8_t*a,const uint8_t*b,int imm,int32_t la,int32_t ld,uint32_t*idx,uint64_t*fl","HW_ESTRI")+"\n")
    f.write(switch("hw_estrm","const uint8_t*a,const uint8_t*b,int imm,int32_t la,int32_t ld,uint8_t*m,uint64_t*fl","HW_ESTRM")+"\n")
    f.write(STR_MAIN)

# --------------------------------------------------------------------------
# SSE4.1
# --------------------------------------------------------------------------
S41_HDR = r"""
/* Differential test: SSE4.1 emulators vs real hardware. */
#include <stdio.h>
#include <stdlib.h>
#include "Hypervisor.h"

#define FLMASK (BIT0|BIT2|BIT4|BIT6|BIT7|BIT11)
static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return rs; }

static long fails=0, tot=0;
static void chk(const char*name,int imm,const uint8_t*hw,const uint8_t*em,
                const uint8_t*a,const uint8_t*b){
  tot++;
  if (memcmp(hw,em,16)==0) return;
  if (fails++ < 6){
    printf("MISMATCH %s imm=0x%02x\n", name, imm);
    printf("   a  :"); for(int k=0;k<16;k++) printf(" %02x",a[k]); printf("\n");
    printf("   b  :"); for(int k=0;k<16;k++) printf(" %02x",b[k]); printf("\n");
    printf("   hw :"); for(int k=0;k<16;k++) printf(" %02x",hw[k]); printf("\n");
    printf("   emu:"); for(int k=0;k<16;k++) printf(" %02x",em[k]); printf("\n");
  }
}

#define OP2(mnem) do { __asm__ volatile( \
   "movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t" \
   mnem " %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%0)\n\t" \
   :: "r"(d), "r"(s) : "xmm1","xmm2","memory"); } while(0)

#define OP2I(mnem,I) do { __asm__ volatile( \
   "movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t" \
   mnem " $" #I ",%%xmm2,%%xmm1\n\t movdqu %%xmm1,(%0)\n\t" \
   :: "r"(d), "r"(s) : "xmm1","xmm2","memory"); } while(0)

#define OPBV(mnem) do { __asm__ volatile( \
   "movdqu (%0),%%xmm1\n\t movdqu (%1),%%xmm2\n\t movdqu (%2),%%xmm0\n\t" \
   mnem " %%xmm2,%%xmm1\n\t movdqu %%xmm1,(%0)\n\t" \
   :: "r"(d), "r"(s), "r"(m) : "xmm0","xmm1","xmm2","memory"); } while(0)
"""

SIMPLE = ["pcmpeqq","pmulld","pmuldq","packusdw","phminposuw",
          "pminsb","pminsd","pminuw","pminud","pmaxsb","pmaxsd","pmaxuw","pmaxud",
          "pmovsxbw","pmovsxbd","pmovsxbq","pmovsxwd","pmovsxwq","pmovsxdq",
          "pmovzxbw","pmovzxbd","pmovzxbq","pmovzxwd","pmovzxwq","pmovzxdq"]
IMMOPS = ["pblendw","insertps","mpsadbw","roundps","roundpd","roundss","roundsd","dpps","dppd"]
BLENDV = ["pblendvb","blendvps","blendvpd"]

S41_MAIN = r"""
static void hw_ptest(const uint8_t*a,const uint8_t*b,uint64_t*fl){
  uint64_t f; __asm__ volatile(
   "movdqu (%1),%%xmm1\n\t movdqu (%2),%%xmm2\n\t"
   "ptest %%xmm2,%%xmm1\n\t pushfq\n\t pop %0\n\t"
   : "=&r"(f) : "r"(a), "r"(b) : "xmm1","xmm2","cc","memory"); *fl=f;
}

static int has_nan32(const uint8_t*v){
  for(int i=0;i<4;i++){ uint32_t u; memcpy(&u,v+4*i,4);
    if (((u>>23)&0xFF)==0xFF && (u&0x7FFFFF)) return 1; } return 0; }
static int has_nan64(const uint8_t*v){
  for(int i=0;i<2;i++){ uint64_t u; memcpy(&u,v+8*i,8);
    if (((u>>52)&0x7FF)==0x7FF && (u&0xFFFFFFFFFFFFFULL)) return 1; } return 0; }

int main(void){
  uint8_t a[16],b[16],msk[16],hw[16],em[16];
  for (int iter=0; iter<20000; iter++){
    for(int i=0;i<16;i++){ a[i]=(uint8_t)(rnd()>>11); b[i]=(uint8_t)(rnd()>>11); msk[i]=(uint8_t)(rnd()>>11); }
    if (iter%3==0){ for(int i=0;i<16;i++){ a[i]=(uint8_t)(rnd()>>17); b[i]=(uint8_t)(rnd()>>17);} }
    if (iter%5==0){ float*fa=(float*)a,*fb=(float*)b;
      for(int i=0;i<4;i++){ fa[i]=(float)((int64_t)(rnd()%2000)-1000)/8.0f;
                            fb[i]=(float)((int64_t)(rnd()%2000)-1000)/8.0f; } }

#define T2(fn,call) do{ memcpy(hw,a,16); fn(hw,b); memcpy(em,a,16); call; \
                        chk(#fn,0,hw,em,a,b); }while(0)
    T2(hw_pcmpeqq,   Sse41Pcmpeqq(em,b));
    T2(hw_pmulld,    Sse41Pmulld(em,b));
    T2(hw_pmuldq,    Sse41Pmuldq(em,b));
    T2(hw_packusdw,  Sse41Packusdw(em,b));
    T2(hw_phminposuw,Sse41Phminposuw(em,b));
    T2(hw_pminsb,    Sse41MinMax(em,b,1,TRUE ,FALSE));
    T2(hw_pminsd,    Sse41MinMax(em,b,4,TRUE ,FALSE));
    T2(hw_pminuw,    Sse41MinMax(em,b,2,FALSE,FALSE));
    T2(hw_pminud,    Sse41MinMax(em,b,4,FALSE,FALSE));
    T2(hw_pmaxsb,    Sse41MinMax(em,b,1,TRUE ,TRUE ));
    T2(hw_pmaxsd,    Sse41MinMax(em,b,4,TRUE ,TRUE ));
    T2(hw_pmaxuw,    Sse41MinMax(em,b,2,FALSE,TRUE ));
    T2(hw_pmaxud,    Sse41MinMax(em,b,4,FALSE,TRUE ));
    T2(hw_pmovsxbw,  Sse41Pmovx(em,b,1,2,TRUE ));
    T2(hw_pmovsxbd,  Sse41Pmovx(em,b,1,4,TRUE ));
    T2(hw_pmovsxbq,  Sse41Pmovx(em,b,1,8,TRUE ));
    T2(hw_pmovsxwd,  Sse41Pmovx(em,b,2,4,TRUE ));
    T2(hw_pmovsxwq,  Sse41Pmovx(em,b,2,8,TRUE ));
    T2(hw_pmovsxdq,  Sse41Pmovx(em,b,4,8,TRUE ));
    T2(hw_pmovzxbw,  Sse41Pmovx(em,b,1,2,FALSE));
    T2(hw_pmovzxbd,  Sse41Pmovx(em,b,1,4,FALSE));
    T2(hw_pmovzxbq,  Sse41Pmovx(em,b,1,8,FALSE));
    T2(hw_pmovzxwd,  Sse41Pmovx(em,b,2,4,FALSE));
    T2(hw_pmovzxwq,  Sse41Pmovx(em,b,2,8,FALSE));
    T2(hw_pmovzxdq,  Sse41Pmovx(em,b,4,8,FALSE));

    { memcpy(hw,a,16); hw_pblendvb(hw,b,msk); memcpy(em,a,16); Sse41Blendv(em,b,msk,1); chk("pblendvb",0,hw,em,a,b); }
    { memcpy(hw,a,16); hw_blendvps(hw,b,msk); memcpy(em,a,16); Sse41Blendv(em,b,msk,4); chk("blendvps",0,hw,em,a,b); }
    { memcpy(hw,a,16); hw_blendvpd(hw,b,msk); memcpy(em,a,16); Sse41Blendv(em,b,msk,8); chk("blendvpd",0,hw,em,a,b); }

    { uint64_t hf=0, ef=0x202; hw_ptest(a,b,&hf); Sse41Ptest(a,b,&ef); tot++;
      if ((hf&FLMASK)!=(ef&FLMASK)){ if(fails++<6) printf("MISMATCH ptest hw=%03llx emu=%03llx\n",
        (unsigned long long)(hf&FLMASK),(unsigned long long)(ef&FLMASK)); } }

    for (int imm=0; imm<256; imm++){
      memcpy(hw,a,16); hw_pblendw(hw,b,imm); memcpy(em,a,16); Sse41Pblendw(em,b,(UINT8)imm);
      chk("pblendw",imm,hw,em,a,b);
      memcpy(hw,a,16); hw_insertps(hw,b,imm); memcpy(em,a,16); Sse41Insertps(em,b,(UINT8)imm);
      chk("insertps",imm,hw,em,a,b);
      memcpy(hw,a,16); hw_mpsadbw(hw,b,imm); memcpy(em,a,16); Sse41Mpsadbw(em,b,(UINT8)imm);
      chk("mpsadbw",imm,hw,em,a,b);

      /* DPPS/DPPD with a NaN input is implementation-specific - the host leaks
         internal sum-tree temporaries into different result lanes.  Matching one
         CPU's quirk would mis-model another, so compare defined inputs only. */
      if (!has_nan32(a) && !has_nan32(b)) {
        memcpy(hw,a,16); hw_dpps(hw,b,imm); memcpy(em,a,16); Sse41Dpps(em,b,(UINT8)imm);
        chk("dpps",imm,hw,em,a,b);
      }
      if (!has_nan64(a) && !has_nan64(b)) {
        memcpy(hw,a,16); hw_dppd(hw,b,imm); memcpy(em,a,16); Sse41Dppd(em,b,(UINT8)imm);
        chk("dppd",imm,hw,em,a,b);
      }
      if ((imm & 0x08)==0) {   /* rounding mode from imm; MXCSR path not modelled */
        memcpy(hw,a,16); hw_roundps(hw,b,imm); memcpy(em,a,16); Sse41Round(em,b,(UINT8)imm,4,FALSE);
        chk("roundps",imm,hw,em,a,b);
        memcpy(hw,a,16); hw_roundpd(hw,b,imm); memcpy(em,a,16); Sse41Round(em,b,(UINT8)imm,8,FALSE);
        chk("roundpd",imm,hw,em,a,b);
        memcpy(hw,a,16); hw_roundss(hw,b,imm); memcpy(em,a,16); Sse41Round(em,b,(UINT8)imm,4,TRUE);
        chk("roundss",imm,hw,em,a,b);
        memcpy(hw,a,16); hw_roundsd(hw,b,imm); memcpy(em,a,16); Sse41Round(em,b,(UINT8)imm,8,TRUE);
        chk("roundsd",imm,hw,em,a,b);
      }
    }
  }
  printf("SSE4.1    : %ld / %ld mismatched\n", fails, tot);
  return fails!=0;
}
"""

with open(os.path.join(OUT, "test41.c"), "w") as f:
    f.write(S41_HDR)
    for m in SIMPLE:
        f.write(f'static void hw_{m}(uint8_t*d,const uint8_t*s){{ OP2("{m}"); }}\n')
    for m in IMMOPS:
        f.write(f'static void hw_{m}(uint8_t*d,const uint8_t*s,int imm){{ switch(imm&0xff){{\n')
        for i in range(256):
            f.write(f'  case {i}: OP2I("{m}",{i}); break;\n')
        f.write('  } }\n')
    for m in BLENDV:
        f.write(f'static void hw_{m}(uint8_t*d,const uint8_t*s,const uint8_t*m_){{ const uint8_t*m=m_; OPBV("{m}"); }}\n')
    f.write(S41_MAIN)

print("generated test_str.c and test41.c in", OUT)
