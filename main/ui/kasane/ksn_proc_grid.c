#include "ksn_proc_grid.h"

#include <limits.h>
#include <string.h>

#define KSN_GRID_MAX_TAPS 256u
#define KSN_GRID_MAX_BODY_VISITS 2000000u
#define KSN_GRID_QACC_MAX ((INT64_C(1) << 39) - 1)

static bool add64(int64_t a, int64_t b, int64_t *out)
{
    return !__builtin_add_overflow(a, b, out);
}

static bool mul64(int64_t a, int64_t b, int64_t *out)
{
    return !__builtin_mul_overflow(a, b, out);
}

static bool valid_index(const ksn_grid_index *index)
{
    for (unsigned k = 0; k < 5; ++k) {
        uint8_t p = index->term[k].param;
        if (p != KSN_GRID_NO_PARAM && p >= KSN_GRID_PARAMS) return false;
    }
    return true;
}

typedef enum { MAC_UNKNOWN, MAC_ACC, MAC_LEAF, MAC_PRODUCT, MAC_SUM } mac_kind;
typedef struct {
    mac_kind kind;
    uint16_t contributors;
    ksn_grid_mac_operand left, right, extra_left, extra_right;
} mac_value;

static bool mac_term(const mac_value *value, ksn_grid_mac_operand *left,
                     ksn_grid_mac_operand *right)
{
    if (value->kind != MAC_LEAF && value->kind != MAC_PRODUCT) return false;
    *left = value->left;
    *right = value->kind == MAC_PRODUCT ? value->right :
             (ksn_grid_mac_operand){.constant = 1};
    return true;
}

static bool mac_constant(const mac_value *value, int16_t number)
{
    return value->kind == MAC_LEAF && !value->left.is_load &&
           value->left.constant == number;
}

static bool mac_scaled_load(const mac_value *value,
                            ksn_grid_mac_operand *load, int16_t *coefficient)
{
    if (value->kind == MAC_LEAF && value->left.is_load) {
        *load = value->left;
        *coefficient = 1;
        return true;
    }
    if (value->kind != MAC_PRODUCT ||
        value->left.is_load == value->right.is_load) return false;
    *load = value->left.is_load ? value->left : value->right;
    *coefficient = value->left.is_load ? value->right.constant :
                   value->left.constant;
    return true;
}

/* Interpret the bounded register program as versioned values at registration.
 * Fold only arithmetic whose intermediate value is known to fit int16, plus
 * ADD 0 / MUL 1 identities. All instructions must reach the final result: a
 * discarded checked operation could fail in the scalar VM. */
