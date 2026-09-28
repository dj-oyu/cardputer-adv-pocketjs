#include "ksn_proc_compiler_device_probe.h"

#include "ksn_proc_plan.h"
#include "ksn_proc_points_dispatch.h"
#include "ksn_proc_points_pie.h"
#include "ksn_proc_grid.h"
#include "ksn_proc_grid_pie.h"
#include "ksn_proc_grid_resize.h"
#include "ksn_grid_scan_cost_device_probe.h"
#include "quickjs.h"
#include "ksn_grid_fold_device_assets.h"

#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <limits.h>
#include <string.h>

#define TAG "KSN_COMPILER"
#define INST(OP,D,A,B,V,C) {OP,D,A,B,V,C}
#define POINT_CAP 48 /* Eight guard lanes remain after the largest test. */
#define POINT_GUARD ((int16_t)0x5a5a)

/* Arithmetic dependencies, a loop, drawing, and a seeded state exercise the
 * complete VM and frame rather than just the final register values. */
static const ksn_proc_inst code[] = {
    INST(KSN_PROC_INPUT,0,0,0,0,0),
    INST(KSN_PROC_SET,1,0,0,2,0),
    INST(KSN_PROC_SET,2,0,0,0.25f,0),
    INST(KSN_PROC_SET,3,0,0,33,0),
    INST(KSN_PROC_REPEAT,0,12,0,0,0),
      INST(KSN_PROC_MUL,4,0,2,0,0),
      INST(KSN_PROC_SIN,5,4,0,0,0),
      INST(KSN_PROC_MUL,5,5,3,0,0),
      INST(KSN_PROC_ADD,6,5,3,0,0),
      INST(KSN_PROC_LINE,0,0,6,0,0x07e0),
      INST(KSN_PROC_ADD,0,0,1,0,0),
    INST(KSN_PROC_END,0,0,0,0,0),
    INST(KSN_PROC_PLOT,0,0,6,0,0xf800)
};
static const ksn_proc_program program = {code, sizeof code / sizeof code[0]};

typedef struct {
    ksn_proc_vm reference, prepared;
    ksn_proc_frame reference_frame, prepared_frame;
    ksn_proc_plan plan;
} compiler_storage;

typedef struct __attribute__((aligned(16))) { int16_t v[POINT_CAP]; } point_plane;
static point_plane sx, sy, rx, ry, px, py;

static bool same_vm(const ksn_proc_vm *a, const ksn_proc_vm *b)
{
    if (a->depth > KSN_PROC_LOOP_DEPTH || b->depth > KSN_PROC_LOOP_DEPTH ||
        a->owned_program.count > KSN_PROC_CODE ||
        a->owned_program.count != b->owned_program.count ||
        a->program != &a->owned_program || b->program != &b->owned_program ||
        a->owned_program.code != a->owned_code ||
        b->owned_program.code != b->owned_code) return false;
    if (a->status != b->status || a->pc != b->pc || a->last_pc != b->last_pc ||
        a->depth != b->depth || a->steps != b->steps ||
        a->pen_x != b->pen_x || a->pen_y != b->pen_y || a->pen_valid != b->pen_valid ||
        memcmp(a->loop_end,b->loop_end,sizeof a->loop_end) ||
        memcmp(a->input,b->input,sizeof a->input) ||
        memcmp(a->reg,b->reg,sizeof a->reg)) return false;
    for (unsigned i=0;i<a->owned_program.count;i++) {
        const ksn_proc_inst *x=&a->owned_code[i], *y=&b->owned_code[i];
        if (x->op!=y->op || x->dst!=y->dst || x->a!=y->a || x->b!=y->b ||
            memcmp(&x->value,&y->value,sizeof x->value) || x->color!=y->color)
            return false;
    }
    for (unsigned i=0;i<a->depth;i++)
        if (a->loop[i].pc != b->loop[i].pc ||
            a->loop[i].remaining != b->loop[i].remaining) return false;
    return true;
}

static bool same_frame(const ksn_proc_frame *a, const ksn_proc_frame *b)
{
    if (a->count > KSN_PROC_SEGMENTS || a->count != b->count ||
        a->raster_steps != b->raster_steps ||
        a->ready != b->ready) return false;
    for (unsigned i=0;i<a->count;i++) {
        const ksn_proc_segment *x=&a->segments[i], *y=&b->segments[i];
        if (x->x0!=y->x0 || x->y0!=y->y0 || x->x1!=y->x1 ||
            x->y1!=y->y1 || x->color!=y->color) return false;
    }
    return true;
}

static bool same_state(const ksn_proc_state *a, const ksn_proc_state *b)
{
    return !memcmp(a->reg,b->reg,sizeof a->reg) && a->pen_x==b->pen_x &&
           a->pen_y==b->pen_y && a->pen_valid==b->pen_valid;
}

static bool cubic_probe(compiler_storage *s)
{
    static const ksn_proc_inst curve_code[]={
        INST(KSN_PROC_SET,0,0,0,10,0), INST(KSN_PROC_SET,1,0,0,10,0),
        INST(KSN_PROC_SET,2,0,0,35,0), INST(KSN_PROC_INPUT,3,0,0,0,0),
        INST(KSN_PROC_SET,4,0,0,75,0), INST(KSN_PROC_SET,5,0,0,95,0),
        INST(KSN_PROC_SET,6,0,0,100,0), INST(KSN_PROC_SET,7,0,0,10,0),
        INST(KSN_PROC_CUBIC,0,32,0,0,0x07e0)
    };
    const ksn_proc_program curve={curve_code,sizeof curve_code/sizeof curve_code[0]};
    const float input[KSN_PROC_INPUTS]={95,0,0,0};
    int64_t started=esp_timer_get_time();
    if(!ksn_proc_plan_prepare(&s->plan,&curve))return false;
    int64_t prepare_us=esp_timer_get_time()-started;
    if(ksn_proc_begin(&s->reference,&curve,input,&s->reference_frame)!=KSN_PROC_RUNNING||
       ksn_proc_plan_begin(&s->prepared,&s->plan,input,&s->prepared_frame)!=KSN_PROC_RUNNING)
        return false;
    started=esp_timer_get_time();
    if(ksn_proc_run(&s->reference)!=KSN_PROC_DONE)return false;
    int64_t scalar_us=esp_timer_get_time()-started;
    started=esp_timer_get_time();
    if(ksn_proc_plan_run(&s->prepared,&s->plan,false)!=KSN_PROC_DONE)return false;
    int64_t plan_us=esp_timer_get_time()-started;
    bool ok=same_frame(&s->reference_frame,&s->prepared_frame)&&
            s->prepared_frame.count==32&&s->prepared_frame.ready;
    ESP_LOGI(TAG,"CUBIC %s segments=%u prepare_us=%lld scalar_us=%lld plan_us=%lld",
             ok?"PASS":"FAIL",(unsigned)s->prepared_frame.count,
             (long long)prepare_us,(long long)scalar_us,(long long)plan_us);
    return ok;
}

