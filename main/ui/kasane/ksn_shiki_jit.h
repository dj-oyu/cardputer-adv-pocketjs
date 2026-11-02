#pragma once
/* Shiki JIT, stage 0: can this firmware write machine code into RAM and run
 * it? (docs/kasane/shiki-jit.md sec.2). One line for the cycle bench
 * (pocket_shiki.c), device only; elsewhere it says so. */
#include <stdbool.h>
#include <stddef.h>

bool ksn_shiki_jit_probe(char *out,size_t n);

/* The encoder (shiki-jit.md sec.4): writes forms of ksn_shiki_jit_enc.h into
 * a byte buffer. Portable C, so the host can check it against the assembler
 * (tools/shiki_jit/test_encoder.py). */
#include <stdint.h>
typedef struct { uint8_t *p; size_t len,cap; bool overflow; } ksn_jit_buf;
/* One instruction: form is a KSN_JIT_* index, o0..o2 its operands as written
 * (q and a register numbers, immediates divided by their scale: an offset
 * of 16 in ee.vld.128.ip is 1). Out of room sets overflow and writes nothing. */
void ksn_jit_emit(ksn_jit_buf *b,unsigned form,int o0,int o1,int o2);

/* Stage 2 (shiki-jit.md sec.3): a VEACH section compiled to PIE code. */
#include "ksn_shiki.h"
#define KSN_JIT_SECTIONS 4
typedef struct {
    uint8_t pc,steps;                  /* the VEACH; steps per block (body + END) */
    uint8_t nptr,nshift,nconst;
    bool acc,q7;                       /* uses QACC; q7 holds the constants */
    uint8_t ptr_region[10],ptr_field[10];
    uint8_t shift[4];
    uint8_t const_reg[8];              /* konst[c] = sat16(r[const_reg[c]]) */
    uint8_t nimg,img_reg[2];           /* VACCSET: QACC images of r[img_reg[k]] */
    uint16_t off,len;                  /* the code, in the plan's buffer */
} ksn_jit_section;
bool ksn_shiki_jit_section(const ksn_shiki_plan *plan,unsigned pc,ksn_jit_section *s,ksn_jit_buf *b);
unsigned ksn_shiki_jit_plan(const ksn_shiki_plan *plan,ksn_jit_section *s,unsigned max,ksn_jit_buf *b);
/* Run time, device only (ksn_shiki.c calls these under KSN_SHIKI_HAS_PIE).
 * attach compiles the plan's sections into executable memory after
 * registration (none: plan->jit stays NULL); detach frees them. veach runs
 * the full blocks of the VEACH at pc and returns how many it ran (0: the
 * interpreter runs the section as before); it adds their steps to vm. */
extern bool ksn_shiki_jit_enabled;
void ksn_shiki_jit_attach(ksn_shiki_plan *plan);
void ksn_shiki_jit_detach(ksn_shiki_plan *plan);
/* ksn_shiki.c's PIE path: the QACC image (52 B, 16-aligned) that sets
 * every lane to r, as VACCSET leaves it. */
void ksn_shiki_pie_accset_image(void *img,int32_t r);
uint32_t ksn_shiki_jit_veach(const ksn_shiki_plan *plan,unsigned pc,uint32_t start,uint32_t count,
                             ksn_shiki_vm *vm,void *vregs,void *acc);