static ksn_grid_mac normalize_mac(const ksn_grid_program *p)
{
    ksn_grid_mac mac = {0};
    uint8_t acc = p->result_reg;
    mac_value values[KSN_GRID_REGS] = {{0}};
    values[acc].kind = MAC_ACC;
    for (unsigned i = 0; i + 1u < p->count; ++i) {
        const ksn_grid_instruction *in = &p->body[i];
        if (in->dst == acc) return mac;
        mac_value next = {.contributors = (uint16_t)(1u << i)};
        if (in->op == KSN_GRID_LOAD || in->op == KSN_GRID_CONST) {
            if (in->op == KSN_GRID_LOAD && in->buffer == KSN_GRID_DEST)
                return mac;
            next.kind = MAC_LEAF;
            next.left = (ksn_grid_mac_operand){
                .is_load = in->op == KSN_GRID_LOAD,
                .instruction = (uint8_t)i,
                .constant = in->immediate
            };
        } else {
            mac_value a = values[in->a], b = values[in->b];
            if (a.kind == MAC_UNKNOWN || b.kind == MAC_UNKNOWN ||
                a.kind == MAC_ACC || b.kind == MAC_ACC) return mac;
            next.contributors |= a.contributors | b.contributors;
            if (in->op == KSN_GRID_ADD && mac_constant(&a, 0)) next = b;
            else if (in->op == KSN_GRID_ADD && mac_constant(&b, 0)) next = a;
            else if (in->op == KSN_GRID_MUL && mac_constant(&a, 1)) next = b;
            else if (in->op == KSN_GRID_MUL && mac_constant(&b, 1)) next = a;
            else if (a.kind == MAC_LEAF && b.kind == MAC_LEAF &&
                     !a.left.is_load && !b.left.is_load) {
                int64_t folded = 0;
                if (in->op == KSN_GRID_ADD)
                    folded = (int32_t)a.left.constant + b.left.constant;
                else if (in->op == KSN_GRID_MUL)
                    folded = (int32_t)a.left.constant * b.left.constant;
                else if (in->op == KSN_GRID_MIN)
                    folded = a.left.constant < b.left.constant ?
                             a.left.constant : b.left.constant;
                else return mac;
                if (folded < INT16_MIN || folded > INT16_MAX) {
                    if (in->op != KSN_GRID_MUL) return mac;
                    next.kind = MAC_PRODUCT;
                    next.left = a.left;
                    next.right = b.left;
                } else {
                    next.kind = MAC_LEAF;
                    next.left.constant = (int16_t)folded;
                }
            } else if (in->op == KSN_GRID_MUL && a.kind == MAC_LEAF &&
                       b.kind == MAC_LEAF) {
                next.kind = MAC_PRODUCT;
                next.left = a.left;
                next.right = b.left;
            } else if (in->op == KSN_GRID_ADD) {
                ksn_grid_mac_operand a_load, b_load;
                int16_t a_coefficient, b_coefficient;
                bool factor = mac_scaled_load(&a, &a_load, &a_coefficient) &&
                              mac_scaled_load(&b, &b_load, &b_coefficient) &&
                              a_load.instruction == b_load.instruction;
                int32_t combined = factor ?
                    (int32_t)a_coefficient + b_coefficient : 0;
                if (factor && combined >= INT16_MIN && combined <= INT16_MAX) {
                    next.kind = MAC_PRODUCT;
                    next.left = a_load;
                    next.right = (ksn_grid_mac_operand){
                        .constant = (int16_t)combined};
                } else if (mac_term(&a, &next.left, &next.right) &&
                           mac_term(&b, &next.extra_left, &next.extra_right)) {
                    next.kind = MAC_SUM;
                } else return mac;
            } else return mac;
            next.contributors = (uint16_t)(next.contributors |
                                  a.contributors | b.contributors | (1u << i));
        }
        values[in->dst] = next;
    }
    const ksn_grid_instruction *add = &p->body[p->count - 1u];
    if (add->op != KSN_GRID_ADD || add->dst != acc ||
        (add->a == acc) == (add->b == acc)) return mac;
    mac_value term = values[add->a == acc ? add->b : add->a];
    if ((term.kind != MAC_LEAF && term.kind != MAC_PRODUCT &&
         term.kind != MAC_SUM) ||
        (unsigned)(term.contributors | (1u << (p->count - 1u))) !=
            ((1u << p->count) - 1u)) return mac;
    mac.left = term.left;
    mac.right = term.kind == MAC_LEAF ?
                (ksn_grid_mac_operand){.constant = 1} :
                term.right;
    mac.terms = term.kind == MAC_SUM ? 2 : 1;
    if (mac.terms == 2) {
        mac.extra_left = term.extra_left;
        mac.extra_right = term.extra_right;
    }
    mac.valid = true;
    return mac;
}

ksn_grid_status ksn_grid_prepare(const ksn_grid_program *program,
                                 ksn_grid_plan *plan)
{
    if (!plan) return KSN_GRID_BAD_IR;
    memset(plan, 0, sizeof(*plan));
    if (!program || !program->count || program->count > KSN_GRID_CODE ||
        program->result_reg >= KSN_GRID_REGS || program->final_shift > 30 ||
        !valid_index(&program->output)) return KSN_GRID_BAD_IR;
    uint8_t defined = (uint8_t)(1u << program->result_reg);
    for (unsigned i = 0; i < program->count; ++i) {
        const ksn_grid_instruction *in = &program->body[i];
        if (in->dst >= KSN_GRID_REGS) return KSN_GRID_BAD_IR;
        switch (in->op) {
        case KSN_GRID_CONST: break;
        case KSN_GRID_LOAD:
            if (in->buffer >= KSN_GRID_BUFFERS || !valid_index(&in->index))
                return KSN_GRID_BAD_IR;
            break;
        case KSN_GRID_ADD:
        case KSN_GRID_MUL:
        case KSN_GRID_MIN:
            if (in->a >= KSN_GRID_REGS || in->b >= KSN_GRID_REGS)
                return KSN_GRID_BAD_IR;
            if (!(defined & (1u << in->a)) ||
                !(defined & (1u << in->b))) return KSN_GRID_BAD_IR;
            break;
        default: return KSN_GRID_BAD_IR;
        }
        defined |= (uint8_t)(1u << in->dst);
    }
    plan->program = *program;
    plan->mac = normalize_mac(&plan->program);
    plan->prepared = true;
    return KSN_GRID_OK;
}