static bool compiler_probe(compiler_storage *s)
{
    const float input[KSN_PROC_INPUTS]={18.5f,0,0,0};
    ksn_proc_state initial;
    ksn_proc_state_reset(&initial);
    initial.reg[7]=123.0f;
    initial.pen_x=9; initial.pen_y=11; initial.pen_valid=true;
    int64_t t=esp_timer_get_time();
    if (!ksn_proc_plan_prepare(&s->plan,&program) || !s->plan.fused_count) {
        ESP_LOGE(TAG,"PLAN prepare failed or found no dependent span"); return false;
    }
    int64_t prepare_us=esp_timer_get_time()-t;
    t=esp_timer_get_time();
    ksn_proc_status ref_begin=ksn_proc_begin_state(&s->reference,&program,input,&initial,&s->reference_frame);
    int64_t ref_begin_us=esp_timer_get_time()-t;
    t=esp_timer_get_time();
    ksn_proc_status plan_begin=ksn_proc_plan_begin_state(&s->prepared,&s->plan,input,&initial,&s->prepared_frame);
    int64_t plan_begin_us=esp_timer_get_time()-t;
    if (ref_begin!=KSN_PROC_RUNNING || plan_begin!=ref_begin) {
        ESP_LOGE(TAG,"BEGIN mismatch reference=%d plan=%d",ref_begin,plan_begin); return false;
    }
    t=esp_timer_get_time();
    ksn_proc_status ref_status=ksn_proc_run(&s->reference);
    int64_t ref_run_us=esp_timer_get_time()-t;
    t=esp_timer_get_time();
    ksn_proc_status plan_status=ksn_proc_plan_run(&s->prepared,&s->plan,false);
    int64_t plan_run_us=esp_timer_get_time()-t;
    ksn_proc_state ref_state,plan_state;
    if (ref_status!=KSN_PROC_DONE || plan_status!=ref_status ||
        !same_vm(&s->reference,&s->prepared) ||
        !same_frame(&s->reference_frame,&s->prepared_frame) ||
        !ksn_proc_capture_state(&s->reference,&ref_state) ||
        !ksn_proc_capture_state(&s->prepared,&plan_state) ||
        !same_state(&ref_state,&plan_state)) {
        ESP_LOGE(TAG,"VM mismatch status=%d/%d pc=%u/%u steps=%lu/%lu segments=%u/%u raster=%u/%u",
            ref_status,plan_status,s->reference.pc,s->prepared.pc,
            (unsigned long)s->reference.steps,(unsigned long)s->prepared.steps,
            s->reference_frame.count,s->prepared_frame.count,
            s->reference_frame.raster_steps,s->prepared_frame.raster_steps);
        return false;
    }
    ESP_LOGI(TAG,"VM PASS fused=%u segments=%u steps=%lu prepare_us=%lld ref_begin_us=%lld plan_begin_us=%lld ref_run_us=%lld plan_run_us=%lld",
        s->plan.fused_count,s->reference_frame.count,(unsigned long)s->reference.steps,
        (long long)prepare_us,(long long)ref_begin_us,(long long)plan_begin_us,
        (long long)ref_run_us,(long long)plan_run_us);
    return true;
}

static const int16_t point_edge[]={0,1,-1,32767,-32768,16383,-16384,30000,-30000};
static int16_t point_x(unsigned i)
{
    return i==0 ? INT16_MIN : i==1 ? 1 : point_edge[i%9];
}
static int16_t point_y(unsigned i)
{
    return i==0 ? INT16_MIN : i==1 ? 1 : point_edge[(i*5+3)%9];
}
static void point_input(void)
{
    for (unsigned i=0;i<POINT_CAP;i++) {
        sx.v[i]=point_x(i);
        sy.v[i]=point_y(i);
    }
}

static bool point_case(size_t n, unsigned alias, const KsnProcAffineQ14 *coeff,
                       unsigned coeff_index)
{
    point_input();
    for (unsigned i=0;i<POINT_CAP;i++) {
        rx.v[i]=ry.v[i]=px.v[i]=py.v[i]=POINT_GUARD;
    }
    if (alias&1) { memcpy(rx.v,sx.v,sizeof sx.v); memcpy(px.v,sx.v,sizeof sx.v); }
    if (alias&2) { memcpy(ry.v,sy.v,sizeof sy.v); memcpy(py.v,sy.v,sizeof sy.v); }
    KsnProcPointDst rd={rx.v,ry.v}, pd={px.v,py.v};
    KsnProcPointSrc rs={(alias&1)?rx.v:sx.v,(alias&2)?ry.v:sy.v};
    KsnProcPointSrc ps={(alias&1)?px.v:sx.v,(alias&2)?py.v:sy.v};
    ksn_proc_points_affine_scalar(rd,rs,n,coeff);
    ksn_proc_points_affine_pie(pd,ps,n,coeff);
    for (unsigned i=0;i<POINT_CAP;i++) {
        if (rx.v[i]!=px.v[i] || ry.v[i]!=py.v[i] ||
            (i>=n && (rx.v[i]!=((alias&1)?sx.v[i]:POINT_GUARD) ||
                      ry.v[i]!=((alias&2)?sy.v[i]:POINT_GUARD)))) {
            ESP_LOGE(TAG,"POINT mismatch coeff=%u n=%u alias=%u i=%u scalar=(%d,%d) pie=(%d,%d)",
                coeff_index,(unsigned)n,alias,i,rx.v[i],ry.v[i],px.v[i],py.v[i]);
            return false;
        }
    }
    for (unsigned i=0;i<POINT_CAP;i++) {
        if (sx.v[i]!=point_x(i) || sy.v[i]!=point_y(i)) {
            ESP_LOGE(TAG,"POINT source modified coeff=%u n=%u i=%u",coeff_index,(unsigned)n,i);
            return false;
        }
    }
    return true;
}

static volatile uint32_t point_checksum;
static int64_t timed_points(bool pie, size_t n, const KsnProcAffineQ14 *coeff)
{
    KsnProcPointDst dst={px.v,py.v};
    KsnProcPointSrc src={sx.v,sy.v};
    int64_t start=esp_timer_get_time();
    for (unsigned i=0;i<512;i++) {
        if (pie) ksn_proc_points_affine_pie(dst,src,n,coeff);
        else ksn_proc_points_affine_scalar(dst,src,n,coeff);
    }
    int64_t elapsed=esp_timer_get_time()-start;
    point_checksum+=(uint16_t)px.v[0]+(uint16_t)py.v[7];
    return elapsed;
}

