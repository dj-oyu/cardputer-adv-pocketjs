/* apps/bigwave/big_wave.js as the firmware runs it, on the host: the real
 * QuickJS, the real pocket.kasane (view + procedural) and the real
 * pocket.input.keys, with key edges fed through the real keymap_poll() and
 * keystate, and the real renderer presenting into a 240x135 panel after every
 * frame(). No device, no serial port.
 *
 * What it proves before a flash:
 *  - a scripted play (title, start, E/S/A/D, carve, pop, tuck into the tube,
 *    the lip, a rock, pause, game over, retry; popping a rock and another
 *    rider, hitting a rider, a breaking section tucked and standing, a bad
 *    landing) and a closed-loop bot that clears all three sets with keys
 *    alone; every physical claim is checked against the game state (speed up
 *    under E, climbing under A, ...), with a fixed seed so every run is
 *    identical. Obstacle cases place the rider by writing the state (setup);
 *    the action is always keys, and the bot only reads;
 *  - every procedural draw of every frame runs three ways that must agree bit
 *    for bit (the plan pocket_proc.c runs, the plan in debug-step mode, the
 *    single-step VM), and the frame rendered from the plan and from the VM
 *    segments is compared pixel for pixel; typed points run scalar, the PIE
 *    model and the dispatcher;
 *  - per-frame load (segments, raster steps, VM steps, instructions, plans,
 *    points) against the adapter's limits, per game state and per load tier;
 *  - the guest heap after evaluation and its peak over the run (TLSF model);
 *  - the Back turn (frame(0x2000)) returns without drawing or throwing.
 * With BW_PPM=<dir> the composited panel is written at the labelled moments.
 *
 *   python3 tools/games/run_big_wave.py      (WSL)
 */
#include "pocket_kasane.h"
#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "keymap.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_render.h"
#include "ui/kasane/ksn_proc_plan.h"
#include "ui/kasane/ksn_proc_points_pie.h"
#include "text/ksn_font.h"
#include "sound.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Generated verbatim from pocket_api.c: production subscription semantics. */
#include "pocket_sub_impl.inc"

/* ---- guest allocator charged like the device's TLSF (see test_megademo_app_host.c) */
typedef struct { size_t n; size_t pad[3]; } pk_hdr;
static size_t cur_bytes,peak_bytes;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;pk_count(tlsf_len(n),0);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;pk_count(0,tlsf_len(h->n));free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    q->n=n;pk_count(tlsf_len(n),tlsf_len(old));return q+1;}
static size_t pk_usable(const void *p){return p?tlsf_len(((const pk_hdr *)p-1)->n):0;}
static const JSMallocFunctions PEAK_MF={pk_calloc,pk_malloc,pk_free,pk_realloc,pk_usable};

/* ---- host stubs: surfaces the game does not use (as test_keytest_app.c) */
void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr){__real_free(ptr);}
void *__wrap_calloc(size_t count,size_t size){return __real_calloc(count,size);}
bool pocket_text_active(void){return false;}
int64_t esp_timer_get_time(void){return 0;}
esp_err_t pocket_grid_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_grid_reset(void){}
bool pocket_grid_pending(void){return false;}
void pocket_grid_present_result(ksn_result r){(void)r;}
void pocket_grid_source_invalidated(uint32_t id){(void)id;}
esp_err_t pocket_video_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_video_reset(void){}
bool pocket_video_pending(void){return false;}
void pocket_video_present_result(ksn_result r){(void)r;}
esp_err_t pocket_pixel_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_pixel_reset(void){}
bool pocket_pixel_pending(void){return false;}
void pocket_pixel_present_result(ksn_result r){(void)r;}

/* ---- keyboard: one FIFO event per keymap_poll(), as the input task reads it */
static board_keyevent_t fifo[8];
static unsigned fifo_n,fifo_at;
bool board_key_event(board_keyevent_t *out){
    if(fifo_at==fifo_n){fifo_n=fifo_at=0;return false;}
    *out=fifo[fifo_at++];return true;
}
static bool down_now[56];
static void edge(const char *name,bool pressed){
    int i=keymap_key_index(name);
    if(i<0){printf("unknown key %s\n",name);exit(2);}
    if(down_now[i]==pressed)return;
    down_now[i]=pressed;
    if(fifo_at==fifo_n)fifo_n=fifo_at=0;
    fifo[fifo_n++]=(board_keyevent_t){.row=(uint8_t)(i/14),.col=(uint8_t)(i%14),.pressed=pressed};
    keystroke_t k;keymap_poll(&k);
}
/* Holds exactly the keys in `keys` (space separated), releasing the rest. */
static const char *const GAME_KEYS[]={"e","s","a","d",";",",",".","/","1","2","3","4","tab"};
static void hold(const char *keys){
    for(unsigned g=0;g<sizeof GAME_KEYS/sizeof GAME_KEYS[0];g++){
        const char *k=GAME_KEYS[g];size_t n=strlen(k);bool want=false;
        for(const char *p=keys;p&&*p;){
            while(*p==' ')p++;
            const char *q=p;while(*q&&*q!=' ')q++;
            if((size_t)(q-p)==n&&!strncmp(p,k,n))want=true;
            p=q;
        }
        edge(k,want);
    }
}