static bool resolve_index(const ksn_grid_index *index,
                          const int32_t param[KSN_GRID_PARAMS],
                          int64_t out[5], uint32_t *work)
{
    for (unsigned k = 0; k < 5; ++k) {
        const ksn_grid_coeff *c = &index->term[k];
        int64_t v = c->constant;
        if (c->param != KSN_GRID_NO_PARAM) {
            int64_t scaled;
            if (!mul64(c->scale, param[c->param], &scaled) ||
                !add64(v, scaled, &v)) return false;
        }
        out[k] = v;
        ++*work;
    }
    return true;
}

static bool index_bounds(const int64_t index[5],
                         const ksn_grid_shape *shape, bool output,
                         size_t count, uint32_t *work)
{
    const uint32_t max[5] = {0, shape->width - 1u, shape->height - 1u,
                             output ? 0u : shape->tap_width - 1u,
                             output ? 0u : shape->tap_height - 1u};
    int64_t lo = index[0], hi = index[0];
    for (unsigned k = 1; k < 5; ++k) {
        int64_t span;
        if (!mul64(index[k], max[k], &span)) return false;
        if (span < 0) {
            if (!add64(lo, span, &lo)) return false;
        } else if (!add64(hi, span, &hi)) return false;
        ++*work;
    }
    return lo >= 0 && (uint64_t)hi < count;
}

static bool disjoint(const ksn_grid_binding *binding, uint8_t used)
{
    uintptr_t d = (uintptr_t)binding->data[KSN_GRID_DEST];
    size_t dn = binding->count[KSN_GRID_DEST] * sizeof(int16_t);
    if (d > UINTPTR_MAX - dn) return false;
    for (unsigned id = 0; id < KSN_GRID_BUFFERS; ++id) {
        if (!(used & (1u << id))) continue;
        if (id == KSN_GRID_DEST) return false;
        uintptr_t s = (uintptr_t)binding->data[id];
        size_t sn = binding->count[id] * sizeof(int16_t);
        if (s > UINTPTR_MAX - sn) return false;
        if (s + sn > d && d + dn > s) return false;
    }
    return true;
}

static uint64_t magnitude(int64_t n)
{
    return n < 0 ? (uint64_t)(-(n + 1)) + 1u : (uint64_t)n;
}

static bool unique_output(const int64_t index[5],
                          const ksn_grid_shape *shape)
{
    if (index[3] || index[4]) return false;
    if (shape->width <= 1 && shape->height <= 1) return true;
    uint64_t x = magnitude(index[1]), y = magnitude(index[2]);
    if (shape->height <= 1) return x != 0;
    if (shape->width <= 1) return y != 0;
    return (x && y > (uint64_t)(shape->width - 1u) * x) ||
           (y && x > (uint64_t)(shape->height - 1u) * y);
}

/* The first load is exactly the preceding output in the same row. Another
 * load is immutable input, so eight rows can advance together without
 * changing the left-to-right order or per-cell saturation of any row. */
