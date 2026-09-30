#ifndef KSN_PROCEDURAL_H
#define KSN_PROCEDURAL_H
#include <stdbool.h>
#include <stdint.h>

/* Host-first experiment. This is separate from the v1 UI command quota and is
 * not yet a JS or Kasane core API. The VM owns a validated program copy during
 * preparation; a completed frame owns its ordered drawing operations. */
/* Registers, inputs and loop depth were widened from 8/4/4 without changing
 * the instruction encoding: register and input indices are uint8_t fields.
 * Register sets in ksn_proc_analysis.h are 16-bit masks, so 16 is a ceiling
 * there; it is checked with a static assertion, not by convention. */
#define KSN_PROC_REGS 16
#define KSN_PROC_INPUTS 8
#define KSN_PROC_CODE 64
#define KSN_PROC_LOOP_DEPTH 8
#define KSN_PROC_SEGMENTS 1024
#define KSN_PROC_STEPS 10000
#define KSN_PROC_RASTER_STEPS 8192
#define KSN_PROC_W 240
#define KSN_PROC_H 135

typedef enum {
    KSN_PROC_SET, KSN_PROC_INPUT, KSN_PROC_ADD, KSN_PROC_MUL,
    KSN_PROC_SIN, KSN_PROC_REPEAT, KSN_PROC_END,
    KSN_PROC_MOVE, KSN_PROC_PLOT, KSN_PROC_LINE,
    KSN_PROC_REPEAT_REG, KSN_PROC_BREAK_IF_GT,
    KSN_PROC_PLOT_COLOR_REG, KSN_PROC_LINE_COLOR_REG,
    KSN_PROC_CUBIC
} ksn_proc_op;
typedef struct {
    uint8_t op,dst,a,b;
    float value;             /* SET value; REPEAT count is in a */
    uint16_t color;          /* RGB565 for PLOT and LINE; color-reg ops use dst */
} ksn_proc_inst;
typedef struct { const ksn_proc_inst *code; uint8_t count; } ksn_proc_program;
typedef struct { int16_t x0,y0,x1,y1; uint16_t color; } ksn_proc_segment;
typedef struct {
    uint16_t count;
    uint16_t raster_steps;
    bool ready;
    ksn_proc_segment segments[KSN_PROC_SEGMENTS];
} ksn_proc_frame;
typedef enum { KSN_PROC_RUNNING, KSN_PROC_DONE, KSN_PROC_INVALID,
               KSN_PROC_LIMIT } ksn_proc_status;
/* Explicit native simulation state. Copy into a VM at begin and copy out only
 * after DONE. Frames remain immutable while this state advances. */
typedef struct {
    float reg[KSN_PROC_REGS];
    int16_t pen_x,pen_y;
    bool pen_valid;
} ksn_proc_state;
typedef struct {
    const ksn_proc_program *program;
    ksn_proc_program owned_program;
    ksn_proc_inst owned_code[KSN_PROC_CODE];
    ksn_proc_frame *frame;
    float input[KSN_PROC_INPUTS],reg[KSN_PROC_REGS];
    uint8_t pc,last_pc,depth;
    uint8_t loop_end[KSN_PROC_CODE];
    struct { uint8_t pc; uint16_t remaining; } loop[KSN_PROC_LOOP_DEPTH];
    uint32_t steps;
    int16_t pen_x,pen_y;
    bool pen_valid;
    ksn_proc_status status;
} ksn_proc_vm;

ksn_proc_status ksn_proc_begin(ksn_proc_vm *vm,const ksn_proc_program *program,
                               const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame);

/* Registration arguments of a const (flash) program. A patch names one field
 * of one instruction that holds argument `param` instead of the constant in
 * the program; the program keeps a placeholder there. Patches are sorted by
 * pc. docs/kasane/flash-plan.md. */
typedef enum { KSN_PROC_FIELD_A, KSN_PROC_FIELD_VALUE, KSN_PROC_FIELD_COLOR } ksn_proc_field;
typedef struct { uint8_t pc,field,param; } ksn_proc_patch;
typedef struct {
    const ksn_proc_patch *patch;
    const float *arg;
    uint8_t patches,args;
} ksn_proc_binding;
/* Writes the binding's arguments into code[0..count). False (code partly
 * written) for a patch outside count or args, an unknown field, a field
 * value the instruction cannot hold (A: integer 0..255, COLOR: integer
 * 0..65535, VALUE: finite), or patches out of pc order. */
bool ksn_proc_apply_binding(ksn_proc_inst *code,uint8_t count,const ksn_proc_binding *binding);
/* ksn_proc_begin for a const program with arguments: the VM copies the
 * program, applies the binding to its own copy, then validates, so what runs
 * is exactly what was validated, as in ksn_proc_begin. */
ksn_proc_status ksn_proc_begin_bound(ksn_proc_vm *vm,const ksn_proc_program *program,
                                     const ksn_proc_binding *binding,
                                     const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame);
void ksn_proc_state_reset(ksn_proc_state *state);
ksn_proc_status ksn_proc_begin_state(ksn_proc_vm *vm,const ksn_proc_program *program,
                                    const float input[KSN_PROC_INPUTS],
                                    const ksn_proc_state *state,ksn_proc_frame *frame);
/* Commit after a completed preparation. On failure, state is untouched. */
bool ksn_proc_capture_state(const ksn_proc_vm *vm,ksn_proc_state *state);
/* One instruction, including a loop boundary, per call. Inspect pc, last_pc,
 * registers, loop stack and frame count after each call for debugger stepping. */
/* REPEAT_REG reads its count from register a: an integer in 0..255; zero skips
 * the body. BREAK_IF_GT compares registers a and b and exits the innermost
 * loop when a>b. PLOT_COLOR_REG and LINE_COLOR_REG use register dst as an
 * integral RGB565 color (0..65535). Every path remains subject to the step,
 * segment and raster caps. */
/* CUBIC reads four (x,y) control points from registers 0..7 (dst=0; r8..r15
 * are never read, however many registers exist), samples
 * a segments (1..64) in a native loop, emits ordered lines of color, and
 * leaves the pen at the final control point. One debugger step covers the
 * entire bounded curve; the emitted segment/raster limits still apply. */
ksn_proc_status ksn_proc_step(ksn_proc_vm *vm);
#ifdef KASANE_PROC_LIMITS_PROBE
/* Diagnostic build only: sweep registers 0..n-1 after every step, as the
 * code before c53f661 did (8 then, 16 if it had been kept). 0 is the shipped
 * behaviour. Exists so both variants are timed in one binary. */
extern uint8_t g_ksn_proc_sweep_regs;
#endif
ksn_proc_status ksn_proc_run(ksn_proc_vm *vm);
/* Replays only the requested rows over an existing RGB565 backdrop. */
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height);
#endif
