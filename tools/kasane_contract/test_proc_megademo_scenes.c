/* Differential oracle and load statistics for every MEGADEMO scene.
 *
 * Reads the plan specs and per-frame draw lists straight from
 * apps/kasane/proc_megademo.js (procMegademo.specs / frame) on the real
 * QuickJS, for every tier and every frame of every scene, and runs each draw
 * three ways that must agree bit for bit:
 *   plan    ksn_proc_plan_run(debug_step=false)   -- what pocket_proc.c runs
 *   debug   ksn_proc_plan_run(debug_step=true)
 *   vm      ksn_proc_begin + ksn_proc_step until done (single-step debugger)
 * Typed point batches run scalar, the PIE model and the plan dispatcher. The
 * frame is then rendered from the plan segments and from the VM segments and
 * compared pixel for pixel; Act I is also compared with proc_megademo.h.
 *
 * The same pass enforces the adapter's limits (pocket_proc.c draw_impl):
 * 1,024 segments and 65,535 raster steps per committed frame, 8,192 raster
 * and 10,000 steps per VM draw, 8 inputs, 32 live plans, typed points in
 * -480..720. Statistics are printed as one line per tier and scene; with an
 * output directory every frame is written as PPM for the preview sheet.
 *
 *   python3 tools/kasane_contract/run_proc_megademo_scenes.py [--out DIR]
 */
#include "quickjs.h"
#include "ksn_proc_plan.h"
#include "ksn_proc_points_pie.h"
#include "proc_megademo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(t) do { if (!(t)) { \
    fprintf(stderr, "%s:%d: %s (tier %u scene %u frame %u draw %u)\n", \
            __FILE__, __LINE__, #t, g_tier, g_scene, g_frame, g_draw); exit(1); \
} } while (0)

#define MAX_PLANS 32u
#define MAX_POINTS 128u
static unsigned g_tier, g_scene, g_frame, g_draw;

typedef struct {
    ksn_proc_inst code[KSN_PROC_CODE];
    ksn_proc_program program;
    ksn_proc_plan plan;
    bool has_points;
    unsigned points;
    uint16_t color;
    KsnProcAffineQ14 coeff;
    _Alignas(16) int16_t x[MAX_POINTS], y[MAX_POINTS];
    _Alignas(16) int16_t sx[MAX_POINTS], sy[MAX_POINTS];   /* scalar */
    _Alignas(16) int16_t px[MAX_POINTS], py[MAX_POINTS];   /* PIE model */
    _Alignas(16) int16_t dx[MAX_POINTS], dy[MAX_POINTS];   /* dispatcher */
    unsigned regs, depth, inputs_read;
    unsigned max_segs, max_raster, max_steps;  /* per draw, over the scene */
} spec;

static spec specs[MAX_PLANS];
static ksn_proc_frame f_plan, f_debug, f_vm;
static ksn_proc_frame cand_plan, cand_vm;
static uint16_t pix_plan[KSN_PROC_W * KSN_PROC_H], pix_vm[KSN_PROC_W * KSN_PROC_H];
static uint16_t pix_ref[KSN_PROC_W * KSN_PROC_H];

typedef struct {
    unsigned frames, surf1_frames, plans, draws_max;
    unsigned seg_max, seg_sum, raster_max, draw_raster_max, draw_steps_max;
    unsigned long long steps_sum; unsigned frame_steps_max;
    unsigned instr_max, regs_max, depth_max, inputs_max, points_max;
    int pt_lo, pt_hi;
    unsigned ops_used;
} stats;

/* FNV-1a over programs and pixels: a rewrite of the app must print the same pair. */
static uint64_t program_hash = 1469598103934665603ull, pixel_hash = 1469598103934665603ull;
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static JSContext *ctx;
static JSValue demo;

static JSValue call(const char *name, int argc, JSValueConst *argv) {
    JSValue fn = JS_GetPropertyStr(ctx, demo, name);
    JSValue r = JS_Call(ctx, fn, demo, argc, argv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "procMegademo.%s: %s\n", name, m ? m : "exception");
        exit(1);
    }
    return r;
}
static double num(JSValueConst v) {
    double d = NAN;
    REQUIRE(JS_ToFloat64(ctx, &d, v) == 0 && isfinite(d));
    return d;
}
static double at(JSValueConst a, unsigned i) {
    JSValue v = JS_GetPropertyUint32(ctx, a, i);
    double d = num(v);
    JS_FreeValue(ctx, v);
    return d;
}
static unsigned len(JSValueConst a) {
    JSValue v = JS_GetPropertyStr(ctx, a, "length");
    unsigned n = (unsigned)num(v);
    JS_FreeValue(ctx, v);
    return n;
}
static JSValue prop(JSValueConst o, const char *k) { return JS_GetPropertyStr(ctx, o, k); }
static int ival(double d) { REQUIRE(d == floor(d)); return (int)d; }

