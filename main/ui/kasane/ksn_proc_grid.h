#ifndef KSN_PROC_GRID_H
#define KSN_PROC_GRID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Typed memory IR, independent of the float procedural VM. Apps can register
 * it through pocket.kasane.grid. The outer rectangular
 * domain visits y, then x; the inner domain visits tap_y, then tap_x.
 * Prepare copies the program; keep the plan immutable while an execution
 * refers to it. Begin freezes parameters, pointers, and counts, not contents.
 * A valid bind proves address ranges; generic arithmetic may still report
 * overflow during scalar execution, after earlier outputs were written. */
#define KSN_GRID_CODE 16
#define KSN_GRID_REGS 8
#define KSN_GRID_PARAMS 8
#define KSN_GRID_BUFFERS 8
#define KSN_GRID_NO_PARAM 255u
#define KSN_GRID_VALUE_NONE 255u
#define KSN_GRID_VALUE_ACC 254u

typedef enum {
    KSN_GRID_CONST = 1,
    KSN_GRID_LOAD,
    KSN_GRID_ADD,
    KSN_GRID_MUL,
    KSN_GRID_MIN
} ksn_grid_opcode;

/* Buffer 1 receives the final store and may be read by a scalar recurrence.
 * Other IDs may hold independent inputs. */
typedef enum { KSN_GRID_SOURCE = 0, KSN_GRID_DEST = 1 } ksn_grid_buffer;

/* An affine coefficient is constant + scale * frozen_param. */
typedef struct {
    int32_t constant;
    int16_t scale;
    uint8_t param;
} ksn_grid_coeff;

/* offset + x*x + y*y + tap_x*tap_x + tap_y*tap_y. */
typedef struct { ksn_grid_coeff term[5]; } ksn_grid_index;

typedef struct {
    uint8_t op, dst, a, b, buffer;
    int16_t immediate;
    ksn_grid_index index;
} ksn_grid_instruction;

typedef struct {
    uint8_t count, result_reg;
    uint8_t final_shift; /* floor divide by 2^shift, then saturate to int16 */
    int64_t initial; /* evaluated once for each outer output */
    ksn_grid_instruction body[KSN_GRID_CODE];
    ksn_grid_index output;
} ksn_grid_program;

typedef struct {
    uint16_t width, height, tap_width, tap_height;
} ksn_grid_shape;

typedef struct {
    int16_t *data[KSN_GRID_BUFFERS];
    size_t count[KSN_GRID_BUFFERS];
    int32_t param[KSN_GRID_PARAMS];
} ksn_grid_binding;

typedef enum {
    KSN_GRID_OK = 0,
    KSN_GRID_BAD_IR,
    KSN_GRID_BAD_SHAPE,
    KSN_GRID_BAD_INDEX,
    KSN_GRID_BAD_BUFFER,
    KSN_GRID_ARITH_OVERFLOW
} ksn_grid_status;

/* Registration-time normal form for one independent signed 16-bit term.
 * A load retains its instruction slot so begin's resolved affine index is
 * reused without reparsing IR at every pixel. */
typedef struct {
    bool is_load;
    uint8_t instruction;
    int16_t constant;
} ksn_grid_mac_operand;

typedef struct {
    bool valid;
    uint8_t terms; /* one or two products in original expression order */
    ksn_grid_mac_operand left, right;
    ksn_grid_mac_operand extra_left, extra_right;
} ksn_grid_mac;

/* Register writes become immutable instruction values at registration. The
 * ACC sentinel is the incoming reduction value. Contributors include the
 * instruction itself and all transitive inputs, so a lowering can prove that
 * it preserves every checked operation. Live masks describe register values
 * needed on either side of each instruction, including checked dead results. */
typedef struct {
    uint8_t a, b, live_in, live_out, effects;
    uint16_t contributors;
    uint64_t magnitude_bound; /* valid only when range_proven is true */
    bool range_proven;
} ksn_grid_value;

enum {
    KSN_GRID_EFFECT_LOAD = 1u,
    KSN_GRID_EFFECT_DEST_READ = 2u,
    KSN_GRID_EFFECT_CHECKED = 4u,
    KSN_GRID_EFFECT_ACC_WRITE = 8u
};

typedef struct {
    ksn_grid_value value[KSN_GRID_CODE];
    uint16_t checked_mask;
} ksn_grid_analysis;

/* Bind-time classification of the output-lane x stride. Alignment and extra
 * cells consumed by a vector load still need checks at each block. */
typedef enum {
    KSN_GRID_ACCESS_GATHER = 0,
    KSN_GRID_ACCESS_CONTIGUOUS,
    KSN_GRID_ACCESS_INTERLEAVED2,
    KSN_GRID_ACCESS_BROADCAST,
    KSN_GRID_ACCESS_REVERSE
} ksn_grid_access_kind;

/* Lowering choices are local to one bound execution. AUTO is a request, never
 * a runnable kernel. Forced choices are used by the same-binary tuner. */
typedef enum {
    KSN_GRID_PIE_LOAD_AUTO,
    KSN_GRID_PIE_LOAD_GATHER,
    KSN_GRID_PIE_LOAD_AFFINE,
    KSN_GRID_PIE_LOAD_FUSED
} ksn_grid_pie_load_strategy;

typedef enum {
    KSN_GRID_SELECTION_NONE,
    KSN_GRID_SELECTION_FALLBACK,
    KSN_GRID_SELECTION_PROFILE,
    KSN_GRID_SELECTION_FORCED
} ksn_grid_selection_reason;

typedef struct {
    ksn_grid_program program; /* owned; caller may discard original */
    ksn_grid_analysis analysis; /* versioned values shared by lowerings */
    ksn_grid_mac mac; /* exact LOAD/CONST product or direct sum */
    bool prepared;
} ksn_grid_plan;

typedef struct {
    const ksn_grid_plan *plan;
    ksn_grid_shape shape;
    ksn_grid_binding binding;
    int64_t index[KSN_GRID_CODE][5], output[5];
    ksn_grid_access_kind access[KSN_GRID_CODE];
    uint32_t validation_work;
    bool safe, independent, reduction_shape, qacc_legal, pie_candidate;
    /* A proved left-to-right sum or affine IIR with independent rows. */
    bool scan_rows_candidate;
    uint8_t scan_source_slot;
    int16_t scan_prev_coefficient;
    bool pie_backend_selected; /* set only after a matching PIE run succeeds */
    ksn_grid_pie_load_strategy requested_strategy, selected_strategy;
    ksn_grid_pie_load_strategy selected_request_snapshot;
    ksn_grid_selection_reason selection_reason;
    uint8_t candidate_mask; /* bits indexed by ksn_grid_pie_load_strategy */
    bool selection_valid;
    bool profile_environment_snapshot;
    uint64_t profile_key;
} ksn_grid_execution;

ksn_grid_status ksn_grid_prepare(const ksn_grid_program *program,
                                 ksn_grid_plan *plan);
ksn_grid_status ksn_grid_begin(const ksn_grid_plan *plan,
                               const ksn_grid_shape *shape,
                               const ksn_grid_binding *binding,
                               ksn_grid_execution *execution);
ksn_grid_status ksn_grid_run_scalar(const ksn_grid_execution *execution);
/* Eight independent output lanes, interleaved by tap. A host model, not PIE. */
ksn_grid_status ksn_grid_run_lanes_model(const ksn_grid_execution *execution);

#endif
