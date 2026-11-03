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
/* p NULL: a sizing pass (len counts, nothing is written). word_only: p is
 * memory that takes 32-bit accesses only (IRAM without a data alias, p
 * 4-aligned); bytes go in by read-modify-write of their word. */
typedef struct { uint8_t *p; size_t len,cap; bool overflow,word_only; } ksn_jit_buf;
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

/* Stage 3 (shiki-jit.md sec.11): the whole plan as scalar code. The code
 * keeps the vm in memory (registers, loop frames, depth, steps), inlines
 * the arithmetic and the loop control and calls ksn_shiki_jit_leaf for
 * every other instruction, so it can stop at any block and leave the vm as
 * the interpreter would have it. What the code reads (32-bit fields: the
 * device's pointers; the host test maps them): */
typedef struct {
    ksn_shiki_vm *vm;                       /* 0 */
    const int32_t *in;                      /* 4: the draw's inputs */
    int32_t limit;                          /* 8: KSN_SHIKI_STEPS */
    uint32_t (*leaf)(const void *,uint32_t);/* 12: ksn_shiki_jit_leaf */
    uint32_t (*draw)(const void *,int32_t,int32_t,int32_t,uint32_t); /* 16: ksn_shiki_jit_draw */
    const int32_t *sin;                     /* 20: ksn_shiki_sin_table */
    uint32_t (*curve)(const void *,int32_t,int32_t,uint32_t); /* 24: ksn_shiki_jit_curve */
    const int32_t *weights;                 /* the plan's CUBIC weights (C only), or NULL */
    const ksn_shiki_plan *plan;
    ksn_proc_segment *out;
    unsigned index;
} ksn_jit_run;
/* One instruction the code does not inline (ksn_shiki.c): 0 go on, 1 skip
 * the loop body (a loop run zero times: control goes to its END + 1), else
 * the status << 8. The step is already counted. */
uint32_t ksn_shiki_jit_leaf(const ksn_jit_run *c,uint32_t pc);
/* The drawing instructions, called by the code directly (sec.12) with the
 * registers already read: x and y raw (Q16), the colour (aux, or r[dst] for
 * the _C forms) and the kind. Same results as the interpreter's case: 0 to
 * go on, else the status << 8. In IRAM on the device. */
enum { KSN_JIT_DRAW_MOVE, KSN_JIT_DRAW_LINE, KSN_JIT_DRAW_PLOT, KSN_JIT_DRAW_LINE_C, KSN_JIT_DRAW_PLOT_C };
uint32_t ksn_shiki_jit_draw(const ksn_jit_run *c,int32_t x,int32_t y,int32_t colour,uint32_t kind);
/* CUBIC and LINE_PATTERN (r[a], r[b] raw for LINE_PATTERN's end), called
 * directly (sec.12.2); 0 or the status << 8. */
uint32_t ksn_shiki_jit_curve(const ksn_jit_run *c,int32_t x,int32_t y,uint32_t pc);
/* Calls and cycles of CUBIC [0] and LINE_PATTERN [1] since boot, whichever
 * path ran them (the session's SHIKI_TURN line prints the difference). */
extern uint32_t ksn_shiki_curve_n[2],ksn_shiki_curve_cy[2];
/* Every CUBIC's weights in pc order, n rows of 4 each (sec.13): the int32s
 * written, or 0 when they would pass max. */
size_t ksn_shiki_cubic_weights(const ksn_shiki_plan *plan,int32_t *out,size_t max);
/* Compiles plan into b: a literal pool, then the function at *entry
 * (uint32_t f(ksn_jit_run *): the status << 8 when the draw ended, else the
 * pc where the interpreter goes on). False when b is too small. */
bool ksn_shiki_jit_scalar(const ksn_shiki_plan *plan,ksn_jit_buf *b,size_t *entry);
/* Run time (device): the plan's code, compiled once it is hot and if the
 * budget holds it; 0 (start at pc 0) when there is none. */
extern bool ksn_shiki_jit_scalar_on;
extern uint32_t ksn_shiki_jit_budget,ksn_shiki_jit_bytes;
/* Compiles that did not happen since boot: refused by the compiler, the
 * budget full, the heap full (the last two are retried). Device only. */
extern uint32_t ksn_shiki_jit_fail[3];
extern uint32_t ksn_shiki_jit_evicted;
extern uint32_t ksn_shiki_jit_cold;
extern uint32_t ksn_shiki_jit_compiles,ksn_shiki_jit_compile_cy;
/* The bench's: compile now, whatever the heat (the budget still holds). */
bool ksn_shiki_jit_compile_now(ksn_shiki_plan *plan);
uint32_t ksn_shiki_jit_run(const ksn_shiki_plan *plan,const int32_t *in,unsigned index,
                           ksn_shiki_vm *vm,ksn_proc_segment *out);
/* The interpreter from pc on, the vm as the code left it (in: the draw's
 * KSN_SHIKI_INPUTS inputs). ksn_shiki_draw_index's own loop. */
ksn_shiki_status ksn_shiki_resume(const ksn_shiki_plan *plan,const int32_t *in,unsigned index,
                                  ksn_shiki_vm *vm,ksn_proc_segment out[KSN_SHIKI_SEGMENTS],unsigned pc);