/* Registers touched, inputs read and loop depth, from the op semantics in
 * ksn_procedural.c (CUBIC reads r0..r7). */
static void analyse(spec *s, unsigned *ops) {
    uint16_t used = 0; unsigned depth = 0, maxd = 0, inputs = 0;
    for (unsigned i = 0; i < s->program.count; ++i) {
        const ksn_proc_inst *c = &s->code[i];
        *ops |= 1u << c->op;
        switch (c->op) {
        case KSN_PROC_SET: used |= 1u << c->dst; break;
        case KSN_PROC_INPUT: used |= 1u << c->dst; if (c->a + 1u > inputs) inputs = c->a + 1u; break;
        case KSN_PROC_ADD: case KSN_PROC_MUL: used |= 1u << c->dst | 1u << c->a | 1u << c->b; break;
        case KSN_PROC_SIN: used |= 1u << c->dst | 1u << c->a; break;
        case KSN_PROC_REPEAT: case KSN_PROC_REPEAT_REG:
            if (c->op == KSN_PROC_REPEAT_REG) used |= 1u << c->a;
            if (++depth > maxd) maxd = depth;
            break;
        case KSN_PROC_END: --depth; break;
        case KSN_PROC_BREAK_IF_GT: case KSN_PROC_MOVE: case KSN_PROC_PLOT: case KSN_PROC_LINE:
            used |= 1u << c->a | 1u << c->b; break;
        case KSN_PROC_PLOT_COLOR_REG: case KSN_PROC_LINE_COLOR_REG:
            used |= 1u << c->dst | 1u << c->a | 1u << c->b; break;
        case KSN_PROC_CUBIC: used |= 0xffu; break;
        default: REQUIRE(!"opcode");
        }
    }
    unsigned n = 0;
    for (unsigned r = 0; r < 16; ++r) n += (used >> r) & 1u;
    s->regs = n; s->depth = maxd; s->inputs_read = inputs;
}

static void load_spec(spec *s, JSValueConst sp, unsigned *ops) {
    memset(s, 0, sizeof *s);
    JSValue code = JS_GetPropertyUint32(ctx, sp, 0);
    unsigned n = len(code);
    REQUIRE(n >= 1 && n <= KSN_PROC_CODE);
    for (unsigned i = 0; i < n; ++i) {
        JSValue row = JS_GetPropertyUint32(ctx, code, i);
        REQUIRE(len(row) == 6);
        ksn_proc_inst *c = &s->code[i];
        c->op = (uint8_t)ival(at(row, 0)); c->dst = (uint8_t)ival(at(row, 1));
        c->a = (uint8_t)ival(at(row, 2)); c->b = (uint8_t)ival(at(row, 3));
        /* The adapter converts the double to float exactly like this. */
        c->value = (float)at(row, 4); c->color = (uint16_t)ival(at(row, 5));
        JS_FreeValue(ctx, row);
    }
    JS_FreeValue(ctx, code);
    s->program = (ksn_proc_program){s->code, (uint8_t)n};
    REQUIRE(ksn_proc_plan_prepare(&s->plan, &s->program));
    analyse(s, ops);
    JSValue b = JS_GetPropertyUint32(ctx, sp, 1);
    if (!JS_IsNull(b) && !JS_IsUndefined(b)) {
        JSValue x = prop(b, "x"), y = prop(b, "y"), k = prop(b, "coeff"), col = prop(b, "color");
        s->points = len(x);
        REQUIRE(s->points >= 2 && s->points <= MAX_POINTS && len(y) == s->points && len(k) == 6);
        for (unsigned i = 0; i < s->points; ++i) {
            s->x[i] = (int16_t)ival(at(x, i)); s->y[i] = (int16_t)ival(at(y, i));
        }
        s->coeff = (KsnProcAffineQ14){(int16_t)ival(at(k, 0)), (int16_t)ival(at(k, 1)),
            (int16_t)ival(at(k, 2)), (int16_t)ival(at(k, 3)), ival(at(k, 4)), ival(at(k, 5))};
        s->color = (uint16_t)ival(num(col));
        const KsnProcPointsPolicy policy = {true, 8};
        REQUIRE(ksn_proc_plan_register_points_affine(&s->plan, &s->coeff, &policy));
        s->has_points = true;
        JS_FreeValue(ctx, x); JS_FreeValue(ctx, y); JS_FreeValue(ctx, k); JS_FreeValue(ctx, col);
    }
    JS_FreeValue(ctx, b);
}

