/* Device-side measurements for the procedural limits relaxed in c53f661.
 * Built only with KASANE_PROC_LIMITS_PROBE=ON. Nothing here is on a shipping
 * path: the native half ('{') times the VM, the point kernels, the band
 * renderer and registration stack depth; the JS half ('}') runs
 * apps/kasane/proc_limits_probe.js, which needs heap, stack and clock readings
 * that pocket.* deliberately does not expose, hence the __lim global below.
 * Numbers are printed as KSN_LIMITS lines for tools/kasane_contract/
 * run_proc_limits_device.py; docs/kasane/procedural-limits-device.md records
 * what they meant. */
#include "ksn_proc_limits_device_probe.h"

#include "ksn_proc_plan.h"
#include "ksn_proc_points_dispatch.h"
#include "ksn_proc_points_pie.h"
#include "pocket_proc.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define TAG "KSN_LIMITS"
#define CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define INST(OP,D,A,B,V,C) {OP,D,A,B,V,C}
#define REPS 12u

static uint32_t ticks_per_us(void){ return esp_rom_get_cpu_ticks_per_us(); }

static int cmp_u32(const void *a,const void *b){
    uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;
    return x<y?-1:x>y;
}
/* Min is the least-disturbed run (the UI task can be preempted by Wi-Fi and
 * the LCD ISR); the median shows whether the min was a lucky outlier. */
static void summarize(uint32_t *v,unsigned n,uint32_t *min,uint32_t *med){
    qsort(v,n,sizeof *v,cmp_u32);*min=v[0];*med=v[n/2];
}

/* ~9,968 steps of pure arithmetic, no drawing: r0 counts, r3..r5 depend on
 * it so each pair has the def-use edge the plan fuses. The body op is MUL or
 * SIN; SIN is the most expensive single VM op, so the SIN form bounds a
 * draw's arithmetic time from above. */
static ksn_proc_inst arith_code[12];
static ksn_proc_program arith(uint8_t op,uint8_t outer){
    const ksn_proc_inst c[]={
        INST(KSN_PROC_SET,1,0,0,1,0), INST(KSN_PROC_SET,2,0,0,0.5f,0),
        INST(KSN_PROC_SET,0,0,0,0,0),
        INST(KSN_PROC_REPEAT,0,outer,0,0,0),
          INST(KSN_PROC_REPEAT,0,9,0,0,0),
            INST(KSN_PROC_ADD,0,0,1,0,0),
            INST(op,3,0,2,0,0),
            INST(KSN_PROC_ADD,4,3,1,0,0),
            INST(op,5,4,2,0,0),
          INST(KSN_PROC_END,0,0,0,0,0),
        INST(KSN_PROC_END,0,0,0,0,0),
    };
    memcpy(arith_code,c,sizeof c);
    return (ksn_proc_program){arith_code,(uint8_t)(sizeof c/sizeof c[0])};
}
/* docs/kasane/procedural-ir-experiment.md's wave: 706 steps, 99 segments. */
static const ksn_proc_inst wave_code[]={
    INST(KSN_PROC_SET,0,0,0,10,0), INST(KSN_PROC_SET,1,0,0,1,0),
    INST(KSN_PROC_INPUT,2,0,0,0,0), INST(KSN_PROC_SET,3,0,0,12,0),
    INST(KSN_PROC_SET,4,0,0,0.1f,0),
    INST(KSN_PROC_REPEAT,0,100,0,0,0),
      INST(KSN_PROC_MUL,5,0,4,0,0), INST(KSN_PROC_SIN,6,5,0,0,0),
      INST(KSN_PROC_MUL,6,6,3,0,0), INST(KSN_PROC_ADD,7,6,2,0,0),
      INST(KSN_PROC_LINE,0,0,7,0,0xffff), INST(KSN_PROC_ADD,0,0,1,0,0),
    INST(KSN_PROC_END,0,0,0,0,0),
};