/* ---- the guest */
static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8],panel[240*135];
static unsigned exceptions,present_failures,frame_no;
static char last_log[256];
static unsigned wipes[8],tube_logs,clears,set_clears,overs,backs,readys;
static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s)return JS_UNDEFINED;
    printf("  [%5u] %s\n",frame_no,s);
    snprintf(last_log,sizeof last_log,"%s",s);
    static const char *const WHY[]={"LIP","ROCK","RIDER","CAUGHT","LAND"};
    if(!strncmp(s,"BIGWAVE WIPE ",13))
        for(unsigned i=0;i<5;i++)if(!strncmp(s+13,WHY[i],strlen(WHY[i])))wipes[i]++;
    if(!strncmp(s,"BIGWAVE TUBE ",13))tube_logs++;
    if(!strncmp(s,"BIGWAVE CLEAR",13))clears++;
    if(!strncmp(s,"BIGWAVE SET ",12))set_clears++;
    if(!strncmp(s,"BIGWAVE OVER",12))overs++;
    if(!strncmp(s,"BIGWAVE BACK",12))backs++;
    if(!strncmp(s,"BIGWAVE READY",13))readys++;
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static unsigned tones;
static JSValue js_tone(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;tones++;
    return JS_Eval(c,"Promise.resolve()",17,"tone.js",JS_EVAL_TYPE_GLOBAL);
}
static bool eval(const char *source,size_t len,const char *name){
    JSValue v=JS_Eval(ctx,source,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok){
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("  %s threw at frame %u: %s\n",name,frame_no,t?t:"?");
        if(t)JS_FreeCString(ctx,t);
        JSValue st=JS_GetPropertyStr(ctx,e,"stack");
        const char *s=JS_IsString(st)?JS_ToCString(ctx,st):NULL;
        if(s){printf("%s",s);JS_FreeCString(ctx,s);}
        JS_FreeValue(ctx,st);JS_FreeValue(ctx,e);
        exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0){}
    pocket_kasane_end_turn();
    return ok;
}
static double num(const char *expr){
    JSValue v=JS_Eval(ctx,expr,strlen(expr),"probe.js",JS_EVAL_TYPE_GLOBAL);
    double d=NAN;
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}
    else JS_ToFloat64(ctx,&d,v);
    JS_FreeValue(ctx,v);return d;
}
static void run(const char *expr){eval(expr,strlen(expr),"setup.js");}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)o;memcpy(panel+y*240,pixels,rows*240*sizeof *pixels);return KSN_OK;
}
static void present(void){
    uint64_t now=(uint64_t)frame_no*33333u;
    pocket_kasane_set_animation_time(now);
    (void)pocket_kasane_advance(now);
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    ksn_result r=pocket_kasane_present(&port,&stats);
    if(r!=KSN_OK)present_failures++;
    else pocket_kasane_animations_presented(now);
}
static const char *ppm_dir;
static void ppm(const char *label){
    if(!ppm_dir)return;
    char name[512];snprintf(name,sizeof name,"%s/%s.ppm",ppm_dir,label);
    FILE *o=fopen(name,"wb");if(!o)return;
    fprintf(o,"P6\n240 135\n255\n");
    for(size_t i=0;i<240*135;i++){
        uint16_t p=panel[i];
        unsigned char rgb[3]={(unsigned char)((((p>>11)&31u)*255u+15u)/31u),
            (unsigned char)((((p>>5)&63u)*255u+31u)/63u),(unsigned char)(((p&31u)*255u+15u)/31u)};
        fwrite(rgb,1,3,o);
    }
    fclose(o);
    printf("  [%5u] PPM %s\n",frame_no,label);
}

/* ---- the differential oracle (the test_proc_megademo_scenes.c discipline) */
#define MAX_POINTS 128u
typedef struct {
    int32_t handle;
    char key[8];
    ksn_proc_inst code[KSN_PROC_CODE];
    ksn_proc_program program;
    ksn_proc_plan plan;
    bool has_points;
    unsigned points,regs,depth,inputs_read;
    uint16_t color;
    KsnProcAffineQ14 coeff;
    _Alignas(16) int16_t x[MAX_POINTS],y[MAX_POINTS];
    _Alignas(16) int16_t sx[MAX_POINTS],sy[MAX_POINTS],px[MAX_POINTS],py[MAX_POINTS],dx[MAX_POINTS],dy[MAX_POINTS];
    unsigned max_segs,max_raster,max_steps,draws;
} spec;
static spec *specs[256];
static unsigned spec_n,live_plans,live_peak,registered,unregistered;
static ksn_proc_frame f_plan,f_debug,f_vm,cand_plan,cand_vm;
static uint16_t pix_plan[KSN_PROC_W*KSN_PROC_H],pix_vm[KSN_PROC_W*KSN_PROC_H];
static unsigned oracle_failures;
#define CHECK(t) do{if(!(t)){printf("ORACLE FAIL frame %u: %s (%s:%d)\n",frame_no,#t,__FILE__,__LINE__);oracle_failures++;return 0;}}while(0)

