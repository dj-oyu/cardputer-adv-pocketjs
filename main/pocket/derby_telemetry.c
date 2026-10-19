#include "derby_telemetry.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Fixed owner-task storage. No heap, guest values, locks, or background work. */
static struct {
    derby_telemetry_context context, previous;
    uint64_t interval_us[10];
    uint32_t intervals[10];
    uint32_t run, context_seq, rendered_seq, previous_seq, samples, refused, lines, io_us_max;
    size_t heap_max, cap, eval_used, eval_cap;
    int64_t first_us, previous_us, last_emit_us;
    bool set, rendered, have_previous;
} state;
static uint32_t run_serial;
static int64_t last_output_us; /* global one-line/s ceiling, including new runs */

void derby_telemetry_reset(void) {
    memset(&state, 0, sizeof state);
    state.run = ++run_serial;
}
void derby_telemetry_set(derby_telemetry_context context) {
    if (context.camera >= 10 || context.phase > 4 || context.tier > 2 ||
        context.u_ppm < -1 || context.u_ppm > 1000000) return;
    state.context = context;
    state.set = true;
    state.rendered = false;
}
void derby_telemetry_begin_frame(void) {
    ++state.context_seq;
    state.set = false;
    state.rendered = false;
}
void derby_telemetry_eval(size_t used, size_t cap) {
    state.eval_used = used;
    state.eval_cap = cap;
}
void derby_telemetry_io_us(uint32_t elapsed) {
    if (elapsed > state.io_us_max) state.io_us_max = elapsed;
}
static bool same_camera(derby_telemetry_context a, derby_telemetry_context b) {
    return a.seed == b.seed && a.race == b.race && a.phase == b.phase &&
           a.camera == b.camera && a.tier == b.tier;
}
void derby_telemetry_present(int64_t now, size_t used, size_t cap) {
    if (!state.set || state.rendered || now < 0) return;
    state.rendered = true;
    state.rendered_seq = state.context_seq;
    if (!state.samples) state.first_us = state.last_emit_us = now;
    ++state.samples;
    if (used > state.heap_max) state.heap_max = used;
    state.cap = cap;
    /* Count only adjacent completed contexts in the same camera/race/phase.
     * Repaired/continued presents of one context do not create extra frames. */
    if (state.have_previous && now > state.previous_us &&
        state.context_seq == state.previous_seq + 1 && same_camera(state.context, state.previous)) {
        ++state.intervals[state.context.camera];
        state.interval_us[state.context.camera] += (uint64_t)(now - state.previous_us);
    }
    state.previous = state.context;
    state.previous_us = now;
    state.previous_seq = state.context_seq;
    state.have_previous = true;
    if (state.lines >= 300 || now < state.last_emit_us || now - state.last_emit_us < 1000000 ||
        now < last_output_us || now - last_output_us < 1000000) return;
    state.last_emit_us = last_output_us = now;
    ++state.lines;
    char line[768];
    int n = snprintf(line, sizeof line,
        "DERBY_TLM v=1 run=%"PRIu32" us=%"PRId64" begin=%"PRId64
        " frame=%"PRIu32" ctx=%"PRIu32" seed=%08"PRIX32" race=%"PRIu32
        " phase=%"PRIu32" cam=%"PRIu32" tier=%"PRIu32" step=%"PRIu32" u=%"PRId32
        " gu_sample_max=%zu cap=%zu eval_used=%zu eval_cap=%zu refused=%"PRIu32" seq=%"PRIu32" limit=300 io_us_max=%"PRIu32" intervals=",
        state.run, now, state.first_us, state.samples, state.rendered_seq,
        state.context.seed, state.context.race, state.context.phase, state.context.camera,
        state.context.tier, state.context.step, state.context.u_ppm, state.heap_max,
        state.cap, state.eval_used, state.eval_cap, state.refused, state.lines, state.io_us_max);
    if (n < 0 || (size_t)n >= sizeof line) { ++state.refused; return; }
    size_t at = (size_t)n;
    for (unsigned i = 0; i < 10; ++i) {
        n = snprintf(line + at, sizeof line - at, "%s%"PRIu32":%"PRIu64,
                     i ? "," : "", state.intervals[i], state.interval_us[i]);
        if (n < 0 || (size_t)n >= sizeof line - at) { ++state.refused; return; }
        at += (size_t)n;
    }
    if (at + 1 >= sizeof line) { ++state.refused; return; }
    line[at++] = '\n';
    if (!derby_telemetry_write(line, at)) ++state.refused;
}