typedef struct {
    ksn_proc_vm vm;
    ksn_proc_plan plan;
    ksn_proc_frame frame;
} vm_storage;

typedef enum { PATH_VM, PATH_PLAN, PATH_PLAN_DEBUG } run_path;
static const char *const path_name[]={"vm_step","plan","plan_debug"};

static bool run_once(vm_storage *s,const ksn_proc_program *p,run_path path,
                     const float in[KSN_PROC_INPUTS],uint32_t *cycles,
                     ksn_proc_status *status){
    uint32_t t0=esp_cpu_get_cycle_count();
    ksn_proc_status st;
    if(path==PATH_VM){
        st=ksn_proc_begin(&s->vm,p,in,&s->frame);
        if(st==KSN_PROC_RUNNING)st=ksn_proc_run(&s->vm);
    }else{
        st=ksn_proc_plan_begin(&s->vm,&s->plan,in,&s->frame);
        if(st==KSN_PROC_RUNNING)st=ksn_proc_plan_run(&s->vm,&s->plan,path==PATH_PLAN_DEBUG);
    }
    *cycles=esp_cpu_get_cycle_count()-t0;
    *status=st;
    return true;
}

static void time_program(vm_storage *s,const char *name,const ksn_proc_program *p,
                         const float in[KSN_PROC_INPUTS]){
    if(!ksn_proc_plan_prepare(&s->plan,p)){
        ESP_LOGE(TAG,"STEP %s prepare failed",name);return;
    }
    static const uint8_t sweeps[]={0,8,16};
    for(unsigned w=0;w<sizeof sweeps;w++){
        g_ksn_proc_sweep_regs=sweeps[w];
        for(unsigned path=0;path<3;path++){
            uint32_t v[REPS],min,med;ksn_proc_status st=KSN_PROC_INVALID;
            for(unsigned r=0;r<REPS;r++)run_once(s,p,(run_path)path,in,&v[r],&st);
            summarize(v,REPS,&min,&med);
            unsigned steps=(unsigned)s->vm.steps;
            ESP_LOGI(TAG,"STEP prog=%s sweep=%u path=%s status=%d steps=%u segs=%u "
                     "cyc_min=%u cyc_med=%u us_min=%u cyc_per_step_x100=%u fused=%u",
                     name,sweeps[w],path_name[path],(int)st,steps,
                     (unsigned)s->frame.count,(unsigned)min,(unsigned)med,
                     (unsigned)(min/ticks_per_us()),
                     steps?(unsigned)((uint64_t)min*100u/steps):0u,
                     (unsigned)s->plan.fused_count);
            /* Up to ~1 s per row on the UI task: let IDLE1 feed the task
             * watchdog, which otherwise fires mid-measurement. */
            vTaskDelay(1);
        }
    }
    g_ksn_proc_sweep_regs=0;
}

static void measure_steps(void){
    vm_storage *s=heap_caps_calloc(1,sizeof *s,CAPS);
    if(!s){ESP_LOGE(TAG,"STEP alloc failed");return;}
    const float in[KSN_PROC_INPUTS]={67};
    ksn_proc_program p=arith(KSN_PROC_MUL,212);
    time_program(s,"arith_mul",&p,in);
    p=arith(KSN_PROC_SIN,212);
    time_program(s,"arith_sin",&p,in);
    /* 255x... never finishes inside KSN_PROC_STEPS: the time to LIMIT is the
     * worst a single draw can spend in arithmetic. */
    p=arith(KSN_PROC_SIN,255);
    time_program(s,"limit_sin",&p,in);
    const ksn_proc_program wave={wave_code,sizeof wave_code/sizeof wave_code[0]};
    time_program(s,"wave",&wave,in);
    heap_caps_free(s);
}

