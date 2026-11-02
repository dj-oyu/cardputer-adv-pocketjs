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