static double at(JSValueConst a,unsigned i){
    JSValue v=JS_GetPropertyUint32(ctx,a,i);double d=NAN;JS_ToFloat64(ctx,&d,v);JS_FreeValue(ctx,v);return d;
}
static unsigned len(JSValueConst a){
    JSValue v=JS_GetPropertyStr(ctx,a,"length");double d=0;JS_ToFloat64(ctx,&d,v);JS_FreeValue(ctx,v);return (unsigned)d;
}
static void analyse(spec *s,unsigned *ops){
    uint16_t used=0;unsigned depth=0,maxd=0,inputs=0;
    for(unsigned i=0;i<s->program.count;++i){
        const ksn_proc_inst *c=&s->code[i];
        *ops|=1u<<c->op;
        switch(c->op){
        case KSN_PROC_SET:used|=1u<<c->dst;break;
        case KSN_PROC_INPUT:used|=1u<<c->dst;if(c->a+1u>inputs)inputs=c->a+1u;break;
        case KSN_PROC_ADD:case KSN_PROC_MUL:used|=1u<<c->dst|1u<<c->a|1u<<c->b;break;
        case KSN_PROC_SIN:used|=1u<<c->dst|1u<<c->a;break;
        case KSN_PROC_REPEAT:case KSN_PROC_REPEAT_REG:
            if(c->op==KSN_PROC_REPEAT_REG)used|=1u<<c->a;
            if(++depth>maxd)maxd=depth;
            break;
        case KSN_PROC_END:--depth;break;
        case KSN_PROC_BREAK_IF_GT:case KSN_PROC_MOVE:case KSN_PROC_PLOT:case KSN_PROC_LINE:
            used|=1u<<c->a|1u<<c->b;break;
        case KSN_PROC_PLOT_COLOR_REG:case KSN_PROC_LINE_COLOR_REG:used|=1u<<c->dst|1u<<c->a|1u<<c->b;break;
        case KSN_PROC_CUBIC:used|=0xffu;break;
        default:break;
        }
    }
    unsigned n=0;for(unsigned r=0;r<16;++r)n+=(used>>r)&1u;
    s->regs=n;s->depth=maxd;s->inputs_read=inputs;
}
static unsigned ops_used;
static spec *find(int32_t h){for(unsigned i=0;i<spec_n;i++)if(specs[i]&&specs[i]->handle==h)return specs[i];return NULL;}
static bool load_spec(int32_t handle,JSValueConst code,JSValueConst pts){
    spec *s=calloc(1,sizeof *s);
    s->handle=handle;
    unsigned n=len(code);
    if(n<1||n>KSN_PROC_CODE){free(s);return false;}
    for(unsigned i=0;i<n;++i){
        JSValue row=JS_GetPropertyUint32(ctx,code,i);
        ksn_proc_inst *c=&s->code[i];
        c->op=(uint8_t)at(row,0);c->dst=(uint8_t)at(row,1);c->a=(uint8_t)at(row,2);c->b=(uint8_t)at(row,3);
        c->value=(float)at(row,4);c->color=(uint16_t)at(row,5);
        JS_FreeValue(ctx,row);
    }
    s->program=(ksn_proc_program){s->code,(uint8_t)n};
    if(!ksn_proc_plan_prepare(&s->plan,&s->program)){free(s);return false;}
    analyse(s,&ops_used);
    if(!JS_IsNull(pts)&&!JS_IsUndefined(pts)){
        JSValue x=JS_GetPropertyStr(ctx,pts,"x"),y=JS_GetPropertyStr(ctx,pts,"y"),
                k=JS_GetPropertyStr(ctx,pts,"coeff"),col=JS_GetPropertyStr(ctx,pts,"color");
        s->points=len(x);
        for(unsigned i=0;i<s->points&&i<MAX_POINTS;++i){s->x[i]=(int16_t)at(x,i);s->y[i]=(int16_t)at(y,i);}
        s->coeff=(KsnProcAffineQ14){(int16_t)at(k,0),(int16_t)at(k,1),(int16_t)at(k,2),(int16_t)at(k,3),
                                    (int32_t)at(k,4),(int32_t)at(k,5)};
        double cd=0;JS_ToFloat64(ctx,&cd,col);s->color=(uint16_t)cd;
        const KsnProcPointsPolicy policy={true,8};
        if(!ksn_proc_plan_register_points_affine(&s->plan,&s->coeff,&policy)){free(s);return false;}
        s->has_points=true;
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,k);JS_FreeValue(ctx,col);
    }
    unsigned slot=0;while(slot<spec_n&&specs[slot])slot++;
    if(slot==spec_n)spec_n++;
    specs[slot]=s;
    return true;
}
static void append(ksn_proc_frame *cand,const ksn_proc_frame *f){
    memcpy(&cand->segments[cand->count],f->segments,f->count*sizeof f->segments[0]);
    cand->count=(uint16_t)(cand->count+f->count);
    cand->raster_steps=(uint16_t)(cand->raster_steps+f->raster_steps);
}
typedef struct {
    unsigned frames,draws_max,seg_max,raster_max,draw_raster_max,draw_steps_max,frame_steps_max;
    unsigned long long seg_sum,raster_sum,steps_sum,draws_sum;
    unsigned points_max,plans_max,instr_max,regs_max,depth_max,inputs_max;
    int pt_lo,pt_hi,co_lo,co_hi;
} stats;
static stats st_state[6],st_tier[3],st_all;
static unsigned long long pixels_checked,draws_checked;
static unsigned frames_checked;
static unsigned run_draw(spec *s,const float in[KSN_PROC_INPUTS],unsigned *raster,unsigned *segs,stats *fs){
    ksn_proc_vm vp,vd,vr;
    CHECK(ksn_proc_plan_begin(&vp,&s->plan,in,&f_plan)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_plan_run(&vp,&s->plan,false)==KSN_PROC_DONE);
    CHECK(ksn_proc_plan_begin(&vd,&s->plan,in,&f_debug)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_plan_run(&vd,&s->plan,true)==KSN_PROC_DONE);
    CHECK(ksn_proc_begin(&vr,&s->program,in,&f_vm)==KSN_PROC_RUNNING);
    ksn_proc_status status;unsigned singles=0;
    do{status=ksn_proc_step(&vr);++singles;}while(status==KSN_PROC_RUNNING);
    CHECK(status==KSN_PROC_DONE);
    CHECK(vp.steps==vr.steps&&vd.steps==vr.steps&&singles==vr.steps);
    CHECK(f_debug.count==f_plan.count&&f_vm.count==f_plan.count);
    CHECK(!memcmp(f_debug.segments,f_plan.segments,f_plan.count*sizeof f_plan.segments[0]));
    CHECK(!memcmp(f_vm.segments,f_plan.segments,f_plan.count*sizeof f_plan.segments[0]));
    CHECK(!memcmp(vp.reg,vr.reg,sizeof vr.reg));
    CHECK(cand_plan.count+f_plan.count<=KSN_PROC_SEGMENTS);
    append(&cand_plan,&f_plan);append(&cand_vm,&f_vm);
    unsigned seg=f_plan.count,ras=f_plan.raster_steps;
    for(unsigned i=0;i<f_plan.count;i++){
        const ksn_proc_segment *g=&f_plan.segments[i];
        int lo=g->x0<g->y0?g->x0:g->y0,hi=g->x0>g->y0?g->x0:g->y0;
        int lo2=g->x1<g->y1?g->x1:g->y1,hi2=g->x1>g->y1?g->x1:g->y1;
        if(lo2<lo)lo=lo2;
        if(hi2>hi)hi=hi2;
        if(lo<fs->co_lo)fs->co_lo=lo;
        if(hi>fs->co_hi)fs->co_hi=hi;
    }
    if(s->has_points){
        const unsigned n=s->points;
        ksn_proc_points_affine_scalar((KsnProcPointDst){s->sx,s->sy},(KsnProcPointSrc){s->x,s->y},n,&s->coeff);
        ksn_proc_points_affine_pie((KsnProcPointDst){s->px,s->py},(KsnProcPointSrc){s->x,s->y},n,&s->coeff);
        KsnProcPointsDecision decision;
        CHECK(ksn_proc_plan_run_points_affine(&s->plan,(KsnProcPointDst){s->dx,s->dy},
                                              (KsnProcPointSrc){s->x,s->y},n,&decision));
        CHECK(!memcmp(s->sx,s->px,n*2)&&!memcmp(s->sy,s->py,n*2));
        CHECK(!memcmp(s->sx,s->dx,n*2)&&!memcmp(s->sy,s->dy,n*2));
        ksn_proc_frame *typed=&f_debug;
        typed->count=0;typed->raster_steps=0;typed->ready=true;
        for(unsigned i=0;i<n;++i){
            CHECK(s->sx[i]>=-480&&s->sx[i]<=720&&s->sy[i]>=-480&&s->sy[i]<=720);
            if(s->sx[i]<fs->pt_lo)fs->pt_lo=s->sx[i];
            if(s->sy[i]<fs->pt_lo)fs->pt_lo=s->sy[i];
            if(s->sx[i]>fs->pt_hi)fs->pt_hi=s->sx[i];
            if(s->sy[i]>fs->pt_hi)fs->pt_hi=s->sy[i];
            if(i){
                int ddx=abs(s->sx[i]-s->sx[i-1]),ddy=abs(s->sy[i]-s->sy[i-1]);
                typed->raster_steps=(uint16_t)(typed->raster_steps+(ddx>ddy?ddx:ddy)+1);
                typed->segments[typed->count++]=(ksn_proc_segment){s->sx[i-1],s->sy[i-1],s->sx[i],s->sy[i],s->color};
            }
        }
        CHECK(cand_plan.count+typed->count<=KSN_PROC_SEGMENTS);
        append(&cand_plan,typed);append(&cand_vm,typed);
        seg+=typed->count;ras+=typed->raster_steps;
        if(n>fs->points_max)fs->points_max=n;
    }
    CHECK(f_plan.raster_steps<=KSN_PROC_RASTER_STEPS&&vr.steps<=KSN_PROC_STEPS);
    if(seg>s->max_segs)s->max_segs=seg;
    if(ras>s->max_raster)s->max_raster=ras;
    if(vr.steps>s->max_steps)s->max_steps=vr.steps;
    s->draws++;
    *raster=f_plan.raster_steps;*segs=seg;
    return vr.steps;
}
static void acc(stats *a,const stats *f,unsigned draws,unsigned segs,unsigned raster,unsigned steps,
                unsigned draw_raster,unsigned draw_steps){
    a->frames++;
    if(draws>a->draws_max)a->draws_max=draws;
    if(segs>a->seg_max)a->seg_max=segs;
    if(raster>a->raster_max)a->raster_max=raster;
    if(draw_raster>a->draw_raster_max)a->draw_raster_max=draw_raster;
    if(draw_steps>a->draw_steps_max)a->draw_steps_max=draw_steps;
    if(steps>a->frame_steps_max)a->frame_steps_max=steps;
    a->seg_sum+=segs;a->raster_sum+=raster;a->steps_sum+=steps;a->draws_sum+=draws;
    if(live_plans>a->plans_max)a->plans_max=live_plans;
    if(f->points_max>a->points_max)a->points_max=f->points_max;
    if(f->instr_max>a->instr_max)a->instr_max=f->instr_max;
    if(f->regs_max>a->regs_max)a->regs_max=f->regs_max;
    if(f->depth_max>a->depth_max)a->depth_max=f->depth_max;
    if(f->inputs_max>a->inputs_max)a->inputs_max=f->inputs_max;
    if(f->pt_lo<a->pt_lo)a->pt_lo=f->pt_lo;
    if(f->pt_hi>a->pt_hi)a->pt_hi=f->pt_hi;
    if(f->co_lo<a->co_lo)a->co_lo=f->co_lo;
    if(f->co_hi>a->co_hi)a->co_hi=f->co_hi;
}
static const stats STATS0={.pt_lo=32767,.pt_hi=-32768,.co_lo=32767,.co_hi=-32768};
/* Registrations, releases and draws reach C at the call (the prelude's
 * wrappers call these), so the harness holds no program or input array in the
 * guest heap it measures. */