static void append(ksn_proc_frame *cand, const ksn_proc_frame *f) {
    REQUIRE((unsigned)cand->count + f->count <= KSN_PROC_SEGMENTS);
    memcpy(&cand->segments[cand->count], f->segments, f->count * sizeof f->segments[0]);
    cand->count = (uint16_t)(cand->count + f->count);
    REQUIRE((unsigned)cand->raster_steps + f->raster_steps <= UINT16_MAX);
    cand->raster_steps = (uint16_t)(cand->raster_steps + f->raster_steps);
}

/* One draw, three executors; returns VM steps. */
static unsigned run_draw(spec *s, const float in[KSN_PROC_INPUTS], stats *st) {
    ksn_proc_vm vm_plan, vm_debug, vm_raw;
    REQUIRE(ksn_proc_plan_begin(&vm_plan, &s->plan, in, &f_plan) == KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_plan_run(&vm_plan, &s->plan, false) == KSN_PROC_DONE);
    REQUIRE(ksn_proc_plan_begin(&vm_debug, &s->plan, in, &f_debug) == KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_plan_run(&vm_debug, &s->plan, true) == KSN_PROC_DONE);
    REQUIRE(ksn_proc_begin(&vm_raw, &s->program, in, &f_vm) == KSN_PROC_RUNNING);
    ksn_proc_status status;
    unsigned singles = 0;
    do { status = ksn_proc_step(&vm_raw); ++singles; } while (status == KSN_PROC_RUNNING);
    REQUIRE(status == KSN_PROC_DONE);
    REQUIRE(vm_plan.steps == vm_raw.steps && vm_debug.steps == vm_raw.steps && singles == vm_raw.steps);
    const ksn_proc_frame *fs[2] = {&f_debug, &f_vm};
    for (unsigned i = 0; i < 2; ++i) {
        REQUIRE(fs[i]->count == f_plan.count && fs[i]->raster_steps == f_plan.raster_steps);
        REQUIRE(!memcmp(fs[i]->segments, f_plan.segments, f_plan.count * sizeof f_plan.segments[0]));
    }
    REQUIRE(memcmp(vm_plan.reg, vm_raw.reg, sizeof vm_raw.reg) == 0);
    REQUIRE(f_plan.raster_steps <= KSN_PROC_RASTER_STEPS && vm_raw.steps <= KSN_PROC_STEPS);
    if (f_plan.raster_steps > st->draw_raster_max) st->draw_raster_max = f_plan.raster_steps;
    if (vm_raw.steps > st->draw_steps_max) st->draw_steps_max = vm_raw.steps;
    if (vm_raw.steps > s->max_steps) s->max_steps = vm_raw.steps;
    append(&cand_plan, &f_plan);
    append(&cand_vm, &f_vm);
    if (f_plan.count > s->max_segs) s->max_segs = f_plan.count;
    if (f_plan.raster_steps > s->max_raster) s->max_raster = f_plan.raster_steps;
    if (s->has_points) {
        const unsigned n = s->points;
        ksn_proc_points_affine_scalar((KsnProcPointDst){s->sx, s->sy}, (KsnProcPointSrc){s->x, s->y}, n, &s->coeff);
        ksn_proc_points_affine_pie((KsnProcPointDst){s->px, s->py}, (KsnProcPointSrc){s->x, s->y}, n, &s->coeff);
        KsnProcPointsDecision decision;
        REQUIRE(ksn_proc_plan_run_points_affine(&s->plan, (KsnProcPointDst){s->dx, s->dy},
                                                (KsnProcPointSrc){s->x, s->y}, n, &decision));
        REQUIRE(!memcmp(s->sx, s->px, n * 2) && !memcmp(s->sy, s->py, n * 2));
        REQUIRE(!memcmp(s->sx, s->dx, n * 2) && !memcmp(s->sy, s->dy, n * 2));
        ksn_proc_frame typed = {.ready = true};
        for (unsigned i = 0; i < n; ++i) {
            /* pocket_proc.c rejects a typed point outside the VM's box. */
            REQUIRE(s->sx[i] >= -480 && s->sx[i] <= 720 && s->sy[i] >= -480 && s->sy[i] <= 720);
            if (s->sx[i] < st->pt_lo) st->pt_lo = s->sx[i];
            if (s->sy[i] < st->pt_lo) st->pt_lo = s->sy[i];
            if (s->sx[i] > st->pt_hi) st->pt_hi = s->sx[i];
            if (s->sy[i] > st->pt_hi) st->pt_hi = s->sy[i];
            if (i) {
                int dx = abs(s->sx[i] - s->sx[i - 1]), dy = abs(s->sy[i] - s->sy[i - 1]);
                typed.raster_steps = (uint16_t)(typed.raster_steps + (dx > dy ? dx : dy) + 1);
                typed.segments[typed.count++] = (ksn_proc_segment){s->sx[i - 1], s->sy[i - 1],
                    s->sx[i], s->sy[i], s->color};
            }
        }
        append(&cand_plan, &typed);
        append(&cand_vm, &typed);
        if (typed.count > s->max_segs) s->max_segs = typed.count;
        if (typed.raster_steps > s->max_raster) s->max_raster = typed.raster_steps;
        if (n > st->points_max) st->points_max = n;
    }
    return vm_raw.steps;
}

