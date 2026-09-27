#ifndef KSN_PROCEDURAL_H
#define KSN_PROCEDURAL_H
#include <stdbool.h>
#include <stdint.h>

/* Host-first experiment. This is separate from the v1 UI command quota and is
 * not yet a JS or Kasane core API. The VM owns a validated program copy during
 * preparation; a completed frame owns its ordered drawing operations. */
#define KSN_PROC_REGS 8
#define KSN_PROC_INPUTS 4
#define KSN_PROC_CODE 64
#define KSN_PROC_LOOP_DEPTH 4
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
    KSN_PROC_PLOT_COLOR_REG, KSN_PROC_LINE_COLOR_REG
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
ksn_proc_status ksn_proc_step(ksn_proc_vm *vm);
ksn_proc_status ksn_proc_run(ksn_proc_vm *vm);
/* Replays only the requested rows over an existing RGB565 backdrop. */
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height);
#endif