typedef struct { int32_t h; unsigned n; float in[KSN_PROC_INPUTS]; } rec_draw;
static rec_draw rec[128];
static unsigned rec_n;
static bool rec_committed;
static double rec_bg;
static JSValue js_reg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;
    double hd=0;JS_ToFloat64(c,&hd,argv[0]);
    if(!load_spec((int32_t)hd,argv[1],argv[2])){printf("ORACLE FAIL frame %u: plan %d rejected\n",frame_no,(int)hd);oracle_failures++;}
    live_plans++;registered++;
    if(live_plans>live_peak)live_peak=live_plans;
    return JS_UNDEFINED;
}
static JSValue js_unreg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;
    double hd=0;JS_ToFloat64(c,&hd,argv[0]);
    for(unsigned j=0;j<spec_n;j++)if(specs[j]&&specs[j]->handle==(int32_t)hd){free(specs[j]);specs[j]=NULL;}
    live_plans--;unregistered++;
    return JS_UNDEFINED;
}
static JSValue js_begin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;JS_ToFloat64(c,&rec_bg,argv[0]);rec_n=0;return JS_UNDEFINED;
}
static JSValue js_draw(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)c;
    if(rec_n==sizeof rec/sizeof rec[0]){oracle_failures++;return JS_UNDEFINED;}
    rec_draw *d=&rec[rec_n++];
    d->h=(int32_t)at(argv[0],0);d->n=len(argv[1]);
    for(unsigned k=0;k<KSN_PROC_INPUTS;k++)d->in[k]=k<d->n?(float)at(argv[1],k):0.0f;
    return JS_UNDEFINED;
}
static JSValue js_commit(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;rec_committed=true;return JS_UNDEFINED;
}
static unsigned last_segments,last_steps;
static int oracle(void){
    /* Names from the app's own handle table, for the per-plan lines. */
    JSValue keys=JS_Eval(ctx,"Object.keys(bigWave.PL).map(k=>[k,bigWave.PL[k]])",49,"keys.js",JS_EVAL_TYPE_GLOBAL);
    for(unsigned i=0,n=JS_IsException(keys)?0:len(keys);i<n;i++){
        JSValue e=JS_GetPropertyUint32(ctx,keys,i),k=JS_GetPropertyUint32(ctx,e,0);
        const char *name=JS_ToCString(ctx,k);spec *s=find((int32_t)at(e,1));
        if(s&&name)snprintf(s->key,sizeof s->key,"%s",name);
        if(name)JS_FreeCString(ctx,name);
        JS_FreeValue(ctx,k);JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,keys);
    int result=0;
    if(rec_committed){
        memset(&cand_plan,0,sizeof cand_plan);memset(&cand_vm,0,sizeof cand_vm);
        stats fs=STATS0;unsigned steps=0,draw_raster=0,draw_steps=0,n=rec_n;
        for(unsigned i=0;i<n;i++){
            spec *s=find(rec[i].h);
            const unsigned ni=rec[i].n;
            const float *input=rec[i].in;
            if(!s){printf("ORACLE FAIL frame %u: draw of unknown plan\n",frame_no);oracle_failures++;continue;}
            if(ni>fs.inputs_max)fs.inputs_max=ni;
            if(s->program.count>fs.instr_max)fs.instr_max=s->program.count;
            if(s->regs>fs.regs_max)fs.regs_max=s->regs;
            if(s->depth>fs.depth_max)fs.depth_max=s->depth;
            unsigned r=0,sg=0,stp=run_draw(s,input,&r,&sg,&fs);
            if(getenv("BW_DUMP")&&(unsigned)atoi(getenv("BW_DUMP"))==frame_no){
                printf("  draw %-5s in",s->key);for(unsigned k=0;k<ni;k++)printf(" %.3f",input[k]);
                printf(" | segs %u",f_plan.count);
                for(unsigned k=0;k<f_plan.count&&k<3;k++)printf(" (%d,%d)-(%d,%d)",f_plan.segments[k].x0,f_plan.segments[k].y0,f_plan.segments[k].x1,f_plan.segments[k].y1);
                printf("\n");
            }
            steps+=stp;draws_checked++;
            if(r>draw_raster)draw_raster=r;
            if(stp>draw_steps)draw_steps=stp;
        }
        if(cand_plan.count!=cand_vm.count||memcmp(cand_plan.segments,cand_vm.segments,cand_plan.count*sizeof cand_plan.segments[0])){
            printf("ORACLE FAIL frame %u: plan and VM segment lists differ\n",frame_no);oracle_failures++;
        }
        cand_plan.ready=cand_vm.ready=true;
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)pix_plan[i]=pix_vm[i]=(uint16_t)rec_bg;
        ksn_proc_render_band(&cand_plan,pix_plan,0,KSN_PROC_H);
        for(int y=0;y<KSN_PROC_H;y+=8)
            ksn_proc_render_band(&cand_vm,pix_vm+y*KSN_PROC_W,y,y+8<=KSN_PROC_H?8:KSN_PROC_H-y);
        if(memcmp(pix_plan,pix_vm,sizeof pix_plan)){printf("ORACLE FAIL frame %u: pixels differ\n",frame_no);oracle_failures++;}
        pixels_checked+=KSN_PROC_W*KSN_PROC_H;frames_checked++;
        if(cand_plan.count>KSN_PROC_SEGMENTS)oracle_failures++;
        unsigned state=(unsigned)num("bigWave.S.st"),tier=(unsigned)num("bigWave.S.tier");
        if(state<6)acc(&st_state[state],&fs,n,cand_plan.count,cand_plan.raster_steps,steps,draw_raster,draw_steps);
        if(tier<3&&state>=1&&state<=2)acc(&st_tier[tier],&fs,n,cand_plan.count,cand_plan.raster_steps,steps,draw_raster,draw_steps);
        acc(&st_all,&fs,n,cand_plan.count,cand_plan.raster_steps,steps,draw_raster,draw_steps);
        last_segments=cand_plan.count;last_steps=steps;
        result=1;
    }
    rec_committed=false;rec_n=0;
    return result;
}