static bool scan_rows(const ksn_grid_execution *e, uint8_t *source_slot,
                      int16_t *prev_coefficient)
{
    const ksn_grid_program *p = &e->plan->program;
    if (e->shape.tap_width != 1 || e->shape.tap_height != 1 ||
        (p->count != 3 && p->count != 4 && p->count != 6) || p->initial ||
        e->output[0] < 1 || e->output[1] != 1 ||
        e->output[2] < (int64_t)e->shape.width + 1) return false;
    const ksn_grid_instruction *prev = &p->body[0];
    unsigned source = p->count == 6 ? 3u : 1u;
    const ksn_grid_instruction *src = &p->body[source];
    if (prev->op != KSN_GRID_LOAD || prev->buffer != KSN_GRID_DEST ||
        src->op != KSN_GRID_LOAD || src->buffer == KSN_GRID_DEST ||
        prev->dst == src->dst || prev->dst == p->result_reg ||
        src->dst == p->result_reg) return false;
    uint8_t term;
    int16_t coefficient = 1;
    if (p->count == 6) {
        const ksn_grid_instruction *constant = &p->body[1];
        const ksn_grid_instruction *mul = &p->body[2];
        const ksn_grid_instruction *sum = &p->body[4];
        uint8_t defined = (uint8_t)(1u << p->result_reg);
        for (unsigned i = 0; i < 5; ++i) {
            uint8_t bit = (uint8_t)(1u << p->body[i].dst);
            if (defined & bit) return false;
            defined |= bit;
        }
        if (constant->op != KSN_GRID_CONST ||
            mul->op != KSN_GRID_MUL ||
            !((mul->a == prev->dst && mul->b == constant->dst) ||
              (mul->b == prev->dst && mul->a == constant->dst)) ||
            sum->op != KSN_GRID_ADD ||
            !((sum->a == mul->dst && sum->b == src->dst) ||
              (sum->b == mul->dst && sum->a == src->dst))) return false;
        coefficient = constant->immediate;
        term = sum->dst;
    } else {
        const ksn_grid_instruction *sum = &p->body[2];
        if (sum->op != KSN_GRID_ADD ||
            !((sum->a == prev->dst && sum->b == src->dst) ||
              (sum->b == prev->dst && sum->a == src->dst))) return false;
        term = sum->dst;
    }
    if (p->count == 3) {
        if (term != p->result_reg) return false;
    } else {
        const ksn_grid_instruction *finish = &p->body[p->count - 1u];
        if (term == p->result_reg || finish->op != KSN_GRID_ADD ||
            finish->dst != p->result_reg ||
            !((finish->a == p->result_reg && finish->b == term) ||
              (finish->b == p->result_reg && finish->a == term))) return false;
    }
    if (e->index[0][0] != e->output[0] - 1) return false;
    for (unsigned k = 1; k < 5; ++k)
        if (e->index[0][k] != e->output[k]) return false;
    uintptr_t d = (uintptr_t)e->binding.data[KSN_GRID_DEST];
    uintptr_t s = (uintptr_t)e->binding.data[src->buffer];
    size_t dn = e->binding.count[KSN_GRID_DEST] * sizeof(int16_t);
    size_t sn = e->binding.count[src->buffer] * sizeof(int16_t);
    if (d > UINTPTR_MAX - dn || s > UINTPTR_MAX - sn ||
        (s + sn > d && d + dn > s)) return false;
    *source_slot = (uint8_t)source;
    *prev_coefficient = coefficient;
    return true;
}

/* Recognize a single loop-carried add. Other registers must be computed
 * anew on every tap; their expression graph may use any supported pure op. */
static bool reduction_bound(const ksn_grid_program *program,
                            uint32_t taps, uint32_t *work, bool *qacc_legal)
{
    uint64_t bound[KSN_GRID_REGS] = {0};
    uint8_t defined = 0;
    uint8_t acc = program->result_reg;
    unsigned updates = 0;
    *qacc_legal = true;
    uint64_t initial = magnitude(program->initial);
    if (initial > (uint64_t)KSN_GRID_QACC_MAX) *qacc_legal = false;
    for (unsigned i = 0; i < program->count; ++i) {
        const ksn_grid_instruction *in = &program->body[i];
        uint8_t dest_bit = (uint8_t)(1u << in->dst);
        ++*work;
        if (in->op == KSN_GRID_LOAD) {
            if (in->buffer == KSN_GRID_DEST || in->dst == acc) return false;
            bound[in->dst] = 32768u;
        } else if (in->op == KSN_GRID_CONST) {
            if (in->dst == acc) return false;
            bound[in->dst] = magnitude(in->immediate);
        } else {
            bool a_acc = in->a == acc, b_acc = in->b == acc;
            if ((!a_acc && !(defined & (1u << in->a))) ||
                (!b_acc && !(defined & (1u << in->b)))) return false;
            if (in->dst == acc) {
                if (in->op != KSN_GRID_ADD || a_acc == b_acc ||
                    ++updates != 1 || i != program->count - 1u) return false;
                uint64_t term = bound[a_acc ? in->b : in->a];
                if (initial <= (uint64_t)KSN_GRID_QACC_MAX &&
                    term > ((uint64_t)KSN_GRID_QACC_MAX - initial) / taps)
                    *qacc_legal = false;
            } else {
                if (a_acc || b_acc) return false;
                uint64_t a = bound[in->a], b = bound[in->b];
                if (in->op == KSN_GRID_ADD) {
                    if (a > (uint64_t)INT64_MAX - b) return false;
                    bound[in->dst] = a + b;
                } else if (in->op == KSN_GRID_MUL) {
                    if (b && a > (uint64_t)INT64_MAX / b) return false;
                    bound[in->dst] = a * b;
                } else {
                    bound[in->dst] = a > b ? a : b;
                }
            }
        }
        if (in->dst != acc) defined |= dest_bit;
    }
    return updates == 1;
}

