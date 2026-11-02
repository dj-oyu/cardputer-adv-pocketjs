#pragma once
/* Shiki JIT, stage 0: can this firmware write machine code into RAM and run
 * it? (docs/kasane/shiki-jit.md sec.2). One line for the cycle bench
 * (pocket_shiki.c), device only; elsewhere it says so. */
#include <stdbool.h>
#include <stddef.h>

bool ksn_shiki_jit_probe(char *out,size_t n);