/* ---- typed points: PIE vs scalar at the batch sizes the adapter allows ---- */
typedef struct __attribute__((aligned(16))) { int16_t v[128]; } plane128;
static plane128 sx,sy,ox_s,oy_s,ox_p,oy_p;

static void measure_points(void){
    uint32_t seed=0x1234567u;
    for(unsigned i=0;i<128;i++){
        seed=seed*1664525u+1013904223u;sx.v[i]=(int16_t)(seed>>16);
        seed=seed*1664525u+1013904223u;sy.v[i]=(int16_t)(seed>>16);
    }
    const KsnProcAffineQ14 coeff[]={
        {16384,0,0,16384,0,0},
        {15500,3000,-2500,16000,40*16384,10*16384},
        {-32768,32767,32767,-32768,INT32_MAX/4,INT32_MIN/4},
    };
    static const unsigned sizes[]={8,64,128};
    bool parity=true;
    for(unsigned c=0;c<sizeof coeff/sizeof coeff[0];c++){
        KsnProcPointSrc src={sx.v,sy.v};
        ksn_proc_points_affine_scalar((KsnProcPointDst){ox_s.v,oy_s.v},src,128,&coeff[c]);
        ksn_proc_points_affine_pie((KsnProcPointDst){ox_p.v,oy_p.v},src,128,&coeff[c]);
        parity&=!memcmp(ox_s.v,ox_p.v,sizeof ox_s.v)&&!memcmp(oy_s.v,oy_p.v,sizeof oy_s.v);
    }
    ESP_LOGI(TAG,"POINTS parity=%u cases=%u n=128",(unsigned)parity,
             (unsigned)(sizeof coeff/sizeof coeff[0]));
    const KsnProcPointsPolicy policy={true,8};
    for(unsigned k=0;k<sizeof sizes/sizeof sizes[0];k++){
        unsigned n=sizes[k];
        KsnProcPointSrc src={sx.v,sy.v};
        uint32_t t0=esp_cpu_get_cycle_count();
        for(unsigned r=0;r<2048;r++)
            ksn_proc_points_affine_scalar((KsnProcPointDst){ox_s.v,oy_s.v},src,n,&coeff[1]);
        uint32_t scalar=esp_cpu_get_cycle_count()-t0;
        t0=esp_cpu_get_cycle_count();
        for(unsigned r=0;r<2048;r++)
            ksn_proc_points_affine_pie((KsnProcPointDst){ox_p.v,oy_p.v},src,n,&coeff[1]);
        uint32_t pie=esp_cpu_get_cycle_count()-t0;
        KsnProcPointsDecision d=ksn_proc_points_affine_dispatch(
            (KsnProcPointDst){ox_p.v,oy_p.v},src,n,&coeff[1],&policy);
        ESP_LOGI(TAG,"POINTS n=%u reps=2048 scalar_us=%u pie_us=%u ratio_x100=%u "
                 "dispatch=%s",n,(unsigned)(scalar/ticks_per_us()),
                 (unsigned)(pie/ticks_per_us()),(unsigned)((uint64_t)scalar*100u/pie),
                 d.backend==KSN_PROC_POINTS_PIE?"PIE":"scalar");
    }
}