ksn_grid_status ksn_grid_begin(const ksn_grid_plan *plan,
                               const ksn_grid_shape *shape,
                               const ksn_grid_binding *binding,
                               ksn_grid_execution *execution)
{
    if (!execution) return KSN_GRID_BAD_IR;
    memset(execution, 0, sizeof(*execution));
    if (!plan || !plan->prepared || !shape || !binding) return KSN_GRID_BAD_IR;
    uint32_t taps = (uint32_t)shape->tap_width * shape->tap_height;
    uint64_t visits = (uint64_t)shape->width * shape->height * taps;
    if (!taps || taps > KSN_GRID_MAX_TAPS ||
        visits > KSN_GRID_MAX_BODY_VISITS) return KSN_GRID_BAD_SHAPE;
    execution->plan = plan;
    execution->shape = *shape;
    execution->binding = *binding;
    uint32_t *work = &execution->validation_work;
    if (!resolve_index(&plan->program.output, binding->param,
                       execution->output, work)) return KSN_GRID_BAD_INDEX;
    if (execution->output[3] || execution->output[4])
        return KSN_GRID_BAD_INDEX;
    for (unsigned i = 0; i < plan->program.count; ++i) {
        const ksn_grid_instruction *in = &plan->program.body[i];
        ++*work;
        if (in->op == KSN_GRID_LOAD &&
            !resolve_index(&in->index, binding->param,
                           execution->index[i], work)) return KSN_GRID_BAD_INDEX;
        if (in->op == KSN_GRID_LOAD) {
            int64_t stride = execution->index[i][1];
            execution->access[i] = stride == 0 ? KSN_GRID_ACCESS_BROADCAST :
                                   stride == 1 ? KSN_GRID_ACCESS_CONTIGUOUS :
                                   stride == 2 ? KSN_GRID_ACCESS_INTERLEAVED2 :
                                                 KSN_GRID_ACCESS_GATHER;
        }
    }
    if (!visits) {
        execution->safe = true;
        return KSN_GRID_OK;
    }
    uint8_t used = 0;
    for (unsigned i = 0; i < plan->program.count; ++i)
        if (plan->program.body[i].op == KSN_GRID_LOAD)
            used |= (uint8_t)(1u << plan->program.body[i].buffer);
    for (unsigned id = 0; id < KSN_GRID_BUFFERS; ++id)
        if (((used | (1u << KSN_GRID_DEST)) & (1u << id)) &&
            (!binding->data[id] ||
             binding->count[id] > SIZE_MAX / sizeof(int16_t)))
            return KSN_GRID_BAD_BUFFER;
    if (!index_bounds(execution->output, shape, true,
                      binding->count[KSN_GRID_DEST],
                      work)) return KSN_GRID_BAD_INDEX;
    for (unsigned i = 0; i < plan->program.count; ++i) {
        const ksn_grid_instruction *in = &plan->program.body[i];
        if (in->op != KSN_GRID_LOAD) continue;
        if (!index_bounds(execution->index[i], shape, false,
                          binding->count[in->buffer], work))
            return KSN_GRID_BAD_INDEX;
    }
    execution->safe = true;
    execution->independent = disjoint(binding, used) &&
        unique_output(execution->output, shape);
    execution->reduction_shape = reduction_bound(&plan->program, taps, work,
                                                 &execution->qacc_legal);
    if (!execution->reduction_shape) execution->qacc_legal = false;
    execution->pie_candidate = execution->independent &&
                               execution->reduction_shape && execution->qacc_legal;
    execution->scan_rows_candidate = scan_rows(execution,
                                               &execution->scan_source_slot,
                                               &execution->scan_prev_coefficient);
    return KSN_GRID_OK;
}

static size_t address(const int64_t index[5], unsigned x, unsigned y,
                      unsigned tap_x, unsigned tap_y)
{
    return (size_t)(index[0] + index[1] * x + index[2] * y +
                    index[3] * tap_x + index[4] * tap_y);
}