static void write_ppm(const char *dir, unsigned tier, unsigned scene, unsigned frame,
                      unsigned surface, const uint16_t *px) {
    char path[1024];
    snprintf(path, sizeof path, "%s/t%u_s%u_f%03u_%u.ppm", dir, tier, scene, frame, surface);
    FILE *f = fopen(path, "wb");
    REQUIRE(f);
    fprintf(f, "P6\n240 135\n255\n");
    for (unsigned i = 0; i < KSN_PROC_W * KSN_PROC_H; ++i) {
        uint16_t c = px[i];
        unsigned char rgb[3] = {(unsigned char)((((c >> 11) & 31u) * 255u + 15u) / 31u),
            (unsigned char)((((c >> 5) & 63u) * 255u + 31u) / 63u),
            (unsigned char)(((c & 31u) * 255u + 15u) / 31u)};
        REQUIRE(fwrite(rgb, 1, 3, f) == 3);
    }
    REQUIRE(fclose(f) == 0);
}

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    REQUIRE(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    REQUIRE(s && fread(s, 1, (size_t)n, f) == (size_t)n);
    s[n] = 0; fclose(f); *size = (size_t)n;
    return s;
}

static const char *const OP_NAMES[] = {"SET", "INPUT", "ADD", "MUL", "SIN", "REPEAT", "END",
    "MOVE", "PLOT", "LINE", "REPEAT_REG", "BREAK_IF_GT", "PLOT_COLOR_REG", "LINE_COLOR_REG", "CUBIC"};

