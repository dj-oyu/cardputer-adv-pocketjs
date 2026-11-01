#ifndef KSN_SHIKI_H
#define KSN_SHIKI_H
/* Shiki v1.4 line programs: registration and the integer engine
 * (docs/kasane/shiki-spec.md). The 32-bit and .H instructions, CUBIC and
 * LINE_PATTERN (v1.2's integer procedures, sec.4.6/4.7: run-time divisions
 * are int32 by int32 only, sec.2.2), the data-region instructions EACH,
 * FETCH, PUT and LOOKUP (v1.3, sec.5), the sec.3 byte format and every
 * sec.6.1 check.
 *
 * Nothing here allocates: the caller owns the plan, the vm and the regions'
 * memory (the Shiki heap of sec.9.2 is the JS API's business). No float
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
#define KSN_SHIKI_REGIONS 4
#define KSN_SHIKI_RECORDS 65535

enum {
    KSN_SHIKI_SET, KSN_SHIKI_INPUT, KSN_SHIKI_ADD, KSN_SHIKI_MUL, KSN_SHIKI_SIN,
    KSN_SHIKI_REPEAT, KSN_SHIKI_END, KSN_SHIKI_MOVE, KSN_SHIKI_PLOT, KSN_SHIKI_LINE,
    KSN_SHIKI_REPEAT_REG, KSN_SHIKI_BREAK_IF_GT, KSN_SHIKI_PLOT_C, KSN_SHIKI_LINE_C,
    KSN_SHIKI_CUBIC, KSN_SHIKI_LINE_PATTERN,
    KSN_SHIKI_SUB, KSN_SHIKI_NEG, KSN_SHIKI_SHIFT,
    KSN_SHIKI_ADD_H, KSN_SHIKI_SUB_H, KSN_SHIKI_MUL_H, KSN_SHIKI_SHIFT_H,
    KSN_SHIKI_EACH, KSN_SHIKI_FETCH, KSN_SHIKI_PUT, KSN_SHIKI_LOOKUP,
    /* PROTOTYPE (docs/kasane/shiki-pie.md, not in the spec yet): 8-lane
     * vector instructions with PIE's semantics, interpreted once per block
     * of 8 records, and batch emitters that draw from a region natively. */
    KSN_SHIKI_VEACH, KSN_SHIKI_VLOAD, KSN_SHIKI_VSTORE, KSN_SHIKI_VSPLAT, KSN_SHIKI_VIOTA,
    KSN_SHIKI_VADD, KSN_SHIKI_VSUB, KSN_SHIKI_VMIN, KSN_SHIKI_VMAX,
    KSN_SHIKI_VZACC, KSN_SHIKI_VMAC, KSN_SHIKI_VMACK, KSN_SHIKI_VACCSET, KSN_SHIKI_VOUT,
    KSN_SHIKI_EMIT_POINTS, KSN_SHIKI_EMIT_LINES,
    KSN_SHIKI_OPS
};

/* Element types of a data region (sec.3's region table). */
enum { KSN_SHIKI_I16, KSN_SHIKI_U16, KSN_SHIKI_I32 };

/* A data region as the caller hands it to registration (sec.5.1, v1.4):
 * records records of k_rec fields of type, laid out as field planes: field
 * c of record j is element c * S + j, S = ksn_shiki_stride(records), so the
 * memory holds k_rec * S elements (ksn_shiki_region_elements) and each plane
 * ends in S - records padding elements whose values never reach a result and
 * are unchanged by a draw (sec.5.1: an implementation may read them and write
 * the same values back, as a masked PIE tail would; this one never does). The binding
 * is fixed at registration; the caller keeps the memory alive and unmoved
 * while the plan exists. Two entries may point at the same memory (sec.5.1). */
typedef struct {
    void *data;
    uint32_t records;
    uint8_t type,q,k_rec;
} ksn_shiki_region;
#define ksn_shiki_stride(records) (((uint32_t)(records)+7u)&~7u)
#define ksn_shiki_region_elements(records,k_rec) ((size_t)(k_rec)*ksn_shiki_stride(records))

/* One instruction as the engine keeps it: the sec.4.2 fields, decoded from
 * little-endian bytes so the engine never reads the caller's buffer again,
 * and what registration works out once in the struct's two spare bytes: a
 * loop opener's matching END (end) and, for FETCH/PUT, the loop-stack level
 * of the EACH they read (sec.5.4.3: the lexically innermost EACH of their
 * region, which is also its run-time level). 12 B either way. */
typedef struct {
    uint8_t op,dst,a,b;
    int32_t imm;
    uint16_t aux;
    uint8_t end,level;
} ksn_shiki_inst;
_Static_assert(sizeof(ksn_shiki_inst)==12,"the spare bytes hold end and level");

/* A registered program: its own patched copy (sec.6.1: the caller's bytes
 * and arguments are not read after registration), the inputs' Q formats and
 * the bound regions. Sized by its instruction count (ksn_shiki_plan_bytes):
 * a Shiki heap of 48 KiB holds BIG WAVE's 22 programs only if a short
 * program costs what it holds (docs/kasane/shiki.md sec.4.5.1). */
typedef struct {
    void *data;
    uint32_t stride;              /* the plane length S (sec.5.1): up to 65,536 */
    uint16_t records;
    uint8_t type,k_rec;
} ksn_shiki_bound;
typedef struct {
    uint8_t count,inputs;
    uint8_t input_q[KSN_SHIKI_INPUTS];
    ksn_shiki_bound region[KSN_SHIKI_REGIONS];
    ksn_shiki_inst code[];
} ksn_shiki_plan;
/* The bytes a plan of count instructions takes; KSN_SHIKI_PLAN_MAX holds any. */
#define ksn_shiki_plan_bytes(count) (sizeof(ksn_shiki_plan)+(size_t)(count)*sizeof(ksn_shiki_inst))
#define KSN_SHIKI_PLAN_MAX ksn_shiki_plan_bytes(KSN_SHIKI_CODE)
/* The bytes code (len bytes, sec.3) needs as a plan, from its header alone,
 * or 0 when the header is not a Shiki header (registration then fails
 * anyway): what to allocate before ksn_shiki_register(). */