static bool points_probe(compiler_storage *s)
{
    static const KsnProcAffineQ14 coeff[] = {
        {16384,0,0,16384,0,0},
        {32767,-32768,-32768,32767,INT32_MAX,INT32_MIN},
        {16383,-16383,-16383,16383,16383,-16383},
        {-32768,32767,32767,-32768,-INT32_C(1073741824),INT32_C(1073741824)},
        {-32768,-32768,8192,8192,INT32_MIN,0}
    };
    static const size_t lengths[]={0,1,7,8,9,10,11,12,13,14,15,16,31,40};
    if (!ksn_proc_points_pie_backend_available()) {
        ESP_LOGE(TAG,"PIE backend unavailable on this build"); return false;
    }
    for (unsigned c=0;c<sizeof coeff/sizeof coeff[0];c++)
        for (unsigned j=0;j<sizeof lengths/sizeof lengths[0];j++)
            for (unsigned alias=0;alias<4;alias++)
                if (!point_case(lengths[j],alias,&coeff[c],c)) return false;

    point_input();
    static const size_t timing_lengths[]={8,16,40};
    for (unsigned j=0;j<sizeof timing_lengths/sizeof timing_lengths[0];j++) {
        size_t n=timing_lengths[j];
        int64_t scalar_total=0,pie_total=0;
        /* Warm both paths; alternating order below reduces cache/order bias.
         * Wall time still includes interrupts and scheduling, so these totals
         * are observations, not an automatically calibrated policy. */
        ksn_proc_points_affine_scalar((KsnProcPointDst){px.v,py.v},
                                     (KsnProcPointSrc){sx.v,sy.v},n,&coeff[2]);
        ksn_proc_points_affine_pie((KsnProcPointDst){px.v,py.v},
                                  (KsnProcPointSrc){sx.v,sy.v},n,&coeff[2]);
        for (unsigned trial=0;trial<4;trial++) {
            if (trial&1) {
                pie_total+=timed_points(true,n,&coeff[2]);
                scalar_total+=timed_points(false,n,&coeff[2]);
            } else {
                scalar_total+=timed_points(false,n,&coeff[2]);
                pie_total+=timed_points(true,n,&coeff[2]);
            }
            vTaskDelay(1);
        }
        ESP_LOGI(TAG,"POINT_TIME n=%u calls_per_backend=%u scalar_us=%lld pie_us=%lld checksum=%lu",
            (unsigned)n,2048,(long long)scalar_total,(long long)pie_total,(unsigned long)point_checksum);
    }

    /* Enable selection only here, after all direct scalar/PIE comparisons.
     * This typed Q14 sidecar is registered separately from float IR. */
    point_input();
    const KsnProcPointsPolicy diagnostic_policy={true,8};
    if (!ksn_proc_plan_register_points_affine(&s->plan,&coeff[2],
                                               &diagnostic_policy)) {
        ESP_LOGE(TAG,"POINT registration failed"); return false;
    }
    KsnProcPointsDecision decision={KSN_PROC_POINTS_SCALAR,KSN_PROC_POINTS_EMPTY};
    if (!ksn_proc_plan_run_points_affine(&s->plan,
        (KsnProcPointDst){px.v,py.v},(KsnProcPointSrc){sx.v,sy.v},7,&decision) ||
        decision.backend!=KSN_PROC_POINTS_SCALAR ||
        decision.reason!=KSN_PROC_POINTS_BELOW_MIN_POINTS) {
        ESP_LOGE(TAG,"POINT short-batch fallback failed reason=%d",decision.reason);
        return false;
    }
    ESP_LOGI(TAG,"POINT_PLAN n=7 backend=%d reason=%d",
        decision.backend,decision.reason);
    if (!ksn_proc_plan_run_points_affine(&s->plan,
        (KsnProcPointDst){px.v,py.v},(KsnProcPointSrc){sx.v,sy.v},40,&decision)) {
        ESP_LOGE(TAG,"POINT plan dispatch failed"); return false;
    }
    if (decision.backend!=KSN_PROC_POINTS_PIE) {
        ESP_LOGE(TAG,"POINT plan did not select PIE reason=%d",decision.reason);
        return false;
    }
    ksn_proc_points_affine_scalar((KsnProcPointDst){rx.v,ry.v},
                                  (KsnProcPointSrc){sx.v,sy.v},40,&coeff[2]);
    if (memcmp(rx.v,px.v,40*sizeof rx.v[0]) ||
        memcmp(ry.v,py.v,40*sizeof ry.v[0])) {
        ESP_LOGE(TAG,"Diagnostic dispatch output mismatch"); return false;
    }
    ESP_LOGI(TAG,"POINT_PLAN n=40 backend=%d reason=%d",decision.backend,decision.reason);
    ESP_LOGI(TAG,"POINT PASS cases=%u diagnostic_plan=PIE",
        (unsigned)(sizeof coeff/sizeof coeff[0]*sizeof lengths/sizeof lengths[0]*4));
    return true;
}

typedef struct {
    ksn_grid_program program;
    ksn_grid_plan plan;
    ksn_grid_execution execution;
    int16_t source[32 * 24];
    int16_t weights[32 * 24];
    int16_t scalar[16 * 12] __attribute__((aligned(16)));
    int16_t lanes[16 * 12] __attribute__((aligned(16)));
    int16_t pie[16 * 12] __attribute__((aligned(16)));
    int16_t fused[16 * 12] __attribute__((aligned(16)));
    int16_t aligned_source[32 * 24] __attribute__((aligned(16)));
} grid_storage;

static ksn_grid_index grid_index(int32_t base, int32_t x, int32_t y,
                                 int32_t tx, int32_t ty)
{
    ksn_grid_index index = {0};
    const int32_t value[5] = {base, x, y, tx, ty};
    for (unsigned k = 0; k < 5; ++k) {
        index.term[k].constant = value[k];
        index.term[k].param = KSN_GRID_NO_PARAM;
    }
    return index;
}

static volatile uint32_t grid_checksum;


static bool grid_js_number(JSContext *ctx, JSValueConst value, int64_t *out)
{
    return JS_IsNumber(value) && JS_ToInt64(ctx, out, value) == 0;
}

static bool grid_js_field(JSContext *ctx, JSValueConst object,
                          const char *name, int64_t *out)
{
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    bool ok = !JS_IsException(value) && grid_js_number(ctx, value, out);
    JS_FreeValue(ctx, value);
    return ok;
}

static bool grid_js_index(JSContext *ctx, JSValueConst array,
                          ksn_grid_index *index)
{
    for (unsigned i = 0; i < 5; ++i) {
        JSValue term = JS_GetPropertyUint32(ctx, array, i);
        int64_t part[3];
        bool ok = !JS_IsException(term);
        for (unsigned k = 0; k < 3 && ok; ++k) {
            JSValue value = JS_GetPropertyUint32(ctx, term, k);
            ok = !JS_IsException(value) && grid_js_number(ctx, value, &part[k]);
            JS_FreeValue(ctx, value);
        }
        JS_FreeValue(ctx, term);
        if (!ok || part[0] < INT32_MIN || part[0] > INT32_MAX ||
            part[1] < INT16_MIN || part[1] > INT16_MAX ||
            part[2] < 0 || part[2] > UINT8_MAX) return false;
        index->term[i] = (ksn_grid_coeff){(int32_t)part[0],
                                           (int16_t)part[1], (uint8_t)part[2]};
    }
    return true;
}