int main(int argc, char **argv) {
    REQUIRE(argc == 2 || argc == 3);
    const char *out = argc == 3 ? argv[2] : NULL;
    JSRuntime *rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    size_t n;
    char *src = read_file(argv[1], &n);
    JSValue r = JS_Eval(ctx, src, n, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "eval: %s\n", m ? m : "?");
        return 1;
    }
    JS_FreeValue(ctx, r); free(src);
    JSValue g = JS_GetGlobalObject(ctx);
    demo = JS_GetPropertyStr(ctx, g, "procMegademo");
    JS_FreeValue(ctx, g);
    JSValue names = prop(demo, "names"), lens = prop(demo, "len"), tiers_v = prop(demo, "tiers");
    const unsigned scenes = len(names), tiers = (unsigned)num(tiers_v);
    unsigned all_ops = 0, frames_checked = 0, draws_checked = 0, act1_frames = 0;
    unsigned long long pixels_checked = 0;
    printf("tier scene       frames plans draws seg_max seg_avg raster_max draw_raster_max "
           "draw_steps_max frame_steps_max instr_max regs_max depth_max inputs_max points_max "
           "point_lo point_hi\n");
    for (g_tier = 0; g_tier < tiers; ++g_tier) {
        for (g_scene = 0; g_scene < scenes; ++g_scene) {
            stats st = {.pt_lo = 32767, .pt_hi = -32768};
            JSValue a[3] = {JS_NewInt32(ctx, (int)g_scene), JS_NewInt32(ctx, (int)g_tier), JS_UNDEFINED};
            JSValue sp = call("specs", 2, a);
            st.plans = len(sp);
            REQUIRE(st.plans <= MAX_PLANS);
            for (unsigned i = 0; i < st.plans; ++i) {
                /* Each entry is a thunk; the app builds one plan at a time. */
                JSValue thunk = JS_GetPropertyUint32(ctx, sp, i);
                JSValue one = JS_Call(ctx, thunk, JS_UNDEFINED, 0, NULL);
                REQUIRE(!JS_IsException(one));
                load_spec(&specs[i], one, &st.ops_used);
                program_hash = fnv(program_hash, specs[i].code, specs[i].program.count * sizeof specs[i].code[0]);
                if (specs[i].has_points) program_hash = fnv(program_hash, specs[i].x, specs[i].points * 4u);
                JS_FreeValue(ctx, one); JS_FreeValue(ctx, thunk);
                if (specs[i].program.count > st.instr_max) st.instr_max = specs[i].program.count;
                if (specs[i].regs > st.regs_max) st.regs_max = specs[i].regs;
                if (specs[i].depth > st.depth_max) st.depth_max = specs[i].depth;
            }
            JS_FreeValue(ctx, sp);
            all_ops |= st.ops_used;
            const unsigned frames = (unsigned)at(lens, g_scene);
            for (g_frame = 0; g_frame < frames; ++g_frame) {
                a[2] = JS_NewInt32(ctx, (int)g_frame);
                JSValue fr = call("frame", 3, a);
                JSValue b = prop(fr, "b"), s = prop(fr, "s"), d = prop(fr, "d");
                const uint16_t backdrop = (uint16_t)ival(num(b));
                const unsigned surface = (unsigned)ival(num(s)), draws = len(d);
                REQUIRE(surface <= 1);
                memset(&cand_plan, 0, sizeof cand_plan);
                memset(&cand_vm, 0, sizeof cand_vm);
                unsigned frame_steps = 0;
                for (g_draw = 0; g_draw < draws; ++g_draw) {
                    JSValue one = JS_GetPropertyUint32(ctx, d, g_draw);
                    JSValue in = JS_GetPropertyUint32(ctx, one, 1);
                    const unsigned plan = (unsigned)ival(at(one, 0)), ni = len(in);
                    REQUIRE(plan < st.plans && ni <= KSN_PROC_INPUTS);
                    /* A program must not read an input the draw leaves zero-filled. */
                    REQUIRE(specs[plan].inputs_read <= ni || specs[plan].inputs_read == 0);
                    if (ni > st.inputs_max) st.inputs_max = ni;
                    float input[KSN_PROC_INPUTS] = {0};
                    for (unsigned i = 0; i < ni; ++i) {
                        input[i] = (float)at(in, i);
                        REQUIRE(isfinite(input[i]));
                    }
                    frame_steps += run_draw(&specs[plan], input, &st);
                    JS_FreeValue(ctx, in); JS_FreeValue(ctx, one);
                    ++draws_checked;
                }
                g_draw = 0;
                REQUIRE(cand_plan.count == cand_vm.count &&
                        !memcmp(cand_plan.segments, cand_vm.segments, cand_plan.count * sizeof cand_plan.segments[0]));
                cand_plan.ready = cand_vm.ready = true;
                for (unsigned i = 0; i < KSN_PROC_W * KSN_PROC_H; ++i) pix_plan[i] = pix_vm[i] = backdrop;
                REQUIRE(ksn_proc_render_band(&cand_plan, pix_plan, 0, KSN_PROC_H));
                /* The VM candidate is replayed in 8-row bands, as the image port does. */
                for (int y = 0; y < KSN_PROC_H; y += 8)
                    REQUIRE(ksn_proc_render_band(&cand_vm, pix_vm + y * KSN_PROC_W, y,
                                                 y + 8 <= KSN_PROC_H ? 8 : KSN_PROC_H - y));
                REQUIRE(!memcmp(pix_plan, pix_vm, sizeof pix_plan));
                pixel_hash = fnv(pixel_hash, pix_plan, sizeof pix_plan);
                pixels_checked += KSN_PROC_W * KSN_PROC_H;
                if (g_scene < 3) {
                    /* Act I must stay the 48-frame C reference, pixel for pixel. */
                    const unsigned tick = g_scene * 16 + g_frame;
                    REQUIRE(backdrop == proc_mega_backdrop(tick));
                    for (unsigned i = 0; i < KSN_PROC_W * KSN_PROC_H; ++i) pix_ref[i] = backdrop;
                    for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
                        ksn_proc_inst code[KSN_PROC_CODE]; ksn_proc_program p; float in[KSN_PROC_INPUTS];
                        ksn_proc_plan plan; ksn_proc_vm vm;
                        REQUIRE(proc_mega_build(tick, layer, code, &p, in));
                        REQUIRE(ksn_proc_plan_prepare(&plan, &p));
                        REQUIRE(ksn_proc_plan_begin(&vm, &plan, in, &f_plan) == KSN_PROC_RUNNING);
                        REQUIRE(ksn_proc_plan_run(&vm, &plan, false) == KSN_PROC_DONE);
                        REQUIRE(ksn_proc_render_band(&f_plan, pix_ref, 0, KSN_PROC_H));
                        if (layer == 3) {
                            const spec *s3 = &specs[3];
                            ksn_proc_frame typed = {.ready = true};
                            for (unsigned i = 1; i < s3->points; ++i)
                                typed.segments[typed.count++] = (ksn_proc_segment){s3->sx[i - 1], s3->sy[i - 1],
                                    s3->sx[i], s3->sy[i], s3->color};
                            REQUIRE(ksn_proc_render_band(&typed, pix_ref, 0, KSN_PROC_H));
                        }
                    }
                    REQUIRE(!memcmp(pix_ref, pix_plan, sizeof pix_ref));
                    ++act1_frames;
                }
                if (out) write_ppm(out, g_tier, g_scene, g_frame, surface, pix_plan);
                if (surface) ++st.surf1_frames;
                ++st.frames; ++frames_checked;
                if (draws > st.draws_max) st.draws_max = draws;
                if (cand_plan.count > st.seg_max) st.seg_max = cand_plan.count;
                st.seg_sum += cand_plan.count;
                if (cand_plan.raster_steps > st.raster_max) st.raster_max = cand_plan.raster_steps;
                if (frame_steps > st.frame_steps_max) st.frame_steps_max = frame_steps;
                st.steps_sum += frame_steps;
                JS_FreeValue(ctx, b); JS_FreeValue(ctx, s); JS_FreeValue(ctx, d); JS_FreeValue(ctx, fr);
            }
            JSValue nm = JS_GetPropertyUint32(ctx, names, g_scene);
            const char *name = JS_ToCString(ctx, nm);
            printf("%u %-7s s%u %6u %5u %5u %7u %7.1f %10u %15u %14u %15u %9u %8u %9u %10u %10u %8d %8d\n",
                   g_tier, name, g_scene, st.frames, st.plans, st.draws_max, st.seg_max,
                   (double)st.seg_sum / st.frames, st.raster_max, st.draw_raster_max, st.draw_steps_max,
                   st.frame_steps_max, st.instr_max, st.regs_max, st.depth_max, st.inputs_max,
                   st.points_max, st.points_max ? st.pt_lo : 0, st.points_max ? st.pt_hi : 0);
            if (getenv("MEGA_PLANS"))
                for (unsigned i = 0; i < st.plans; ++i)
                    printf("    plan %2u: instr %2u regs %2u depth %u inputs %u points %3u | max segs %4u raster %5u steps %5u\n",
                           i, specs[i].program.count, specs[i].regs, specs[i].depth, specs[i].inputs_read,
                           specs[i].points, specs[i].max_segs, specs[i].max_raster, specs[i].max_steps);
            JS_FreeCString(ctx, name); JS_FreeValue(ctx, nm);
        }
    }
    printf("ops used:");
    for (unsigned op = 0; op <= KSN_PROC_CUBIC; ++op)
        if (all_ops & (1u << op)) printf(" %s", OP_NAMES[op]);
    printf("\nops unused:");
    for (unsigned op = 0; op <= KSN_PROC_CUBIC; ++op)
        if (!(all_ops & (1u << op))) printf(" %s", OP_NAMES[op]);
    printf("\n");
    REQUIRE(all_ops == (1u << (KSN_PROC_CUBIC + 1)) - 1u);
    JS_FreeValue(ctx, names); JS_FreeValue(ctx, lens); JS_FreeValue(ctx, tiers_v);
    JS_FreeValue(ctx, demo);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf("hash programs %016llx pixels %016llx\n", (unsigned long long)program_hash, (unsigned long long)pixel_hash);
    printf("PASS megademo scenes: %u tiers, %u frames (%u Act I frames equal to proc_megademo.h), "
           "%u draws, plan == debug-step plan == single-step VM, scalar == PIE model == dispatcher "
           "points, %llu pixels compared, every ksn_proc_op used\n",
           tiers, frames_checked, act1_frames, draws_checked, pixels_checked);
    return 0;
}