size_t ksn_shiki_plan_bytes_for(const uint8_t *code,size_t len);

typedef enum { KSN_SHIKI_RUNNING, KSN_SHIKI_DONE, KSN_SHIKI_INVALID,
               KSN_SHIKI_LIMIT, KSN_SHIKI_BADINPUT } ksn_shiki_status;

typedef struct {
    int32_t reg[KSN_SHIKI_REGS];
    /* total is set by EACH only: its cursor is total - remaining, so END
     * stays the same instruction for every loop. */
    struct { uint8_t pc; uint16_t remaining,total,n; } loop[KSN_SHIKI_LOOP_DEPTH];
    uint8_t depth;
    uint16_t steps,raster;
    int16_t pen_x,pen_y;
    bool pen_valid;
    uint16_t count;                         /* segments emitted by this draw */
    bool ext;                               /* a LINE_PATTERN chord is among them */
    /* The prototype's vector state (shiki-pie.md sec.3.1), kept in memory:
     * v0..v7 (8 lanes of int16) then the accumulator (8 lanes of 40 bits,
     * sat40), both 16-B aligned inside raw for PIE's 128-bit accesses. The
     * accumulator is int64_t[8] for the C model and QACC's own image
     * (EE.ST.QACC_*) for PIE: pie, latched from ksn_shiki_vec_pie when the
     * draw starts, says which, so a draw never mixes them. */
    uint8_t vraw[8*16+64+15];
    bool pie;
} ksn_shiki_vm;
/* The C model's accumulator (meaningless when vm->pie). */
static inline int64_t *ksn_shiki_vm_acc(ksn_shiki_vm *vm){
    return (int64_t *)((((uintptr_t)vm->vraw+15u)&~(uintptr_t)15u)+128u);
}
/* Prototype switch: the vector instructions run on PIE (ESP32-S3 only;
 * elsewhere it is ignored). Off by default; the bench flips it to compare
 * the two in one binary. */
extern bool ksn_shiki_vec_pie;

/* 257 entries, sec.4.4: round(sin(k pi / 512) * 65536). */
extern const int32_t ksn_shiki_sin_table[257];
/* The SIN instruction (sec.4.4): r's low 16 bits are the fraction of a turn,
 * the result is Q16 (-65536..65536). Also the JS API's sin/cos/sinMany
 * (sec.9.4, v1.5), so a JS value and a program's agree bit for bit. */
int32_t ksn_shiki_sin(int32_t r);

/* sec.2.4: a double to Q f, false for a non-finite v. */
bool ksn_shiki_from_double(double v,unsigned f,int32_t *out);

/* Registers code (len bytes, sec.3) with nargs arguments and nregions data
 * regions into *plan, which must hold ksn_shiki_plan_bytes_for(code, len). False on any sec.3/sec.6.1 failure, a wrong argument
 * or region count, a region that does not match the program's region table
 * or is malformed (sec.5.1), or a non-finite argument; *plan is then
 * unspecified. */
bool ksn_shiki_register(const uint8_t *code,size_t len,const double *args,unsigned nargs,
                        const ksn_shiki_region *regions,unsigned nregions,ksn_shiki_plan *plan);

/* Runs one draw: up to KSN_SHIKI_INPUTS inputs (n of them; missing ones are
 * 0, extra ones ignored, sec.4.8.7). Segments go to out[0..vm->count), a
 * LINE_PATTERN chord as two entries in ksn_procedural.h's encoding. The
 * vm is fully reset here; its state after the call is sec.4.8.8's. */
ksn_shiki_status ksn_shiki_draw(const ksn_shiki_plan *plan,const double *inputs,unsigned n,
                                ksn_shiki_vm *vm,ksn_proc_segment out[KSN_SHIKI_SEGMENTS]);
/* The same draw from inputs already in each input's Q format (as sec.2.4
 * would make them): no conversion, so no BADINPUT. Missing inputs are 0,
 * extra ones ignored. ksn_shiki_draw converts and calls this. */
ksn_shiki_status ksn_shiki_draw_fixed(const ksn_shiki_plan *plan,const int32_t *inputs,unsigned n,
                                      ksn_shiki_vm *vm,ksn_proc_segment out[KSN_SHIKI_SEGMENTS]);
/* The result line of the shared text contract (tools/kasane_ir/shiki_ref.mjs
 * format(), tools/kasane_contract/shiki_run.c), handed to put in pieces so a
 * caller can print it or hash it without holding a line that 1,024 segments
 * make some 30 KB long. registered false gives "REJECT". No trailing newline. */
typedef void (*ksn_shiki_put)(void *ctx,const char *text,size_t len);
void ksn_shiki_result(bool registered,ksn_shiki_status status,const ksn_shiki_vm *vm,
                      const ksn_proc_segment *seg,ksn_shiki_put put,void *ctx);
/* The contract's region part, appended to a registered case's result line
 * when the case passed regions: " D <n>" and, per region, " <records> <e...>"
 * with the k_rec * records values field by field and the padding left out
 * (sec.10.1 v1.4; i16/i32 signed, u16 unsigned, decimal). */
void ksn_shiki_result_regions(const ksn_shiki_region *regions,unsigned n,ksn_shiki_put put,void *ctx);
#endif
