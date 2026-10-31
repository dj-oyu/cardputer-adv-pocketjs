#ifndef KSN_SHIKI_H
#define KSN_SHIKI_H
/* Shiki v1 line programs: registration and the integer engine
 * (docs/kasane/shiki-spec.md). Stage 1 of the spec's migration (sec.11):
 * the 32-bit and .H instructions, the sec.3 byte format and every sec.6.1
 * check. CUBIC, LINE_PATTERN (integer procedures still open, sec.12) and the
 * data-region instructions (stage 4) are refused at registration for now,
 * as is a header that declares data regions.
 *
 * Nothing here allocates: the caller asks ksn_shiki_plan_bytes() and hands
 * over that much memory (the Shiki heap of sec.9.2 comes later). No float
 * appears except the double inputs and arguments, converted once by the
 * sec.2.4 rule. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksn_procedural.h"   /* ksn_proc_segment, the frame's entry */

#define KSN_SHIKI_CODE 64
#define KSN_SHIKI_REGS 16
#define KSN_SHIKI_INPUTS 8
#define KSN_SHIKI_ARGS 8
#define KSN_SHIKI_PATCHES 32
#define KSN_SHIKI_LOOP_DEPTH 8
#define KSN_SHIKI_STEPS 10000
#define KSN_SHIKI_SEGMENTS 1024
#define KSN_SHIKI_RASTER 8192

enum {
    KSN_SHIKI_SET, KSN_SHIKI_INPUT, KSN_SHIKI_ADD, KSN_SHIKI_MUL, KSN_SHIKI_SIN,
    KSN_SHIKI_REPEAT, KSN_SHIKI_END, KSN_SHIKI_MOVE, KSN_SHIKI_PLOT, KSN_SHIKI_LINE,
    KSN_SHIKI_REPEAT_REG, KSN_SHIKI_BREAK_IF_GT, KSN_SHIKI_PLOT_C, KSN_SHIKI_LINE_C,
    KSN_SHIKI_CUBIC, KSN_SHIKI_LINE_PATTERN,
    KSN_SHIKI_SUB, KSN_SHIKI_NEG, KSN_SHIKI_SHIFT,
    KSN_SHIKI_ADD_H, KSN_SHIKI_SUB_H, KSN_SHIKI_MUL_H, KSN_SHIKI_SHIFT_H,
    KSN_SHIKI_EACH, KSN_SHIKI_FETCH, KSN_SHIKI_PUT, KSN_SHIKI_LOOKUP,
    KSN_SHIKI_OPS
};

/* One instruction as the engine keeps it: the sec.4.2 fields, decoded from
 * little-endian bytes so the engine never reads the caller's buffer again. */
typedef struct {
    uint8_t op,dst,a,b;
    int32_t imm;
    uint16_t aux;
} ksn_shiki_inst;

/* A registered program: its own patched copy (sec.6.1: the caller's bytes
 * and arguments are not read after registration), the inputs' Q formats,
 * and each loop opener's matching END, found once here instead of per draw. */
typedef struct {
    uint8_t count,inputs;
    uint8_t input_q[KSN_SHIKI_INPUTS];
    uint8_t loop_end[KSN_SHIKI_CODE];
    ksn_shiki_inst code[KSN_SHIKI_CODE];
} ksn_shiki_plan;

typedef enum { KSN_SHIKI_RUNNING, KSN_SHIKI_DONE, KSN_SHIKI_INVALID,
               KSN_SHIKI_LIMIT, KSN_SHIKI_BADINPUT } ksn_shiki_status;

typedef struct {
    int32_t reg[KSN_SHIKI_REGS];
    struct { uint8_t pc; uint16_t remaining; } loop[KSN_SHIKI_LOOP_DEPTH];
    uint8_t depth;
    uint16_t steps,raster;
    int16_t pen_x,pen_y;
    bool pen_valid;
    uint16_t count;                         /* segments emitted by this draw */
} ksn_shiki_vm;

/* 257 entries, sec.4.4: round(sin(k pi / 512) * 65536). */
extern const int32_t ksn_shiki_sin_table[257];

/* sec.2.4: a double to Q f, false for a non-finite v. */
bool ksn_shiki_from_double(double v,unsigned f,int32_t *out);

/* Registers code (len bytes, sec.3) with nargs arguments into *plan. False
 * on any sec.3/sec.6.1 failure, a wrong argument count or a non-finite
 * argument; *plan is then unspecified. */
bool ksn_shiki_register(const uint8_t *code,size_t len,const double *args,unsigned nargs,
                        ksn_shiki_plan *plan);

/* Runs one draw: up to KSN_SHIKI_INPUTS inputs (n of them; missing ones are
 * 0, extra ones ignored, sec.4.8.7). Segments go to out[0..vm->count). The
 * vm is fully reset here; its state after the call is sec.4.8.8's. */
ksn_shiki_status ksn_shiki_draw(const ksn_shiki_plan *plan,const double *inputs,unsigned n,
                                ksn_shiki_vm *vm,ksn_proc_segment out[KSN_SHIKI_SEGMENTS]);
/* The result line of the shared text contract (tools/kasane_ir/shiki_ref.mjs
 * format(), tools/kasane_contract/shiki_run.c), handed to put in pieces so a
 * caller can print it or hash it without holding a line that 1,024 segments
 * make some 30 KB long. registered false gives "REJECT". No trailing newline. */
typedef void (*ksn_shiki_put)(void *ctx,const char *text,size_t len);
void ksn_shiki_result(bool registered,ksn_shiki_status status,const ksn_shiki_vm *vm,
                      const ksn_proc_segment *seg,ksn_shiki_put put,void *ctx);
#endif
