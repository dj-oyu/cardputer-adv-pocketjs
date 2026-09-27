#include "ksn_proc_compiler_device_probe.h"

#include "ksn_proc_plan.h"
#include "ksn_proc_points_dispatch.h"
#include "ksn_proc_points_pie.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
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

bool ksn_proc_compiler_device_probe_run(void)
{
    size_t free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    compiler_storage *s=heap_caps_calloc(1,sizeof *s,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!s) {
        ESP_LOGE(TAG,"ALLOC_FAIL bytes=%u free=%u",(unsigned)sizeof *s,(unsigned)free_before);
        return false;
    }
    bool ok=compiler_probe(s) && points_probe(s);
    heap_caps_free(s);
    ESP_LOGI(TAG,"%s heap_before=%u heap_after=%u min_free=%u stack_free=%u",
        ok?"PASS":"FAIL",(unsigned)free_before,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ok;
}