static ksn_grid_status execute_tap(const ksn_grid_execution *execution,
                                   unsigned x, unsigned y,
                                   unsigned tap_x, unsigned tap_y,
                                   int64_t reg[KSN_GRID_REGS])
{
    const ksn_grid_program *p = &execution->plan->program;
    for (unsigned i = 0; i < p->count; ++i) {
        const ksn_grid_instruction *in = &p->body[i];
        int64_t v = 0;
        switch (in->op) {
        case KSN_GRID_CONST: v = in->immediate; break;
        case KSN_GRID_LOAD: {
            size_t at = address(execution->index[i], x, y, tap_x, tap_y);
            v = execution->binding.data[in->buffer][at];
            break;
        }
        case KSN_GRID_ADD:
            if (!add64(reg[in->a], reg[in->b], &v))
                return KSN_GRID_ARITH_OVERFLOW;
            break;
        case KSN_GRID_MUL:
            if (!mul64(reg[in->a], reg[in->b], &v))
                return KSN_GRID_ARITH_OVERFLOW;
            break;
        case KSN_GRID_MIN:
            v = reg[in->a] < reg[in->b] ? reg[in->a] : reg[in->b];
            break;
        default: return KSN_GRID_BAD_IR;
        }
        reg[in->dst] = v;
    }
    return KSN_GRID_OK;
}

static void store_output(const ksn_grid_execution *execution,
                         unsigned x, unsigned y,
                         const int64_t reg[KSN_GRID_REGS])
{
    const ksn_grid_program *p = &execution->plan->program;
    int64_t value = reg[p->result_reg];
    int64_t divisor = INT64_C(1) << p->final_shift;
    int64_t q = value / divisor;
    if (value < 0 && value % divisor) --q;
    if (q > INT16_MAX) q = INT16_MAX;
    if (q < INT16_MIN) q = INT16_MIN;
    execution->binding.data[KSN_GRID_DEST][address(execution->output,
                                                   x, y, 0, 0)] = (int16_t)q;
}

ksn_grid_status ksn_grid_run_scalar(const ksn_grid_execution *execution)
{
    if (!execution || !execution->safe) return KSN_GRID_BAD_IR;
    for (unsigned y = 0; y < execution->shape.height; ++y) {
        for (unsigned x = 0; x < execution->shape.width; ++x) {
            int64_t reg[KSN_GRID_REGS] = {0};
            reg[execution->plan->program.result_reg] =
                execution->plan->program.initial;
            for (unsigned ty = 0; ty < execution->shape.tap_height; ++ty)
                for (unsigned tx = 0; tx < execution->shape.tap_width; ++tx) {
                    ksn_grid_status s = execute_tap(execution, x, y, tx, ty, reg);
                    if (s != KSN_GRID_OK) return s;
                }
            store_output(execution, x, y, reg);
        }
    }
    return KSN_GRID_OK;
}

ksn_grid_status ksn_grid_run_lanes_model(const ksn_grid_execution *execution)
{
    if (!execution || !execution->safe || !execution->pie_candidate)
        return KSN_GRID_BAD_IR;
    unsigned total = (unsigned)execution->shape.width * execution->shape.height;
    for (unsigned start = 0; start < total; start += 8u) {
        unsigned lanes = total - start < 8u ? total - start : 8u;
        int64_t reg[8][KSN_GRID_REGS] = {{0}};
        for (unsigned lane = 0; lane < lanes; ++lane)
            reg[lane][execution->plan->program.result_reg] =
                execution->plan->program.initial;
        for (unsigned ty = 0; ty < execution->shape.tap_height; ++ty)
            for (unsigned tx = 0; tx < execution->shape.tap_width; ++tx)
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    unsigned at = start + lane;
                    unsigned x = at % execution->shape.width;
                    unsigned y = at / execution->shape.width;
                    ksn_grid_status s = execute_tap(execution, x, y, tx, ty,
                                                    reg[lane]);
                    if (s != KSN_GRID_OK) return s;
                }
        for (unsigned lane = 0; lane < lanes; ++lane) {
            unsigned at = start + lane;
            store_output(execution, at % execution->shape.width,
                         at / execution->shape.width, reg[lane]);
        }
    }
    return KSN_GRID_OK;
}