/* ---- band renderer: cost against the 1,024-segment and raster limits ---- */
static void add_seg(ksn_proc_frame *f,int x0,int y0,int x1,int y1){
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,
                                               (int16_t)x1,(int16_t)y1,0xffff};
    int dx=abs(x1-x0),dy=abs(y1-y0);
    f->raster_steps=(uint16_t)(f->raster_steps+(dx>dy?dx:dy)+1);
}
/* Full screen as the adapter paints it: 17 bands of 8 rows (the last 7). */
static uint32_t render_full(const ksn_proc_frame *f,uint16_t *band,int first_band){
    uint32_t t0=esp_cpu_get_cycle_count();
    for(int y=first_band*8;y<KSN_PROC_H;y+=8){
        int rows=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        ksn_proc_render_band(f,band,y,rows);
    }
    return esp_cpu_get_cycle_count()-t0;
}
static void time_render(const char *name,const ksn_proc_frame *f,uint16_t *band,int first){
    uint32_t v[REPS],min,med;
    for(unsigned r=0;r<REPS;r++)v[r]=render_full(f,band,first);
    summarize(v,REPS,&min,&med);
    ESP_LOGI(TAG,"RENDER frame=%s segs=%u raster=%u bands_from=%d us_min=%u us_med=%u",
             name,(unsigned)f->count,(unsigned)f->raster_steps,first,
             (unsigned)(min/ticks_per_us()),(unsigned)(med/ticks_per_us()));
}
static void measure_render(void){
    ksn_proc_frame *f=heap_caps_calloc(1,sizeof *f,CAPS);
    uint16_t *band=heap_caps_malloc(KSN_PROC_W*8*sizeof *band,CAPS);
    if(!f||!band){ESP_LOGE(TAG,"RENDER alloc failed");heap_caps_free(f);heap_caps_free(band);return;}
    f->ready=true;
    /* Rejection only: every segment lies in band 0 and bands 1..16 are drawn,
     * so the time is 16 passes of the per-segment row-range test. */
    for(unsigned i=0;i<KSN_PROC_SEGMENTS;i++)add_seg(f,(int)(i%240),0,(int)(i%240),0);
    time_render("reject1024",f,band,1);
    memset(f,0,sizeof *f);f->ready=true;
    for(unsigned i=0;i<256;i++){
        int x=(int)((i*37u)%232u),y=(int)((i*53u)%135u);add_seg(f,x,y,x+7,y);
    }
    time_render("short256",f,band,0);
    memset(f,0,sizeof *f);f->ready=true;
    for(unsigned i=0;i<KSN_PROC_SEGMENTS;i++){
        int x=(int)((i*37u)%232u),y=(int)((i*53u)%135u);add_seg(f,x,y,x+7,y);
    }
    time_render("short1024",f,band,0);
    /* ~8,192 raster steps: what one VM draw may emit at most. */
    memset(f,0,sizeof *f);f->ready=true;
    for(unsigned i=0;i<1024;i++){
        int x=(int)((i*37u)%233u),y=(int)((i*53u)%135u);add_seg(f,x,y,x+7,y);
        if(f->raster_steps>=8192-8)break;
    }
    time_render("draw_raster8192",f,band,0);
    /* 1,024 diagonals of 63 steps: 64,512 of the 65,535 frame steps. */
    memset(f,0,sizeof *f);f->ready=true;
    for(unsigned i=0;i<KSN_PROC_SEGMENTS;i++){
        int x=(int)((i*37u)%240u)-31,y=(int)((i*53u)%135u)-31;
        add_seg(f,x,y,x+62,y+62);
    }
    time_render("long1024",f,band,0);
    /* One 128-point batch of long lines, as a typed draw may emit with no
     * per-draw raster cap: 127 segments spanning -480..720. */
    memset(f,0,sizeof *f);f->ready=true;
    for(unsigned i=0;i<127;i++){
        int x0=(i&1)?720:-480,y0=-480+(int)(i*9u),x1=(i&1)?-480:720,y1=y0+9;
        add_seg(f,x0,y0,x1,y1);
    }
    time_render("points127_long",f,band,0);
    heap_caps_free(f);heap_caps_free(band);
}

/* ---- registration stack depth, in a fresh task so the high-water mark is
 * this call alone ---- */
typedef struct {
    int mode;
    ksn_proc_plan *plan;
    const ksn_proc_program *program;
    UBaseType_t hwm;
    SemaphoreHandle_t done;
} stack_job;
#define STACK_TASK 12288
static void stack_task(void *arg){
    stack_job *j=arg;
    if(j->mode==1)(void)ksn_proc_plan_prepare(j->plan,j->program);
    j->hwm=uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}