static const char PRELUDE[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,"
    "capabilities:{get:n=>({name:n,supported:__cap(n)||n==='audio.tone',available:true})},"
    "audio:{tone:globalThis.__tone}};"
    "(function(){const P=pocket.kasane.procedural,R=P.register,U=P.unregister,D=P.draw,B=P.beginFrame,"
    "C=P.commit,g=globalThis;"
    "P.register=function(c,p){const h=p?R.call(P,c,p):R.call(P,c);g.__reg(h,c,p||null);return h};"
    "P.unregister=function(h){U.call(P,h);g.__unreg(h)};"
    "P.beginFrame=function(c){g.__begin(c);return B.apply(P,arguments)};"
    "P.draw=function(h,i){D.call(P,h,i);g.__draw([h],i)};"
    "P.commit=function(){C.call(P);g.__commit()};})();"
    /* The rider formula of big_wave.js, read-only: the bot must not set t0. */
    "globalThis.__rp=k=>{const S=bigWave.S,t=k.t0===undefined?0:(S.t-k.t0)/30;"
    "return [k.x+k.v*t,Math.min(.9,Math.max(.05,k.p+.15*Math.sin(.9*t+k.ph)))]};";
static JSValue js_cap(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *n=argc?JS_ToCString(c,argv[0]):NULL;
    bool yes=n&&pocket_api_supported(n);
    if(n)JS_FreeCString(c,n);
    return JS_NewBool(c,yes);
}

/* ---- heap */
static size_t heap_req_peak;
static size_t heap_used(void){
    JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);
    if((size_t)m.memory_used_size>heap_req_peak)heap_req_peak=(size_t)m.memory_used_size;
    return (size_t)m.malloc_size;
}
static size_t heap_live_peak,heap_peak;

/* ---- one game frame: the keys the script holds, a pump, frame(), the oracle, present */
static void frame(uint32_t buttons){
    frame_no++;
    pocket_input_pump(buttons);
    char call[64];snprintf(call,sizeof call,"frame(%u)",buttons);
    eval(call,strlen(call),"frame.js");
    oracle();
    present();
    if(getenv("BW_TRACE"))printf("  T%u st=%g xi=%.2f p=%.2f h=%+.2f v=%.2f z=%.2f tuck=%g tube=%g hit=%g segs=%u\n",frame_no,
        num("bigWave.S.st"),num("bigWave.S.x-bigWave.S.xc"),num("bigWave.S.p"),num("bigWave.S.h"),num("bigWave.S.v"),
        num("bigWave.S.z"),num("bigWave.S.tuck"),num("bigWave.S.tube"),num("bigWave.S.hit"),last_segments);
    size_t h=heap_used();if(h>heap_peak)heap_peak=h;
    if(!(frame_no%16)){JS_RunGC(rt);h=heap_used();if(h>heap_live_peak)heap_live_peak=h;}
}
static void frames(unsigned n,const char *keys){hold(keys);for(unsigned i=0;i<n;i++)frame(0);}
#define S_(f) num("bigWave.S." f)
static unsigned fails;
static void expect(bool ok,const char *what,const char *fmt,...);
#include <stdarg.h>
static void expect(bool ok,const char *what,const char *fmt,...){
    va_list ap;va_start(ap,fmt);char buf[256];vsnprintf(buf,sizeof buf,fmt,ap);va_end(ap);
    printf("%s %-44s %s\n",ok?"PASS":"FAIL",what,buf);
    if(!ok)fails++;
}
static double xi(void){return S_("x")-S_("xc");}
/* Closed-loop bot: reads the state, holds keys, never writes the state.
 * Threats in priority order: a breaking section (tuck, stay low), an obstacle
 * in the pop window (;), an obstacle ahead (steer to a lane beside it). The
 * lane is held by a heading target, so the turn does not overshoot. */
static const char look[]="(()=>{const S=bigWave.S,c=S.course;let lo=.34,hi=.5,tuck=0,pop=0;"
    "for(const k of c.secs)if(k.x+k.w>S.x-2&&k.x<S.x+12&&bigWave.curlAt(k.x+k.w/2)>.5){lo=.08;hi=.3;tuck=1}"
    "const dodge=(p,g,w0,w1)=>{if(g>w0&&g<w1&&Math.abs(p-S.p)<.2)pop=1;"
    "if(!tuck&&g>0&&g<14&&Math.abs(p-(lo+hi)/2)<.22){lo=p>.45?Math.max(.05,p-.4):Math.min(.8,p+.28);hi=lo+.12}};"
    "for(const k of c.rocks)dodge(k.p,k.x-S.x,2.2,3.4);"
    "for(const k of c.riders){const q=__rp(k);dodge(q[1],q[0]-S.x,1.8,3)}"
    "return globalThis.__look=[lo,hi,tuck,pop]})()";
static const char *bot(unsigned phase){
    static char keys[64];keys[0]=0;
    double q=xi(),p=S_("p"),z=S_("z"),h=S_("h");
    num(look);
    double lo=num("__look[0]"),hi=num("__look[1]");bool tuck=num("__look[2]")>0,pop=num("__look[3]")>0;
    double tq=14;
    if((phase&1)&&!tuck){tq=3.5;lo=.18;hi=.42;}
    if(pop&&z<=0)strcat(keys,"; ");
    if(tuck||(phase&1)||q<8.5)strcat(keys,". ");
    if(q<tq-((phase&1)?0:1.5))strcat(keys,"e ");
    else if(q>tq+((phase&1)?2:3))strcat(keys,"s ");
    double mid=(lo+hi)/2,want=3*(mid-p);
    if(want>.5)want=.5;
    if(want<-.5)want=-.5;
    if(p>=lo&&p<=hi)want=0;
    if(h<want-.08)strcat(keys,"a ");
    else if(h>want+.08)strcat(keys,"d ");
    return keys;
}