static bool grid_js_decode(JSContext *ctx, JSValueConst object,
                           ksn_grid_program *program, ksn_grid_shape *shape)
{
    int64_t count, reg, shift, initial;
    if (!grid_js_field(ctx, object, "count", &count) || count < 1 ||
        count > KSN_GRID_CODE ||
        !grid_js_field(ctx, object, "result_reg", &reg) || reg < 0 || reg > 7 ||
        !grid_js_field(ctx, object, "final_shift", &shift) || shift < 0 || shift > 30 ||
        !grid_js_field(ctx, object, "initial", &initial) ||
        initial < INT32_MIN || initial > INT32_MAX) return false;
    program->count = (uint8_t)count;
    program->result_reg = (uint8_t)reg;
    program->final_shift = (uint8_t)shift;
    program->initial = initial;
    JSValue shape_js = JS_GetPropertyStr(ctx, object, "shape");
    JSValue output = JS_GetPropertyStr(ctx, object, "output");
    JSValue body = JS_GetPropertyStr(ctx, object, "body");
    bool ok = !JS_IsException(shape_js) && !JS_IsException(output) &&
              !JS_IsException(body) && grid_js_index(ctx, output, &program->output);
    uint16_t *dims[] = {&shape->width, &shape->height,
                        &shape->tap_width, &shape->tap_height};
    for (unsigned i = 0; i < 4 && ok; ++i) {
        JSValue value = JS_GetPropertyUint32(ctx, shape_js, i);
        int64_t n;
        ok = !JS_IsException(value) && grid_js_number(ctx, value, &n) &&
             n > 0 && n <= UINT16_MAX;
        if (ok) *dims[i] = (uint16_t)n;
        JS_FreeValue(ctx, value);
    }
    for (unsigned i = 0; i < program->count && ok; ++i) {
        JSValue instruction = JS_GetPropertyUint32(ctx, body, i);
        ksn_grid_instruction *in = &program->body[i];
        int64_t op, dst, a, b, buffer, immediate;
        ok = !JS_IsException(instruction) &&
             grid_js_field(ctx, instruction, "op", &op) &&
             grid_js_field(ctx, instruction, "dst", &dst) &&
             grid_js_field(ctx, instruction, "a", &a) &&
             grid_js_field(ctx, instruction, "b", &b) &&
             grid_js_field(ctx, instruction, "buffer", &buffer) &&
             grid_js_field(ctx, instruction, "immediate", &immediate) &&
             op >= 0 && op <= UINT8_MAX && dst >= 0 && dst < KSN_GRID_REGS &&
             a >= 0 && a < KSN_GRID_REGS && b >= 0 && b < KSN_GRID_REGS &&
             buffer >= 0 && buffer < KSN_GRID_BUFFERS &&
             immediate >= INT16_MIN && immediate <= INT16_MAX;
        if (ok) {
            in->op = (uint8_t)op; in->dst = (uint8_t)dst;
            in->a = (uint8_t)a; in->b = (uint8_t)b;
            in->buffer = (uint8_t)buffer;
            in->immediate = (int16_t)immediate;
            JSValue index = JS_GetPropertyStr(ctx, instruction, "index");
            ok = !JS_IsException(index) && grid_js_index(ctx, index, &in->index);
            JS_FreeValue(ctx, index);
        }
        JS_FreeValue(ctx, instruction);
    }
    JS_FreeValue(ctx, body);
    JS_FreeValue(ctx, output);
    JS_FreeValue(ctx, shape_js);
    return ok;
}

static bool grid_js_eval(JSContext *ctx, const char *source,
                         size_t length, const char *name)
{
    JSValue value = JS_Eval(ctx, source, length, name,
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        ESP_LOGE(TAG, "GRID_JS %s: %s", name, message ? message : "exception");
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, value);
        return false;
    }
    JS_FreeValue(ctx, value);
    return true;
}

static bool grid_js_probe(grid_storage *s)
{
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int64_t start = esp_timer_get_time();
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) return false;
    JSContext *ctx = JS_NewContext(runtime);
    if (!ctx) { JS_FreeRuntime(runtime); return false; }
    int64_t runtime_us = esp_timer_get_time() - start;
    bool ok = false;
    start = esp_timer_get_time();
    if (!grid_js_eval(ctx, grid_fold_js, sizeof grid_fold_js - 1u,
                      "grid_fold.js"))
        goto done;
    int64_t frontend_load_us = esp_timer_get_time() - start;
    start = esp_timer_get_time();
    if (!grid_js_eval(ctx, grid_fold_device_js,
                      sizeof grid_fold_device_js - 1u,
                      "grid_fold_device_probe.js")) goto done;
    int64_t frontend_compile_us = esp_timer_get_time() - start;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue spec = JS_GetPropertyStr(ctx, global, "gridFoldDeviceProgram");
    memset(&s->program, 0, sizeof s->program);
    ksn_grid_shape shape = {0};
    start = esp_timer_get_time();
    bool decoded = !JS_IsException(spec) &&
        grid_js_decode(ctx, spec, &s->program, &shape);
    int64_t decode_us = esp_timer_get_time() - start;
    JS_FreeValue(ctx, spec);
    JS_FreeValue(ctx, global);
    if (!decoded) goto done;
    start = esp_timer_get_time();
    if (ksn_grid_prepare(&s->program, &s->plan) != KSN_GRID_OK) goto done;
    int64_t prepare_us = esp_timer_get_time() - start;
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = s->source;
    binding.count[KSN_GRID_SOURCE] = 32u * 24u;
    binding.data[KSN_GRID_DEST] = s->scalar;
    binding.count[KSN_GRID_DEST] = 16u * 12u;
    binding.param[0] = 32; binding.param[1] = 16;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        ksn_grid_run_scalar(&s->execution) != KSN_GRID_OK) goto done;
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            int32_t sum = 0;
            for (unsigned ty = 0; ty < 2; ++ty)
                for (unsigned tx = 0; tx < 2; ++tx)
                    sum += s->source[(2 * y + ty) * 32 + 2 * x + tx];
            int32_t expected = sum / 4;
            if (sum < 0 && sum % 4) --expected;
            if (s->scalar[y * 16 + x] != expected) goto done;
        }
    ksn_grid_execution gather;
    binding.data[KSN_GRID_DEST] = s->lanes;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &gather) != KSN_GRID_OK)
        goto done;
    gather.requested_strategy = KSN_GRID_PIE_LOAD_GATHER;
    if (!ksn_grid_pie_select(&gather)) goto done;
    binding.data[KSN_GRID_DEST] = s->pie;
    start = esp_timer_get_time();
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        !ksn_grid_pie_select(&s->execution)) goto done;
    int64_t begin_select_us = esp_timer_get_time() - start;
    ksn_grid_pie_load_strategy selected = s->execution.selected_strategy;
    ksn_grid_selection_reason reason = s->execution.selection_reason;
    uint64_t key = s->execution.profile_key;
    const ksn_grid_pie_policy policy = {true, 8, 0, 0};
    int64_t auto_us = 0, gather_us = 0;
    for (unsigned trial = 0; trial < 4; ++trial) {
        for (unsigned order = 0; order < 2; ++order) {
            bool auto_arm = ((trial + order) & 1u) == 0;
            ksn_grid_execution *e = auto_arm ? &s->execution : &gather;
            if ((auto_arm ? ksn_grid_run_auto(e, &policy) : ksn_grid_run_pie(e)) !=
                KSN_GRID_OK) goto done;
            start = esp_timer_get_time();
            for (unsigned call = 0; call < 256; ++call)
                if ((auto_arm ? ksn_grid_run_auto(e, &policy) : ksn_grid_run_pie(e)) !=
                    KSN_GRID_OK) goto done;
            if (auto_arm) auto_us += esp_timer_get_time() - start;
            else gather_us += esp_timer_get_time() - start;
        }
        vTaskDelay(1);
    }
    if (memcmp(s->scalar, s->pie, sizeof s->scalar) ||
        memcmp(s->scalar, s->lanes, sizeof s->scalar) ||
        selected != KSN_GRID_PIE_LOAD_FUSED ||
        reason != KSN_GRID_SELECTION_PROFILE ||
        !s->execution.pie_backend_selected) goto done;
    ESP_LOGI(TAG, "GRID_JS_TIME runtime_us=%lld frontend_load_us=%lld frontend_compile_us=%lld decode_us=%lld prepare_us=%lld begin_select_us=%lld calls_per_backend=1024 gather_us=%lld auto_us=%lld key=%016llx selected=%u reason=%u free_before=%u free_after=%u",
             (long long)runtime_us, (long long)frontend_load_us,
             (long long)frontend_compile_us, (long long)decode_us,
             (long long)prepare_us, (long long)begin_select_us,
             (long long)gather_us, (long long)auto_us,
             (unsigned long long)key, (unsigned)selected, (unsigned)reason,
             (unsigned)free_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* Ordinary apps copy Int16Array inputs into 16-byte aligned native
     * storage. The earlier JS probe's source sat at +8 in grid_storage, so
     * its profile key cannot rank this production bind. Measure both legal
     * routes under the actual adapter alignment before adding a table row. */
    memcpy(s->aligned_source, s->source, sizeof s->aligned_source);
    binding.data[KSN_GRID_SOURCE] = s->aligned_source;
    binding.data[KSN_GRID_DEST] = s->lanes;
    ksn_grid_execution aligned_gather, aligned_fused;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &aligned_gather) != KSN_GRID_OK)
        goto done;
    aligned_gather.requested_strategy = KSN_GRID_PIE_LOAD_GATHER;
    binding.data[KSN_GRID_DEST] = s->fused;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &aligned_fused) != KSN_GRID_OK)
        goto done;
    aligned_fused.requested_strategy = KSN_GRID_PIE_LOAD_FUSED;
    if (ksn_grid_run_pie(&aligned_gather) != KSN_GRID_OK ||
        ksn_grid_run_pie(&aligned_fused) != KSN_GRID_OK ||
        memcmp(s->scalar, s->lanes, sizeof s->scalar) ||
        memcmp(s->scalar, s->fused, sizeof s->scalar)) goto done;
    int64_t aligned_gather_us = 0, aligned_fused_us = 0;
    for (unsigned trial = 0; trial < 4; ++trial) {
        for (unsigned order = 0; order < 2; ++order) {
            bool fused_arm = ((trial + order) & 1u) == 0;
            ksn_grid_execution *e = fused_arm ? &aligned_fused : &aligned_gather;
            start = esp_timer_get_time();
            for (unsigned call = 0; call < 256; ++call)
                if (ksn_grid_run_pie(e) != KSN_GRID_OK) goto done;
            if (fused_arm) aligned_fused_us += esp_timer_get_time() - start;
            else aligned_gather_us += esp_timer_get_time() - start;
        }
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "GRID_ALIGNED_TIME outputs=192 calls_per_backend=1024 source_align=%u dest_align=%u gather_us=%lld fused_us=%lld key=%016llx",
             (unsigned)((uintptr_t)s->aligned_source & 15u),
             (unsigned)((uintptr_t)s->fused & 15u),
             (long long)aligned_gather_us, (long long)aligned_fused_us,
             (unsigned long long)aligned_fused.profile_key);
    ok = true;