static unsigned stack_used(int mode,ksn_proc_plan *plan,const ksn_proc_program *p){
    stack_job j={mode,plan,p,0,xSemaphoreCreateBinary()};
    if(!j.done)return 0;
    if(xTaskCreate(stack_task,"lim_stack",STACK_TASK,&j,5,NULL)!=pdPASS){
        vSemaphoreDelete(j.done);return 0;
    }
    xSemaphoreTake(j.done,portMAX_DELAY);
    vSemaphoreDelete(j.done);
    vTaskDelay(1); /* let the idle task reap the TCB before the next heap read */
    return STACK_TASK-(unsigned)j.hwm;
}
static void measure_stack(void){
    ksn_proc_plan *plan=heap_caps_calloc(1,sizeof *plan,CAPS);
    ksn_proc_inst *code=heap_caps_calloc(KSN_PROC_CODE,sizeof *code,CAPS);
    if(!plan||!code){heap_caps_free(plan);heap_caps_free(code);return;}
    /* 64 instructions with the deepest legal nesting and every register. */
    unsigned n=0;
    for(unsigned i=0;i<KSN_PROC_LOOP_DEPTH;i++)code[n++]=(ksn_proc_inst)INST(KSN_PROC_REPEAT,0,2,0,0,0);
    while(n<KSN_PROC_CODE-KSN_PROC_LOOP_DEPTH){
        code[n]=(ksn_proc_inst)INST(KSN_PROC_ADD,(uint8_t)(n%16),(uint8_t)((n+1)%16),(uint8_t)((n+5)%16),0,0);
        n++;
    }
    for(unsigned i=0;i<KSN_PROC_LOOP_DEPTH;i++)code[n++]=(ksn_proc_inst)INST(KSN_PROC_END,0,0,0,0,0);
    const ksn_proc_program p64={code,(uint8_t)n};
    const ksn_proc_program wave={wave_code,sizeof wave_code/sizeof wave_code[0]};
    unsigned base=stack_used(0,plan,&p64);
    unsigned u64=stack_used(1,plan,&p64);
    bool ok64=plan->valid;
    unsigned uw=stack_used(1,plan,&wave);
    uint32_t t0=esp_cpu_get_cycle_count();
    bool ok=ksn_proc_plan_prepare(plan,&p64);
    uint32_t prep=esp_cpu_get_cycle_count()-t0;
    ESP_LOGI(TAG,"STACK task=%u base_used=%u prepare64_used=%u prepare_wave_used=%u "
             "prepare64_delta=%u valid=%u prepare64_us=%u sizeof_analysis=%u",
             STACK_TASK,base,u64,uw,u64>base?u64-base:0,(unsigned)(ok64&&ok),
             (unsigned)(prep/ticks_per_us()),(unsigned)sizeof(ksn_proc_analysis));
    heap_caps_free(plan);heap_caps_free(code);
}

void ksn_proc_limits_device_probe_run(void){
    size_t free0=heap_caps_get_free_size(CAPS),large0=heap_caps_get_largest_free_block(CAPS);
    ESP_LOGI(TAG,"START free=%u largest=%u sizeof_plan=%u sizeof_vm=%u sizeof_frame=%u "
             "sizeof_inst=%u pie=%u ticks_per_us=%u",
             (unsigned)free0,(unsigned)large0,(unsigned)sizeof(ksn_proc_plan),
             (unsigned)sizeof(ksn_proc_vm),(unsigned)sizeof(ksn_proc_frame),
             (unsigned)sizeof(ksn_proc_inst),(unsigned)ksn_proc_points_pie_backend_available(),
             (unsigned)ticks_per_us());
    measure_steps();
    measure_points();
    measure_render();
    measure_stack();
    ESP_LOGI(TAG,"END free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(CAPS),
             (unsigned)heap_caps_get_largest_free_block(CAPS));
}