int main(int argc,char **argv){
    const char *path=argc>1?argv[1]:"apps/bigwave/big_wave.js";
    ppm_dir=getenv("BW_PPM");
    FILE *f=fopen(path,"rb");
    if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<16];size_t n=fread(src,1,sizeof src-1,f);fclose(f);src[n]=0;
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    const char *limit_env=getenv("BW_HEAP_LIMIT");
    if(limit_env){
        size_t limit=(size_t)strtoul(limit_env,NULL,0);
        JS_SetMemoryLimit(rt,limit);
        if(limit/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,limit/2);
    }
    pocket_kasane_install(ctx,NULL);
    pocket_input_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__cap",JS_NewCFunction(ctx,js_cap,"cap",1));
    JS_SetPropertyStr(ctx,g,"__tone",JS_NewCFunction(ctx,js_tone,"tone",1));
    JS_SetPropertyStr(ctx,g,"__reg",JS_NewCFunction(ctx,js_reg,"reg",3));
    JS_SetPropertyStr(ctx,g,"__unreg",JS_NewCFunction(ctx,js_unreg,"unreg",1));
    JS_SetPropertyStr(ctx,g,"__begin",JS_NewCFunction(ctx,js_begin,"begin",1));
    JS_SetPropertyStr(ctx,g,"__draw",JS_NewCFunction(ctx,js_draw,"draw",2));
    JS_SetPropertyStr(ctx,g,"__commit",JS_NewCFunction(ctx,js_commit,"commit",0));
    JS_FreeValue(ctx,g);
    JS_RunGC(rt);
    const size_t heap_before=heap_used();
    eval(PRELUDE,strlen(PRELUDE),"prelude.js");
    JS_RunGC(rt);
    const size_t heap_prelude=heap_used();
    peak_bytes=cur_bytes;const size_t eval_start=cur_bytes;
    edge("1",true);                       /* the Enter that launched it, stand-in */
    bool ok=eval(src,n,path);
    const size_t eval_peak=peak_bytes;
    oracle();present();
    JS_RunGC(rt);
    const size_t heap_after_eval=heap_used();
    printf("%s eval + first present; source %zu B\n",ok?"ok  ":"FAIL",n);
    edge("1",false);
    if(!ok)return 1;

    /* ---- 1. title: the attract ride and the loader */
    frames(40,"");
    expect(S_("st")==0,"title: attract loop keeps the title","st=%g x=%.1f",S_("st"),S_("x"));
    expect(num("bigWave.queue.length")==0,"title: plans loaded one per frame","live=%u registered=%u",live_plans,registered);
    ppm("01_title");
    /* ---- 2. start */
    frames(1,"1");frames(1,"");
    expect(S_("st")==1,"1 starts a ride","st=%g lives=%g",S_("st"),S_("lives"));
    frames(30,"");
    ppm("02_ride");
    double v0=S_("v"),q0=xi(),s0=S_("score");
    frames(30,"");
    double v1=S_("v"),q1=xi();
    expect(S_("st")==1&&S_("score")>s0,"trim: riding scores in the pocket","xi %.1f->%.1f v %.2f->%.2f score %.0f->%.0f",q0,q1,v0,v1,s0,S_("score"));
    /* ---- 3. E pumps: faster than the trimmed ride over the same frames */
    frames(30,"e");
    double v2=S_("v"),q2=xi();
    expect(v2>v1+1,"E held 1 s: speed rises","v %.2f->%.2f (+%.2f m/s) xi %.1f->%.1f",v1,v2,v2-v1,q1,q2);
    frames(20,"e");
    ppm("03_pump");
    /* ---- 4. S brakes: slower, falls back toward the curl */
    double v3=S_("v"),q3=xi();
    frames(40,"s");
    double v4=S_("v"),q4=xi();
    expect(v4<v3-1.5&&q4<q3,"S held: speed drops, falls back","v %.2f->%.2f xi %.1f->%.1f",v3,v4,q3,q4);
    frames(30,"e");frames(20,"");
    /* ---- 5. A climbs, D drops; roll follows the turn */
    double p5=S_("p");
    frames(12,"a");
    double p6=S_("p"),h6=S_("h"),roll6=S_("roll");
    expect(p6>p5+.03&&h6>0&&roll6<0,"A held: climbs the face, banks left","p %.2f->%.2f heading %+.2f roll %+.3f",p5,p6,h6,roll6);
    ppm("04_turn_left");
    frames(26,"d");
    double p7=S_("p"),h7=S_("h"),roll7=S_("roll");
    expect(p7<p6&&h7<0&&roll7>0,"D held: drops down the face, banks right","p %.2f->%.2f heading %+.2f roll %+.3f",p6,p7,h7,roll7);
    ppm("05_turn_right");
    frames(12,"a");frames(10,"");
    /* ---- 6. carve: , turns faster than A */
    double ha=S_("h");frames(5,"a");double hb=S_("h");frames(12,"d");frames(8,"");
    double hc=S_("h");frames(5,",");double hd=S_("h");
    expect(hd-hc>(hb-ha)*1.5,"carve , turns harder than A","A %+.3f rad/5f, carve %+.3f rad/5f",hb-ha,hd-hc);
    frames(8,"/");frames(20,"");
    /* ---- 7. pop */
    frames(1,";");
    double z1=S_("z");frames(1,"");double z2=S_("z");
    frames(4,"");ppm("06_air");
    unsigned air_frames=2;
    while(S_("z")>0&&air_frames<60){frames(1,"");air_frames++;}
    expect(z1>0&&z2>z1&&S_("z")==0&&S_("st")==1,"; pops: airborne, then lands","z %.2f->%.2f, landed after %u frames",z1,z2,air_frames+4);
    /* ---- 8. tube: brake into the barrel, tuck low, pump to hold it */
    unsigned guard=0;
    while(xi()>9&&guard++<200)frames(1,S_("p")>.3?"s d .":"s .");
    unsigned tube_max=0;double sc0=S_("score");
    for(unsigned i=0;i<90&&S_("st")==1;i++){
        frames(1,bot(1));
        if(S_("tube")>tube_max)tube_max=(unsigned)S_("tube");
        if(tube_max==40)ppm("07_tube");
    }
    expect(tube_max>=60&&S_("st")==1,"tuck (.) low in the barrel: tube ride","tube %u frames (%.1f s), score +%.0f",tube_max,tube_max/30.0,S_("score")-sc0);
    /* ---- 9. the lip: stand up inside the barrel */
    double lives0=S_("lives");unsigned lip0=wipes[0];
    for(unsigned i=0;i<20&&S_("st")==1;i++)frames(1,"e");
    frames(4,"");
    ppm("08_wipeout_lip");
    expect(S_("st")==2&&wipes[0]==lip0+1&&S_("lives")==lives0-1,"standing in the barrel: lip wipeout","why=LIP lives %g->%g",lives0,S_("lives"));
    frames(60,"");
    expect(S_("st")==1&&S_("inv")>0,"respawn after the wipeout","st=%g inv=%g xi=%.1f",S_("st"),S_("inv"),xi());
    frames(50,"");
    /* ---- 10. a rock: setup teleports the rider 8 m before one at its height */
    run("(()=>{const S=bigWave.S,r=S.course.rocks[1];S.x=r.x-8;S.xc=S.x-14;S.p=r.p;S.h=0;S.inv=0})()");
    unsigned rock0=wipes[1];
    for(unsigned i=0;i<40&&S_("st")==1;i++){frames(1,"");if(S_("st")==2)ppm("09_wipeout_rock");}
    expect(wipes[1]==rock0+1,"riding into a rock: rock wipeout","wipes ROCK=%u",wipes[1]);
    frames(12,"");ppm("10_wipeout_splash");
    frames(60,"");
    /* ---- 11. pause */
    frames(1,"tab");frames(1,"");double xp=S_("x");frames(20,"e");
    expect(S_("pause")==1&&S_("x")==xp,"tab pauses the ride","x %.2f->%.2f",xp,S_("x"));
    ppm("11_pause");
    frames(1,"tab");frames(5,"");
    expect(S_("pause")==0&&S_("x")>xp,"tab again resumes","x %.2f",S_("x"));
    /* ---- 12. hold S: the whitewater catches the rider until the game is over */
    unsigned over0=overs;
    for(unsigned i=0;i<1200&&S_("st")!=5;i++)frames(1,S_("st")==1?"s":"");
    expect(S_("st")==5&&overs==over0+1&&S_("lives")==0,"braking into the whitewater: game over","wipes LIP=%u CAUGHT=%u",wipes[0],wipes[3]);
    ppm("12_game_over");
    /* ---- 13. retry, then the bot clears all three sets with keys only */
    frames(1,"1");frames(1,"");
    for(unsigned i=0;i<80&&S_("st")!=1;i++)frames(1,"");
    expect(S_("st")==1&&S_("lives")==3&&S_("score")<50,"1 retries from set 1","st=%g lives=%g",S_("st"),S_("lives"));
    /* ---- 14. obstacles: setup places the rider (state writes), the action is keys */
    run("(()=>{const S=bigWave.S,r=S.course.rocks[2];S.x=r.x-9;S.xc=S.x-14;S.p=r.p;S.h=0;S.v=10;S.inv=0})()");
    unsigned rock1=wipes[1],popped=0;
    for(unsigned i=0;i<45&&S_("st")==1;i++){
        double ahead=num("bigWave.S.course.rocks[2].x-bigWave.S.x");
        bool pop=!popped&&ahead<3.5&&ahead>0;popped|=pop;
        frames(1,pop?";":"");
    }
    expect(popped&&wipes[1]==rock1&&S_("st")==1,"; over a rock: no wipeout","rock wipes %u->%u",rock1,wipes[1]);
    run("(()=>{const S=bigWave.S,c=S.course.riders[0];S.x=c.x-8;c.t0=undefined;S.xc=S.x-14;"
        "S.p=c.p+.15*Math.sin(c.ph);S.h=0;S.v=11;S.inv=0})()");
    static const char chase[]="__rp(bigWave.S.course.riders[0])[1]-bigWave.S.p";
    static const char gap[]="__rp(bigWave.S.course.riders[0])[0]-bigWave.S.x";
    unsigned rider0=wipes[2];popped=0;
    for(unsigned i=0;i<60&&S_("st")==1;i++){
        double dp=num(chase),ahead=num(gap);bool pop=!popped&&ahead<2.6&&ahead>0;popped|=pop;
        frames(1,pop?";":dp>.03?"e a":dp<-.03?"e d":"e");
        if(ahead<-3)break;
    }
    expect(popped&&wipes[2]==rider0&&S_("st")==1,"; over another rider: no wipeout","rider wipes %u->%u",rider0,wipes[2]);
    run("(()=>{const S=bigWave.S,c=S.course.riders[1];S.x=c.x-8;c.t0=undefined;S.xc=S.x-14;"
        "S.p=c.p+.15*Math.sin(c.ph);S.h=0;S.v=11;S.inv=0})()");
    static const char chase1[]="__rp(bigWave.S.course.riders[1])[1]-bigWave.S.p";
    for(unsigned i=0;i<90&&S_("st")==1;i++){double dp=num(chase1);frames(1,dp>.03?"e a":dp<-.03?"e d":"e");}
    ppm("16_wipeout_rider");
    expect(wipes[2]==rider0+1,"steering into another rider: rider wipeout","why=RIDER lives=%g",S_("lives"));
    for(unsigned i=0;i<60&&S_("st")!=1;i++)frames(1,"");
    frames(10,"");
    /* A breaking section: tucked and low passes (tube points), standing high does not. */
    run("(()=>{const S=bigWave.S,c=S.course.secs[0];S.x=c.x-3;S.xc=c.x+c.w+4-19;S.p=.25;S.h=0;S.v=10;S.inv=0})()");
    double t0=S_("tube"),sc1=S_("score");unsigned in_sec=0,lip1=wipes[0];
    for(unsigned i=0;i<25&&S_("st")==1;i++){
        frames(1,". e");
        if(S_("tube")>0)in_sec++;
        if(i==8)ppm("17_section_tuck");
    }
    expect(in_sec>5&&wipes[0]==lip1&&S_("st")==1,"tucked low through a breaking section","%u tube frames, score +%.0f (tube %g)",
           in_sec,S_("score")-sc1,t0);
    frames(40,"");
    run("(()=>{const S=bigWave.S,c=S.course.secs[1];S.x=c.x-3;S.xc=c.x+c.w+4-19;S.p=.75;S.h=0;S.v=10;S.inv=0})()");
    for(unsigned i=0;i<25&&S_("st")==1;i++)frames(1,"e");
    expect(wipes[0]==lip1+1,"standing high through a breaking section: lip","why=LIP lives=%g",S_("lives"));
    for(unsigned i=0;i<60&&S_("st")!=1;i++)frames(1,"");
    frames(20,"");
    /* A bad landing: carve up the face through the air. */
    unsigned land0=wipes[4];
    frames(8,"a");frames(1,"a ;");
    for(unsigned i=0;i<40&&S_("st")==1;i++)frames(1,"a");
    expect(wipes[4]==land0+1,"landing across the face (A held in the air)","why=LAND lives=%g",S_("lives"));
    for(unsigned i=0;i<80&&S_("st")!=5;i++)frames(1,"");
    expect(S_("st")==5,"third wipeout ends the run","st=%g",S_("st"));
    frames(1,"1");frames(1,"");
    for(unsigned i=0;i<80&&S_("st")!=1;i++)frames(1,"");
    unsigned clear0=clears,bot_frames=0,wipe_sum0=wipes[0]+wipes[1]+wipes[2]+wipes[3]+wipes[4];
    bool shot_set2=false,shot_dusk=false;
    while(S_("st")!=4&&S_("st")!=5&&bot_frames<9000){
        unsigned ph=((unsigned)(S_("x")/60))%4==2?1:0;   /* a tube attempt every 240 m */
        frames(1,S_("st")==1?bot(ph):"");bot_frames++;
        if(!shot_set2&&S_("st")==3){ppm("13_set_break");shot_set2=true;}
        if(!shot_dusk&&S_("set")==2&&S_("st")==1&&S_("x")>60){ppm("14_dusk");shot_dusk=true;}
    }
    unsigned wipe_bot=wipes[0]+wipes[1]+wipes[2]+wipes[3]+wipes[4]-wipe_sum0;
    frames(3,"");ppm("15_clear");
    expect(S_("st")==4&&clears==clear0+1,"bot: three sets cleared with keys only","%u frames, %u wipeouts, score %.0f, set clears %u",
           bot_frames,wipe_bot,S_("score"),set_clears);
    /* ---- 15. load tiers: the same bot ride at LIGHT and HEAVY (MID above) */
    for(unsigned t=0;t<3;t++){
        if(t==1)continue;
        char k[2]={(char)('2'+t),0};
        frames(1,k);frames(1,"");frames(1,"1");frames(1,"");
        for(unsigned i=0;i<80&&S_("st")!=1;i++)frames(1,"");
        expect(S_("tier")==t&&S_("st")==1,"tier key selects the load","tier=%g",S_("tier"));
        for(unsigned i=0;i<900&&S_("st")==1;i++){frames(1,bot(((unsigned)(S_("x")/60))%4==2));}
        for(unsigned i=0;i<200&&S_("st")!=1&&S_("st")!=4&&S_("st")!=5;i++)frames(1,"");
        while(S_("st")!=4&&S_("st")!=5){frames(1,S_("st")==1?"s":"");if(frame_no>40000)break;}
    }
    /* ---- 16. Back: the save turn */
    unsigned back0=backs,exc0=exceptions;
    hold("");pocket_input_pump(0x2000);frame_no++;eval("frame(8192)",11,"frame.js");
    int drew=oracle();
    expect(backs==back0+1&&exceptions==exc0&&!drew,"Back (frame(0x2000)): logs, draws nothing","%s",last_log);

    /* ---- report */
    printf("\nplans: live peak %u (limit 32), registered %u, unregistered %u\n",live_peak,registered,unregistered);
    printf("\nper plan (max over the run): key instr regs depth inputs points | segs raster steps draws\n");
    for(unsigned i=0;i<spec_n;i++){spec *s=specs[i];if(!s)continue;
        printf("  %-5s %3u %3u %2u %2u %4u | %4u %5u %5u %6u\n",s->key,s->program.count,s->regs,s->depth,s->inputs_read,
               s->points,s->max_segs,s->max_raster,s->max_steps,s->draws);}
    static const char *const SN[]={"title","ride","wipeout","set break","clear","game over"};
    printf("\nper frame: frames draws_max seg_max seg_avg raster_max raster_avg draw_raster_max draw_steps_max frame_steps_max frame_steps_avg plans_max instr_max regs_max depth_max inputs_max points_max coord_lo coord_hi\n");
    for(unsigned i=0;i<6+3+1;i++){
        const stats *a=i<6?&st_state[i]:i<9?&st_tier[i-6]:&st_all;
        char name[32];
        if(i<6)snprintf(name,sizeof name,"state %s",SN[i]);
        else if(i<9)snprintf(name,sizeof name,"ride tier %s",i==6?"LIGHT":i==7?"MID":"HEAVY");
        else snprintf(name,sizeof name,"all");
        if(!a->frames)continue;
        printf("  %-16s %6u %3u %4u %6.1f %6u %8.1f %5u %5u %6u %8.1f %3u %3u %3u %2u %2u %4u %5d %5d\n",name,a->frames,a->draws_max,
               a->seg_max,(double)a->seg_sum/a->frames,a->raster_max,(double)a->raster_sum/a->frames,a->draw_raster_max,a->draw_steps_max,
               a->frame_steps_max,(double)a->steps_sum/a->frames,a->plans_max,a->instr_max,a->regs_max,a->depth_max,a->inputs_max,a->points_max,
               a->co_lo,a->co_hi);
    }
    printf("\nops used:");
    static const char *const OPN[]={"SET","INPUT","ADD","MUL","SIN","REPEAT","END","MOVE","PLOT","LINE","REPEAT_REG",
                                    "BREAK_IF_GT","PLOT_COLOR_REG","LINE_COLOR_REG","CUBIC"};
    for(unsigned op=0;op<15;op++)if(ops_used&(1u<<op))printf(" %s",OPN[op]);
    printf("\nops unused:");
    for(unsigned op=0;op<15;op++)if(!(ops_used&(1u<<op)))printf(" %s",OPN[op]);
    printf("\noracle: %u frames, %llu draws, %llu pixels, plan == debug-step plan == single-step VM, failures %u\n",
           frames_checked,draws_checked,pixels_checked,oracle_failures);
    printf("guest heap (TLSF model, %zu-bit host): runtime %zu, after prelude %zu, after eval %zu (+%zu), "
           "live peak after GC %zu (+%zu), peak before GC %zu (+%zu)\n",sizeof(void *)*8,heap_before,heap_prelude,
           heap_after_eval,heap_after_eval-heap_prelude,heap_live_peak,heap_live_peak-heap_prelude,heap_peak,heap_peak-heap_prelude);
    printf("guest heap (TLSF model): before eval %zu, peak during eval %zu (the compile), whole-run peak %zu; "
           "QuickJS memory_used_size peak %zu; device limit 163840\n",eval_start,eval_peak,peak_bytes,heap_req_peak);
    printf("frames %u: exceptions=%u present_failures=%u tones=%u wipes LIP=%u ROCK=%u RIDER=%u CAUGHT=%u LAND=%u tube_logs=%u\n",
           frame_no,exceptions,present_failures,tones,wipes[0],wipes[1],wipes[2],wipes[3],wipes[4],tube_logs);
    bool pass=!fails&&!exceptions&&!present_failures&&!oracle_failures&&live_peak<=32&&readys==1;
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",pass?"BIGWAVE_HOST PASS":"BIGWAVE_HOST FAIL");
    return pass?0:1;
}