done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    if (!ok) ESP_LOGE(TAG, "GRID_JS FAIL free=%u", (unsigned)
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    else ESP_LOGI(TAG, "GRID_JS PASS free=%u", (unsigned)
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ok;
}

/* The same binary alternates scalar, gather, affine and fused-load PIE.
 * Bind and warm-up are excluded; each arm has the same output shape and calls. */
static bool grid_time_three(grid_storage *s, ksn_grid_shape shape,
                            ksn_grid_binding *binding, const char *label)
{
    int16_t *const output[] = {s->scalar, s->lanes, s->pie, s->fused};
    int64_t elapsed[4] = {0};
    bool ok = false;
    binding->data[KSN_GRID_DEST] = output[0];
    if (ksn_grid_begin(&s->plan, &shape, binding, &s->execution) != KSN_GRID_OK)
        return false;
    uint8_t candidates = ksn_grid_pie_candidates(&s->execution);
    unsigned arms = candidates & (1u << KSN_GRID_PIE_LOAD_FUSED) ? 4u : 3u;
    uint64_t key = ksn_grid_pie_profile_key(&s->execution);
    for (unsigned arm = 0; arm < arms; ++arm)
        memset(output[arm], 0x5a, sizeof s->scalar);
    for (unsigned trial = 0; trial < 4; ++trial) {
        for (unsigned order = 0; order < arms; ++order) {
            unsigned arm = (trial + order) % arms;
            binding->data[KSN_GRID_DEST] = output[arm];
            if (ksn_grid_begin(&s->plan, &shape, binding, &s->execution) !=
                    KSN_GRID_OK || !ksn_grid_pie_eligible(&s->execution))
                goto done;
            s->execution.requested_strategy = arm == 1 ?
                KSN_GRID_PIE_LOAD_GATHER : arm == 3 ?
                KSN_GRID_PIE_LOAD_FUSED : KSN_GRID_PIE_LOAD_AFFINE;
            ksn_grid_status (*run)(ksn_grid_execution *) =
                arm == 0 ? NULL : ksn_grid_run_pie;
            if ((run ? run(&s->execution) :
                       ksn_grid_run_scalar(&s->execution)) != KSN_GRID_OK)
                goto done;
            int64_t start = esp_timer_get_time();
            for (unsigned call = 0; call < 256; ++call)
                if ((run ? run(&s->execution) :
                           ksn_grid_run_scalar(&s->execution)) != KSN_GRID_OK)
                    goto done;
            elapsed[arm] += esp_timer_get_time() - start;
            grid_checksum += (uint16_t)output[arm][0];
        }
        vTaskDelay(1);
    }
    if (memcmp(s->scalar, s->lanes, sizeof s->scalar) ||
        memcmp(s->scalar, s->pie, sizeof s->scalar) ||
        (arms == 4 && memcmp(s->scalar, s->fused, sizeof s->scalar))) goto done;
    ESP_LOGI(TAG, "%s rev=%u key=%016llx mask=%u width=%u height=%u taps_x=%u taps_y=%u calls_per_backend=1024 scalar_us=%lld gather_us=%lld affine_us=%lld fused_us=%lld checksum=%lu",
             label, KSN_GRID_PIE_BACKEND_REV,
             (unsigned long long)key, (unsigned)candidates,
             (unsigned)shape.width, (unsigned)shape.height,
             (unsigned)shape.tap_width, (unsigned)shape.tap_height,
             (long long)elapsed[0], (long long)elapsed[1],
             (long long)elapsed[2], arms == 4 ? (long long)elapsed[3] : -1LL,
             (unsigned long)grid_checksum);
    ok = true;
done:
    return ok;
}

static bool grid_probe(void)
{
    void *raw = heap_caps_calloc(1, sizeof(grid_storage) + 15u,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw) {
        ESP_LOGE(TAG, "GRID alloc failed bytes=%u",
                 (unsigned)(sizeof(grid_storage) + 15u));
        return false;
    }
    grid_storage *s = (grid_storage *)(((uintptr_t)raw + 15u) & ~(uintptr_t)15u);
    bool ok = false;
    ksn_grid_program *p = &s->program;
    p->count = 4;
    p->result_reg = 0;
    p->final_shift = 2;
    p->body[0].op = KSN_GRID_LOAD;
    p->body[0].dst = 1;
    p->body[0].index = grid_index(0, 2, 0, 1, 0);
    p->body[0].index.term[2] = (ksn_grid_coeff){0, 2, 0};
    p->body[0].index.term[4] = (ksn_grid_coeff){0, 1, 0};
    p->body[1].op = KSN_GRID_CONST;
    p->body[1].dst = 2;
    p->body[1].immediate = 1;
    p->body[2].op = KSN_GRID_MUL;
    p->body[2].dst = 3;
    p->body[2].a = 1;
    p->body[2].b = 2;
    p->body[3].op = KSN_GRID_ADD;
    p->body[3].dst = 0;
    p->body[3].a = 0;
    p->body[3].b = 3;
    p->output = grid_index(0, 1, 0, 0, 0);
    p->output.term[2] = (ksn_grid_coeff){0, 1, 1};
    for (unsigned i = 0; i < 32u * 24u; ++i) {
        s->source[i] = (int16_t)((int32_t)((i * 73u + 19u) & 65535u) - 32768);
        s->weights[i] = (int16_t)((int32_t)((i * 163u + 97u) & 65535u) - 32768);
    }
    ksn_grid_shape shape = {16, 12, 2, 2};
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = s->source;
    binding.count[KSN_GRID_SOURCE] = 32u * 24u;
    binding.data[2] = s->weights;
    binding.count[2] = 32u * 24u;
    binding.data[KSN_GRID_DEST] = s->scalar;
    binding.count[KSN_GRID_DEST] = 16u * 12u;
    binding.param[0] = 32;
    binding.param[1] = 16;
    int64_t start = esp_timer_get_time();
    if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK) goto done;
    int64_t prepare_us = esp_timer_get_time() - start;
    memset(p, 0, sizeof *p); /* Prove that the plan owns its code. */
    start = esp_timer_get_time();
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK)
        goto done;
    int64_t begin_us = esp_timer_get_time() - start;
    if (!s->execution.safe || !s->execution.independent ||
        !s->execution.reduction_shape || !s->execution.qacc_legal ||
        !s->execution.pie_candidate || s->execution.pie_backend_selected)
        goto done;
    uint32_t validation_work = s->execution.validation_work;
    if (ksn_grid_run_scalar(&s->execution) != KSN_GRID_OK) goto done;
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            int32_t total = 0;
            for (unsigned ty = 0; ty < 2; ++ty)
                for (unsigned tx = 0; tx < 2; ++tx)
                    total += s->source[(2*y+ty)*32 + 2*x+tx];
            int32_t expected = total / 4;
            if (total < 0 && total % 4) --expected;
            if (s->scalar[y*16+x] != expected) {
                ESP_LOGE(TAG, "GRID reference mismatch x=%u y=%u got=%d expected=%ld",
                         x, y, s->scalar[y*16+x], (long)expected);
                goto done;
            }
        }
    binding.data[KSN_GRID_DEST] = s->lanes;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        !s->execution.pie_candidate ||
        ksn_grid_run_lanes_model(&s->execution) != KSN_GRID_OK ||
        memcmp(s->scalar, s->lanes, sizeof s->scalar)) goto done;
    binding.data[KSN_GRID_DEST] = s->pie;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        !ksn_grid_pie_backend_available() ||
        !ksn_grid_pie_eligible(&s->execution) ||
        ksn_grid_run_pie(&s->execution) != KSN_GRID_OK ||
        !s->execution.pie_backend_selected) goto done;
    for (unsigned i = 0; i < 16u * 12u; ++i)
        if (s->scalar[i] != s->pie[i]) {
            ESP_LOGE(TAG, "GRID PIE mismatch i=%u scalar=%d pie=%d",
                     i, s->scalar[i], s->pie[i]);
            goto done;
        }
    binding.data[KSN_GRID_DEST] = s->lanes;
    binding.count[KSN_GRID_DEST]--;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) !=
        KSN_GRID_BAD_INDEX) goto done;
    binding.count[KSN_GRID_DEST]++;
    binding.data[KSN_GRID_SOURCE] = s->lanes;
    binding.count[KSN_GRID_SOURCE] = 16u * 12u;
    shape = (ksn_grid_shape){1, 1, 1, 1};
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        !s->execution.safe || s->execution.independent) goto done;
    binding.data[KSN_GRID_SOURCE] = s->source;
    binding.count[KSN_GRID_SOURCE] = 32u * 24u;
    shape = (ksn_grid_shape){16, 12, 2, 2};
    *p = s->plan.program;
    static const uint16_t widths[] = {8, 9, 16};
    static const int16_t weights[] = {INT16_MIN, 1, INT16_MAX};
    static const uint8_t shifts[] = {0, 2, 14, 30};
    unsigned pie_cases = 0;
    for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; ++wi)
        for (unsigned co = 0; co < sizeof weights / sizeof weights[0]; ++co)
            for (unsigned sh = 0; sh < sizeof shifts / sizeof shifts[0]; ++sh)
                for (unsigned taps = 1; taps <= 2; ++taps) {
                    p->body[1].immediate = weights[co];
                    p->final_shift = shifts[sh];
                    shape = (ksn_grid_shape){widths[wi], 12, taps, taps};
                    if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK) goto done;
                    memset(s->scalar, 0x5a, sizeof s->scalar);
                    memset(s->pie, 0x5a, sizeof s->pie);
                    binding.data[KSN_GRID_DEST] = s->scalar;
                    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
                        ksn_grid_run_scalar(&s->execution) != KSN_GRID_OK) goto done;
                    binding.data[KSN_GRID_DEST] = s->pie;
                    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
                        !ksn_grid_pie_eligible(&s->execution) ||
                        ksn_grid_run_pie(&s->execution) != KSN_GRID_OK) goto done;
                    if (memcmp(s->scalar, s->pie, sizeof s->scalar)) {
                        ESP_LOGE(TAG, "GRID PIE edge mismatch width=%u weight=%d shift=%u taps=%u",
                                 widths[wi], weights[co], shifts[sh], taps);
                        goto done;
                    }
                    ++pie_cases;
                }
    p->body[1].immediate = 1;
    p->final_shift = 2;
    ksn_grid_program base = *p;
    for (unsigned variant = 0; variant < 6; ++variant)
        for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; ++wi)
            for (unsigned sh = 0; sh < sizeof shifts / sizeof shifts[0]; ++sh) {
                *p = base;
                p->final_shift = shifts[sh];
                if (variant == 0 || variant == 1 || variant == 3 || variant == 4) {
                    p->body[1].op = KSN_GRID_LOAD;
                    p->body[1].buffer = 2;
                    p->body[1].index = grid_index(0, 1, 16, 1, 32);
                    if (variant == 1) {
                        ksn_grid_instruction tmp = p->body[0];
                        p->body[0] = p->body[1];
                        p->body[1] = tmp;
                        p->body[3].a = 3;
                        p->body[3].b = 0;
                    }
                } else if (variant == 2) {
                    p->count = 2;
                    p->body[1] = p->body[3];
                    p->body[1].b = 1;
                }
                if (variant == 3) p->initial = 225000;
                if (variant == 4) p->initial = INT32_MIN;
                if (variant == 5) p->initial = -327680;
                shape = (ksn_grid_shape){widths[wi], 12, 2, 2};
                if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK ||
                    !s->plan.mac.valid) goto done;
                memset(s->scalar, 0x5a, sizeof s->scalar);
                memset(s->pie, 0x5a, sizeof s->pie);
                binding.data[KSN_GRID_DEST] = s->scalar;
                if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
                    ksn_grid_run_scalar(&s->execution) != KSN_GRID_OK) goto done;
                binding.data[KSN_GRID_DEST] = s->pie;
                if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
                    !ksn_grid_pie_eligible(&s->execution) ||
                    ksn_grid_run_pie(&s->execution) != KSN_GRID_OK ||
                    memcmp(s->scalar, s->pie, sizeof s->scalar)) {
                    ESP_LOGE(TAG, "GRID PIE normalized mismatch variant=%u width=%u shift=%u",
                             variant, widths[wi], shifts[sh]);
                    goto done;
                }
                ++pie_cases;
            }
    *p = base;
    shape = (ksn_grid_shape){16, 12, 2, 2};
    if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK) goto done;
    binding.data[KSN_GRID_DEST] = s->scalar;
    int64_t begin_total = 0, scalar_total = 0, lanes_total = 0, pie_total = 0;
    for (unsigned trial = 0; trial < 64; ++trial) {
        start = esp_timer_get_time();
        if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK)
            goto done;
        begin_total += esp_timer_get_time() - start;
        start = esp_timer_get_time();
        if (ksn_grid_run_scalar(&s->execution) != KSN_GRID_OK) goto done;
        scalar_total += esp_timer_get_time() - start;
        grid_checksum += (uint16_t)s->scalar[trial % (16u * 12u)];
        binding.data[KSN_GRID_DEST] = s->lanes;
        if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK)
            goto done;
        start = esp_timer_get_time();
        if (ksn_grid_run_lanes_model(&s->execution) != KSN_GRID_OK) goto done;
        lanes_total += esp_timer_get_time() - start;
        grid_checksum += (uint16_t)s->lanes[trial % (16u * 12u)];
        binding.data[KSN_GRID_DEST] = s->pie;
        if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK)
            goto done;
        start = esp_timer_get_time();
        if (ksn_grid_run_pie(&s->execution) != KSN_GRID_OK) goto done;
        pie_total += esp_timer_get_time() - start;
        grid_checksum += (uint16_t)s->pie[trial % (16u * 12u)];
        binding.data[KSN_GRID_DEST] = s->scalar;
    }
    static const struct { uint16_t width, height; } sizes[] = {
        {8, 1}, {9, 1}, {16, 1}, {8, 5}, {16, 12}
    };
    for (unsigned size = 0; size < sizeof sizes / sizeof sizes[0]; ++size) {
        shape = (ksn_grid_shape){sizes[size].width, sizes[size].height, 2, 2};
        if (!grid_time_three(s, shape, &binding, "GRID_TIME")) goto done;
    }
    shape = (ksn_grid_shape){16, 12, 2, 2};
    binding.data[KSN_GRID_DEST] = s->pie;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        !ksn_grid_pie_select(&s->execution)) goto done;
    ESP_LOGI(TAG, "GRID_PROFILE_FIXED key=%016llx reason=%u selected=%u output=%lld,%lld,%lld index=%lld,%lld,%lld,%lld,%lld",
             (unsigned long long)s->execution.profile_key,
             (unsigned)s->execution.selection_reason,
             (unsigned)s->execution.selected_strategy,
             (long long)s->execution.output[0], (long long)s->execution.output[1],
             (long long)s->execution.output[2],
             (long long)s->execution.index[0][0], (long long)s->execution.index[0][1],
             (long long)s->execution.index[0][2], (long long)s->execution.index[0][3],
             (long long)s->execution.index[0][4]);
    ksn_grid_pie_policy policy = {true, 8, 0, 0};
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        ksn_grid_run_auto(&s->execution, &policy) != KSN_GRID_OK ||
        !s->execution.pie_backend_selected ||
        memcmp(s->scalar, s->pie, sizeof s->scalar)) goto done;
    policy.min_outputs = 0;
    if (ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK ||
        ksn_grid_run_auto(&s->execution, &policy) != KSN_GRID_OK ||
        s->execution.pie_backend_selected) goto done;
    *p = base;
    p->body[1].op = KSN_GRID_LOAD;
    p->body[1].buffer = 2;
    p->body[1].index = grid_index(0, 1, 16, 1, 32);
    if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK) goto done;
    for (unsigned size = 0; size < sizeof sizes / sizeof sizes[0]; ++size) {
        shape = (ksn_grid_shape){sizes[size].width, sizes[size].height, 2, 2};
        if (!grid_time_three(s, shape, &binding, "GRID_DYNAMIC_TIME")) goto done;
    }
    *p = base;
    p->final_shift = 14;
    p->body[0].index = grid_index(0, 1, 32, 1, 32);
    p->body[1].op = KSN_GRID_LOAD;
    p->body[1].buffer = 2;
    p->body[1].index = grid_index(0, 0, 0, 1, 3);
    shape = (ksn_grid_shape){16, 12, 3, 3};
    if (ksn_grid_prepare(p, &s->plan) != KSN_GRID_OK ||
        ksn_grid_begin(&s->plan, &shape, &binding, &s->execution) != KSN_GRID_OK)
        goto done;
    s->execution.requested_strategy = KSN_GRID_PIE_LOAD_AFFINE;
    if (!ksn_grid_pie_select(&s->execution)) goto done;
    ksn_grid_pie_access_info access;
    if (!ksn_grid_pie_describe_access(&s->execution, &access) ||
        access.input != KSN_GRID_ACCESS_CONTIGUOUS ||
        access.coefficient != KSN_GRID_ACCESS_BROADCAST) goto done;
    if (!grid_time_three(s, shape, &binding, "GRID_BROADCAST_TIME")) goto done;
    if (!grid_js_probe(s)) goto done;
    ESP_LOGI(TAG, "GRID PASS outputs=%u taps=%u pie_cases=%u validation_work=%lu prepare_us=%lld begin_us=%lld begin64_us=%lld scalar64_us=%lld lanes64_us=%lld pie64_us=%lld checksum=%lu bytes=%u",
             16u * 12u, 4u, pie_cases, (unsigned long)validation_work,
             (long long)prepare_us, (long long)begin_us,
             (long long)begin_total, (long long)scalar_total,
             (long long)lanes_total, (long long)pie_total,
             (unsigned long)grid_checksum, (unsigned)(sizeof *s + 15u));
    ok = true;