/* ---- __lim: readings the JS half cannot get from pocket.* ---- */
static JSValue lim_mem(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    JSValue a=JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx,a,0,JS_NewInt64(ctx,(int64_t)heap_caps_get_free_size(CAPS)));
    JS_SetPropertyUint32(ctx,a,1,JS_NewInt64(ctx,(int64_t)heap_caps_get_largest_free_block(CAPS)));
    JS_SetPropertyUint32(ctx,a,2,JS_NewInt64(ctx,(int64_t)uxTaskGetStackHighWaterMark(NULL)));
    JS_SetPropertyUint32(ctx,a,3,JS_NewInt64(ctx,(int64_t)heap_caps_get_minimum_free_size(CAPS)));
    /* The guest's own share, so a free-heap delta can be split into JS and
     * native (plan/point) bytes. */
    JSMemoryUsage u;JS_ComputeMemoryUsage(JS_GetRuntime(ctx),&u);
    JS_SetPropertyUint32(ctx,a,4,JS_NewInt64(ctx,u.malloc_size));
    return a;
}
static JSValue lim_us(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    return JS_NewFloat64(ctx,(double)esp_timer_get_time());
}
static JSValue lim_counts(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    uint32_t scalar=0,pie=0;pocket_proc_batch_counts(&scalar,&pie);
    JSValue a=JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx,a,0,JS_NewInt64(ctx,scalar));
    JS_SetPropertyUint32(ctx,a,1,JS_NewInt64(ctx,pie));
    return a;
}
static JSValue lim_gc(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
/* FNV-1a over the surface-0 frame as the backdrop path renders it (pending
 * candidate first), little-endian bytes: the host reference hashes the same. */
static JSValue lim_hash(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    uint16_t *band=heap_caps_malloc(KSN_PROC_W*8*sizeof *band,CAPS);
    if(!band)return JS_NewInt32(ctx,-1);
    uint32_t h=2166136261u;
    for(unsigned y=0;y<KSN_PROC_H;y+=8){
        unsigned rows=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        if(pocket_proc_backdrop(NULL,(uint16_t)y,(uint16_t)rows,band)!=KSN_OK){
            heap_caps_free(band);return JS_NewInt32(ctx,-2);
        }
        for(unsigned i=0;i<rows*KSN_PROC_W;i++){
            h=(h^(band[i]&255u))*16777619u;
            h=(h^(band[i]>>8))*16777619u;
        }
    }
    heap_caps_free(band);
    return JS_NewInt64(ctx,h);
}
static JSValue lim_log(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *s=argc?JS_ToCString(ctx,argv[0]):NULL;
    if(s){ESP_LOGI(TAG,"JS %s",s);JS_FreeCString(ctx,s);}
    return JS_UNDEFINED;
}
esp_err_t ksn_proc_limits_js_install(JSContext *ctx,void *user_data){
    (void)user_data;
    static const JSCFunctionListEntry fns[]={
        JS_CFUNC_DEF("mem",0,lim_mem), JS_CFUNC_DEF("us",0,lim_us),
        JS_CFUNC_DEF("counts",0,lim_counts), JS_CFUNC_DEF("gc",0,lim_gc),
        JS_CFUNC_DEF("hash",0,lim_hash), JS_CFUNC_DEF("log",1,lim_log),
    };
    JSValue o=JS_NewObject(ctx);
    if(JS_IsException(o))return ESP_ERR_NO_MEM;
    if(JS_SetPropertyFunctionList(ctx,o,fns,(int)(sizeof fns/sizeof fns[0]))<0){
        JS_FreeValue(ctx,o);return ESP_ERR_NO_MEM;
    }
    JSValue g=JS_GetGlobalObject(ctx);
    int r=JS_SetPropertyStr(ctx,g,"__lim",o);
    JS_FreeValue(ctx,g);
    return r<0?ESP_ERR_NO_MEM:ESP_OK;
}
