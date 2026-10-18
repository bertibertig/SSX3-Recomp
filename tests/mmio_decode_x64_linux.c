/* Test für den x86-64-MMIO-Decoder über Linux-ucontext (xboxrecomp-Patch 0001).
 *   gcc -Wall -Wextra -Ixboxrecomp/src/platform tests/mmio_decode_x64_linux.c -o /tmp/t && /tmp/t   */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "mmio_decode.h"
static uint32_t regs[64]; static int last_wr_size; static uint32_t last_wr_off;
static uint64_t rd(void *d, uint32_t off, int size){ (void)d; uint64_t v=0; memcpy(&v,(char*)regs+off,size); return v; }
static void wr(void *d, uint32_t off, uint64_t v, int size){ (void)d; memcpy((char*)regs+off,&v,size); last_wr_size=size; last_wr_off=off; }
static int fails;
#define CHECK(n,c) do{ if(!(c)){printf("FAIL: %s\n",n);fails++;} else printf("ok:   %s\n",n);}while(0)
static int run(const uint8_t *code, size_t len, ucontext_t *uc, uint32_t off){
    uc->uc_mcontext.gregs[REG_RIP]=(greg_t)(uintptr_t)code;
    int r=mmio_emulate(uc,off,NULL,rd,wr);
    return r && (size_t)(uc->uc_mcontext.gregs[REG_RIP]-(greg_t)(uintptr_t)code)==len;
}
int main(void){
    ucontext_t uc; memset(&uc,0,sizeof uc);
    regs[0x40/4]=0x12345678;
    { const uint8_t c[]={0x8B,0x40,0x40};            /* mov eax,[rax+0x40] */
      uc.uc_mcontext.gregs[REG_RAX]=0xFFFFFFFFFFFFFFFFull;
      CHECK("mov eax,[m] read + zero-extend", run(c,3,&uc,0x40) && (uint64_t)uc.uc_mcontext.gregs[REG_RAX]==0x12345678ull); }
    { const uint8_t c[]={0x44,0x89,0x40,0x10};       /* mov [rax+0x10],r8d */
      uc.uc_mcontext.gregs[REG_R8]=0xCAFEBABE;
      CHECK("mov [m],r8d write (REX.R)", run(c,4,&uc,0x10) && regs[0x10/4]==0xCAFEBABE && last_wr_size==4); }
    { const uint8_t c[]={0xF7,0x40,0x40,0x00,0x00,0x00,0x10};  /* test dword [rax+0x40],0x10000000 */
      uc.uc_mcontext.gregs[REG_EFL]=0;
      CHECK("test [m],imm32 -> ZF clear", run(c,7,&uc,0x40) && !(uc.uc_mcontext.gregs[REG_EFL]&0x40)); }
    { const uint8_t c[]={0xF7,0x40,0x40,0x01,0x00,0x00,0x80};
      CHECK("test [m],imm32 -> ZF set", run(c,7,&uc,0x40) && (uc.uc_mcontext.gregs[REG_EFL]&0x40)); }
    { const uint8_t c[]={0x0F,0xB6,0x48,0x41};       /* movzx ecx, byte [rax+0x41] */
      CHECK("movzx ecx,byte [m]", run(c,4,&uc,0x41) && (uint64_t)uc.uc_mcontext.gregs[REG_RCX]==0x56); }
    { const uint8_t c[]={0x8A,0x60,0x40};            /* mov ah,[rax+0x40] (no REX -> AH) */
      uc.uc_mcontext.gregs[REG_RAX]=0x1111;
      CHECK("mov ah,[m] (byte reg 4 = AH)", run(c,3,&uc,0x40) && (uint64_t)uc.uc_mcontext.gregs[REG_RAX]==0x7811); }
    { const uint8_t c[]={0x81,0x48,0x08,0x00,0x01,0x00,0x00}; /* or dword [rax+8],0x100 */
      regs[2]=1;
      CHECK("or [m],imm32 read-modify-write", run(c,7,&uc,8) && regs[2]==0x101); }
    { const uint8_t c[]={0x0F,0xA2};                 /* cpuid: must be declined */
      CHECK("unknown opcode declined", !run(c,2,&uc,0)); }
    printf("%s (%d failures)\n", fails?"FAILED":"PASSED", fails); return fails!=0;
}
