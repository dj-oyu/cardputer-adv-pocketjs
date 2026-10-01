#ifndef KSN_PROC_ANALYSIS_H
#define KSN_PROC_ANALYSIS_H
#include "ksn_procedural.h"

/* A bounded, diagnostic-only analysis. Register masks are ksn_pa_regs, bit r
 * for register r; the type must hold KSN_PROC_REGS bits (asserted below).
 * No result grants permission to bypass VM validation or resource limits.
 * CFG edges overapproximate execution (even a fixed nonzero REPEAT has a
 * skip edge); liveness describes normal completion, not failure snapshots.
 * relocatable means an independent adjacent pure instruction exists in the
 * same block, not that arbitrary motion is safe. loop_independent describes
 * dependence/failure barriers only, not permission to delete iterations or
 * their final register writes. Neither flag preserves per-step debugger
 * snapshots: any future optimized path must retain the original debug path
 * and logical step accounting. Inputs and initial state must pass VM begin. */
typedef enum {
    KSN_PA_NONE = 0,
    KSN_PA_INVALID = 1u << 0,
    KSN_PA_CONTROL = 1u << 1,
    KSN_PA_REG_DEP = 1u << 2,
    KSN_PA_PEN = 1u << 3,
    KSN_PA_DRAW_ORDER = 1u << 4,
    KSN_PA_MAY_FAIL = 1u << 5,
    KSN_PA_STEP_LIMIT = 1u << 6,
    KSN_PA_LOOP_CARRIED = 1u << 7,
    KSN_PA_OBSERVABLE_REG = 1u << 8
} ksn_pa_reason;

typedef enum {
    KSN_PA_EFFECT_NONE = 0,
    KSN_PA_EFFECT_REG = 1u << 0,
    KSN_PA_EFFECT_PEN = 1u << 1,
    KSN_PA_EFFECT_DRAW = 1u << 2,
    KSN_PA_EFFECT_CONTROL = 1u << 3
} ksn_pa_effect;

typedef enum {
    KSN_PA_FAIL_NONE = 0,
    KSN_PA_FAIL_FINITE = 1u << 0,
    KSN_PA_FAIL_COUNT = 1u << 1,
    KSN_PA_FAIL_COORD = 1u << 2,
    KSN_PA_FAIL_COLOR = 1u << 3,
    KSN_PA_FAIL_SEGMENTS = 1u << 4,
    KSN_PA_FAIL_RASTER = 1u << 5,
    KSN_PA_FAIL_STEPS = 1u << 6
} ksn_pa_failure;

/* Widened from uint8_t when KSN_PROC_REGS went 8 -> 16: an 8-bit mask would
 * silently drop r8..r15 from every read/write/live set. */
typedef uint16_t ksn_pa_regs;
_Static_assert(KSN_PROC_REGS <= 16, "register masks are 16-bit");

typedef struct {
    ksn_pa_regs reads, writes;
    uint8_t successor[2], successor_count;
    uint8_t block, loop_depth;
    uint8_t effects;
    uint8_t failure;
    uint16_t relocation_reasons, loop_reasons;
    bool relocatable, loop_independent;
} ksn_pa_inst;

typedef struct {
    ksn_pa_regs use, def, live_in, live_out;
    uint8_t first, last, successor[2], successor_count;
} ksn_pa_block;

typedef struct {
    ksn_pa_regs reads, writes, use_before_def, carried;
    uint16_t reasons;
    uint8_t begin, end;
    bool independent;
} ksn_pa_loop;

typedef struct {
    bool valid;
    uint8_t count, block_count, loop_count;
    /* 0 means no static bound is proved within KSN_PROC_STEPS. */
    uint16_t max_steps;
    uint16_t program_reasons;
    ksn_pa_inst inst[KSN_PROC_CODE];
    ksn_pa_block block[KSN_PROC_CODE];
    ksn_pa_loop loop[KSN_PROC_CODE];
} ksn_proc_analysis;

/* Returns false on invalid IR. The output is still initialized and carries
 * KSN_PA_INVALID. Analysis copies no program pointer and executes no code. */
bool ksn_proc_analyze(const ksn_proc_program *program, ksn_proc_analysis *out);

#endif