done:
    if (!ok) ESP_LOGE(TAG, "GRID FAIL");
    heap_caps_free(raw);
    return ok;
}

static bool resize_probe(void)
{
    enum { SOURCE = 60 * 30, OUTPUT = 60 * 30, CALLS = 256 };
    ksn_grid_resize_plan *plan = heap_caps_malloc(sizeof *plan,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *source = heap_caps_malloc(SOURCE * sizeof *source,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *scalar = heap_caps_malloc(OUTPUT * sizeof *scalar,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pie = heap_caps_malloc(OUTPUT * sizeof *pie,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!plan || !source || !scalar || !pie) {
        heap_caps_free(plan); heap_caps_free(source);
        heap_caps_free(scalar); heap_caps_free(pie);
        return false;
    }
    for (unsigned i = 0; i < SOURCE; ++i) {
        unsigned v = (i * 13u + i / 60u * 7u) & 63u;
        source[i] = (int16_t)((((v >> 1) & 31u) << 11) |
                               (v << 5) | ((63u - v) >> 1));
    }
    static const struct { unsigned w, h; } sizes[] = {
        {8, 1}, {8, 4}, {16, 8}, {16, 16},
        {28, 14}, {37, 23}, {30, 15}
    };
    bool ok = true;
    for (unsigned size = 0; size < sizeof sizes / sizeof sizes[0]; ++size) {
        unsigned w = sizes[size].w, h = sizes[size].h;
        size_t count = (size_t)w * h;
        bool selected = false;
        if (!ksn_grid_resize_prepare(plan, 60, 30, w, h) ||
            !ksn_grid_resize_run(plan, source, SOURCE, scalar, count,
                                 false, &selected) || selected ||
            !ksn_grid_resize_run(plan, source, SOURCE, pie, count,
                                 true, &selected) || !selected ||
            memcmp(scalar, pie, count * sizeof *pie)) { ok = false; break; }
        int64_t start = esp_timer_get_time();
        for (unsigned i = 0; i < CALLS; ++i)
            if (!ksn_grid_resize_run(plan, source, SOURCE, scalar, count,
                                     false, NULL)) { ok = false; break; }
        int64_t scalar_us = esp_timer_get_time() - start;
        start = esp_timer_get_time();
        for (unsigned i = 0; i < CALLS; ++i)
            if (!ksn_grid_resize_run(plan, source, SOURCE, pie, count,
                                     true, NULL)) { ok = false; break; }
        int64_t pie_us = esp_timer_get_time() - start;
        if (!ok || memcmp(scalar, pie, count * sizeof *pie)) { ok = false; break; }
        ESP_LOGI(TAG, "GRID_RESIZE_TIME src=60x30 dst=%ux%u calls=%u scalar_us=%lld pie_us=%lld checksum=%u",
                 w, h, CALLS, (long long)scalar_us, (long long)pie_us,
                 (unsigned)(uint16_t)pie[count / 2]);
    }
    /* Same-binary A/B for the streaming kernel. Sixteen-pixel spans match
     * Kasane's ordinary image reader; full rows expose per-call overhead. */
    if (ok) {
        uint16_t rows[2][240], output[112], reference[112];
        for (unsigned i = 0; i < 240; ++i) {
            rows[0][i] = (uint16_t)((((i * 7u) & 31u) << 11) |
                                      (((i * 11u) & 63u) << 5) |
                                      ((i * 13u) & 31u));
            rows[1][i] = (uint16_t)(((((i + 3u) * 5u) & 31u) << 11) |
                                      ((((i + 1u) * 9u) & 63u) << 5) |
                                      (((i + 7u) * 3u) & 31u));
        }
        ok = ksn_grid_resize_prepare_stream(plan, 240, 135, 112, 63);
        for (unsigned fast = 0; ok && fast < 2; ++fast)
            for (unsigned shared = 0; ok && shared < 2; ++shared) {
                ksn_grid_resize_fast_weights = fast != 0;
                ksn_grid_resize_shared_qr = shared != 0;
                for (unsigned y = 0; ok && y < 63; ++y) {
                    ksn_grid_resize_fast_weights = false;
                    ksn_grid_resize_shared_qr = false;
                    ok = ksn_grid_resize_span(plan, y, 0, 112, rows[0],
                                              rows[1], reference, true, NULL);
                    ksn_grid_resize_fast_weights = fast != 0;
                    ksn_grid_resize_shared_qr = shared != 0;
                    ok = ok && ksn_grid_resize_span(plan, y, 0, 112, rows[0],
                                                     rows[1], output, true, NULL) &&
                         !memcmp(reference, output, sizeof output);
                }
                static const unsigned spans[] = {16, 32, 112};
                for (unsigned si = 0; ok && si < 3; ++si) {
                    unsigned span = spans[si];
                    int64_t start = esp_timer_get_time();
                    for (unsigned call = 0; ok && call < 32; ++call)
                        for (unsigned y = 0; ok && y < 63; ++y)
                            for (unsigned x = 0; ok && x < 112; x += span)
                                ok = ksn_grid_resize_span(plan, y, x,
                                       x + span <= 112 ? span : 112 - x,
                                       rows[0], rows[1], output + x, true, NULL);
                    int64_t elapsed = esp_timer_get_time() - start;
                    ESP_LOGI(TAG, "GRID_RESIZE_AB fast=%u shared=%u span=%u frames=32 us=%lld checksum=%u",
                             fast, shared, span, (long long)elapsed,
                             (unsigned)output[56]);
                }
            }
        ksn_grid_resize_routes routes;
        ksn_grid_resize_routes_read(&routes);
        ESP_LOGI(TAG, "GRID_RESIZE_AB_ROUTES flat=%u sparse=%u dense=%u",
                 (unsigned)routes.flat, (unsigned)routes.sparse,
                 (unsigned)routes.dense);
        ksn_grid_resize_fast_weights = false;
        ksn_grid_resize_shared_qr = false;
    }
    heap_caps_free(plan); heap_caps_free(source);
    heap_caps_free(scalar); heap_caps_free(pie);
    ESP_LOGI(TAG, "GRID_RESIZE %s", ok ? "PASS" : "FAIL");
    return ok;
}

bool ksn_proc_compiler_device_probe_run(void)
{
    char elf_sha[65];
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    static const char hex[] = "0123456789abcdef";
    const esp_app_desc_t *app = esp_app_get_description();
    for (unsigned i = 0; i < 32; ++i) {
        elf_sha[2 * i] = hex[app->app_elf_sha256[i] >> 4];
        elf_sha[2 * i + 1] = hex[app->app_elf_sha256[i] & 15u];
    }
    elf_sha[64] = '\0';
    ESP_LOGI(TAG, "GRID_PROFILE_DEVICE elf=%s cpu=%lu idf=%s chip=v%u.%u target=esp32s3 opt=%s",
             elf_sha, (unsigned long)(esp_rom_get_cpu_ticks_per_us() * 1000000u),
             app->idf_ver,
             (unsigned)(chip.revision / 100u), (unsigned)(chip.revision % 100u),
#ifdef CONFIG_COMPILER_OPTIMIZATION_SIZE
             "size");
#else
             "other");
#endif
    size_t free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    compiler_storage *s=heap_caps_calloc(1,sizeof *s,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!s) {
        ESP_LOGE(TAG,"ALLOC_FAIL bytes=%u free=%u",(unsigned)sizeof *s,(unsigned)free_before);
        return false;
    }
    bool ok=compiler_probe(s) && cubic_probe(s) && points_probe(s) && grid_probe() &&
            resize_probe() && ksn_grid_scan_cost_device_probe_run();
    heap_caps_free(s);
    ESP_LOGI(TAG,"%s heap_before=%u heap_after=%u min_free=%u stack_free=%u",
        ok?"PASS":"FAIL",(unsigned)free_before,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ok;
}
