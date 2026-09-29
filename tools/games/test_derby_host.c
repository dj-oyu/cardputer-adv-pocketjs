/* apps/derby/derby_watch.js as the firmware runs it, on the host: the real
 * QuickJS, the real pocket.kasane (view + procedural) and the real renderer
 * presenting into a 240x135 panel after every frame(), with key edges fed
 * through the real keymap_poll() and keystate into pocket.input.keys.
 *
 * A scripted game: paddock (pick #3, bet, cycle the load tier), gate, race
 * with camera changes, slow motion, photo, result, a replay of the same race
 * with no camera changes (must finish identically: the camera cannot change
 * the race), the next race, and the Back turn (frame(0x2000) saves).
 *
 * Every procedural draw is also run three ways that must agree bit for bit
 * (ksn_proc_plan_run, the debug-step plan, and ksn_proc_begin + one
 * ksn_proc_step at a time), typed points scalar vs PIE model vs dispatcher,
 * and every committed frame is rendered from the plan segments (whole frame)
 * and from the VM segments (8-row bands) and compared pixel for pixel. The
 * adapter's limits are checked and per-frame statistics are written.
 *
 *   python3 tools/games/run_derby.py            (WSL)
 * Env: DERBY_TIER=0|2 (LIGHT or HEAVY, no replay), DERBY_PPM=<dir>,
 *      DERBY_CSV=<file>, DERBY_HEAP_LIMIT=<bytes>.
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
#include "pocket_sub_impl.inc"

/* ---- the guest allocator as the device charges it (TLSF lengths), as in
 * tools/kasane_contract/test_megademo_app_host.c. */
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

/* ---- stubs, as tools/test_keytest_app.c */
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

static board_keyevent_t fifo[16];
static unsigned fifo_n,fifo_at;
bool board_key_event(board_keyevent_t *out){
    if(fifo_at==fifo_n){fifo_n=fifo_at=0;return false;}
    *out=fifo[fifo_at++];return true;
}
static void edge(const char *name,bool pressed){
    int i=keymap_key_index(name);
    if(i<0){printf("no key %s\n",name);exit(2);}
    if(fifo_at==fifo_n)fifo_n=fifo_at=0;
    fifo[fifo_n++]=(board_keyevent_t){.row=(uint8_t)(i/14),.col=(uint8_t)(i%14),.pressed=pressed};
    keystroke_t k;keymap_poll(&k);
}

#define FAIL(...) do{printf("FAIL: " __VA_ARGS__);printf("\n");exit(1);}while(0)
#define REQ(t) do{if(!(t))FAIL("%s:%d %s (tick %u scene %s)",__FILE__,__LINE__,#t,tick,scene);}while(0)

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8],panel[240*135];
static unsigned tick,exceptions,bad_present,framefails;
static char scene[16]="boot";
static unsigned scene_t;
static char finish[3][512];
static unsigned finishes,loaded_seen,saves,go_seen,slow_at,lead_logs;
static int tier_env=1;   /* the app starts at MID */

/* ---- the procedural oracle */
#define SLOTS 64u
typedef struct {
    int handle;
    ksn_proc_inst code[KSN_PROC_CODE];
    ksn_proc_program program;
    ksn_proc_plan plan;
    bool has_points;
    unsigned points;
    uint16_t color;
    KsnProcAffineQ14 coeff;
    _Alignas(16) int16_t x[128],y[128],sx[128],sy[128],px[128],py[128],dx[128],dy[128];
    unsigned regs,depth,inputs_read;
} spec;
static spec specs[SLOTS];
static unsigned live_plans,live_max,registered,unregistered,ops_used,frame_regs,frame_reg_max;
static ksn_proc_frame f_plan,f_debug,f_vm,cand_plan,cand_vm;
static uint16_t pix_plan[240*135],pix_vm[240*135];
static uint16_t cur_bg;
static unsigned frame_draws,frame_steps,draw_raster_max,draw_steps_max,frame_points,frame_instr_max,frames_checked;
static int pt_lo=32767,pt_hi=-32768;
static uint64_t pixel_hash=1469598103934665603ull;
static FILE *csv;

typedef struct {
    char name[16];
    unsigned frames,draws_max,seg_max,raster_max,draw_raster_max,draw_steps_max,frame_steps_max,live_max,
             instr_max,regs_max,depth_max,inputs_max,points_max;
    unsigned long long seg_sum,raster_sum;
} scene_stats;
static scene_stats stats[8];
static unsigned nstats;
static unsigned g_regs_max,g_depth_max,g_inputs_max,g_instr_max;
static scene_stats *stat_for(const char *n){
    for(unsigned i=0;i<nstats;i++)if(!strcmp(stats[i].name,n))return &stats[i];
    REQ(nstats<8);
    snprintf(stats[nstats].name,sizeof stats[0].name,"%s",n);
    return &stats[nstats++];
}

static double num(JSValueConst v){double d=NAN;JS_ToFloat64(ctx,&d,v);return d;}
static double at(JSValueConst a,unsigned i){JSValue v=JS_GetPropertyUint32(ctx,a,i);double d=num(v);JS_FreeValue(ctx,v);return d;}
static unsigned len(JSValueConst a){JSValue v=JS_GetPropertyStr(ctx,a,"length");unsigned n=(unsigned)num(v);JS_FreeValue(ctx,v);return n;}
static spec *find(int h){for(unsigned i=0;i<SLOTS;i++)if(specs[i].handle==h)return &specs[i];return NULL;}

static void analyse(spec *s){
    uint16_t used=0;unsigned depth=0,maxd=0,inputs=0;
    for(unsigned i=0;i<s->program.count;i++){
        const ksn_proc_inst *c=&s->code[i];
        ops_used|=1u<<c->op;
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
        default:REQ(!"opcode");
        }
    }
    unsigned n=0;for(unsigned r=0;r<16;r++)n+=(used>>r)&1u;
    s->regs=n;s->depth=maxd;s->inputs_read=inputs;
    if(n>g_regs_max)g_regs_max=n;
    if(maxd>g_depth_max)g_depth_max=maxd;
    if(s->program.count>g_instr_max)g_instr_max=s->program.count;
}
static JSValue js_cap_reg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    spec *s=NULL;
    for(unsigned i=0;i<SLOTS&&!s;i++)if(!specs[i].handle)s=&specs[i];
    REQ(s);
    memset(s,0,sizeof *s);
    s->handle=(int)num(argv[0]);
    unsigned n=len(argv[1]);
    REQ(n>=1&&n<=KSN_PROC_CODE);
    for(unsigned i=0;i<n;i++){
        JSValue row=JS_GetPropertyUint32(ctx,argv[1],i);
        REQ(len(row)==6);
        s->code[i]=(ksn_proc_inst){(uint8_t)at(row,0),(uint8_t)at(row,1),(uint8_t)at(row,2),(uint8_t)at(row,3),
                                   (float)at(row,4),(uint16_t)at(row,5)};
        JS_FreeValue(ctx,row);
    }
    s->program=(ksn_proc_program){s->code,(uint8_t)n};
    REQ(ksn_proc_plan_prepare(&s->plan,&s->program));
    analyse(s);
    if(argc>2&&JS_IsObject(argv[2])){
        JSValue x=JS_GetPropertyStr(ctx,argv[2],"x"),y=JS_GetPropertyStr(ctx,argv[2],"y"),
                k=JS_GetPropertyStr(ctx,argv[2],"coeff"),col=JS_GetPropertyStr(ctx,argv[2],"color");
        s->points=len(x);
        REQ(s->points>=2&&s->points<=128&&len(y)==s->points&&len(k)==6);
        for(unsigned i=0;i<s->points;i++){s->x[i]=(int16_t)at(x,i);s->y[i]=(int16_t)at(y,i);}
        s->coeff=(KsnProcAffineQ14){(int16_t)at(k,0),(int16_t)at(k,1),(int16_t)at(k,2),(int16_t)at(k,3),
                                    (int32_t)at(k,4),(int32_t)at(k,5)};
        s->color=(uint16_t)num(col);
        const KsnProcPointsPolicy policy={true,8};
        REQ(ksn_proc_plan_register_points_affine(&s->plan,&s->coeff,&policy));
        s->has_points=true;
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,k);JS_FreeValue(ctx,col);
    }
    live_plans++;registered++;frame_regs++;
    if(live_plans>live_max)live_max=live_plans;
    REQ(live_plans<=32);
    return JS_UNDEFINED;
}
static JSValue js_cap_unreg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    spec *s=find((int)num(argv[0]));REQ(s);
    s->handle=0;live_plans--;unregistered++;
    return JS_UNDEFINED;
}
static JSValue js_cap_begin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    memset(&cand_plan,0,sizeof cand_plan);memset(&cand_vm,0,sizeof cand_vm);
    cur_bg=(uint16_t)num(argv[0]);
    frame_draws=frame_steps=frame_points=frame_instr_max=0;
    return JS_UNDEFINED;
}
static void append(ksn_proc_frame *cand,const ksn_proc_frame *f){
    REQ((unsigned)cand->count+f->count<=KSN_PROC_SEGMENTS);
    memcpy(&cand->segments[cand->count],f->segments,f->count*sizeof f->segments[0]);
    cand->count=(uint16_t)(cand->count+f->count);
    REQ((unsigned)cand->raster_steps+f->raster_steps<=UINT16_MAX);
    cand->raster_steps=(uint16_t)(cand->raster_steps+f->raster_steps);
}
static JSValue js_cap_draw(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    spec *s=find((int)num(argv[0]));REQ(s);
    unsigned ni=len(argv[1]);REQ(ni<=KSN_PROC_INPUTS);
    REQ(s->inputs_read<=ni||!s->inputs_read);
    if(ni>g_inputs_max)g_inputs_max=ni;
    float in[KSN_PROC_INPUTS]={0};
    for(unsigned i=0;i<ni;i++){in[i]=(float)at(argv[1],i);REQ(isfinite(in[i]));}
    ksn_proc_vm vp,vd,vr;
    REQ(ksn_proc_plan_begin(&vp,&s->plan,in,&f_plan)==KSN_PROC_RUNNING);
    REQ(ksn_proc_plan_run(&vp,&s->plan,false)==KSN_PROC_DONE);
    REQ(ksn_proc_plan_begin(&vd,&s->plan,in,&f_debug)==KSN_PROC_RUNNING);
    REQ(ksn_proc_plan_run(&vd,&s->plan,true)==KSN_PROC_DONE);
    REQ(ksn_proc_begin(&vr,&s->program,in,&f_vm)==KSN_PROC_RUNNING);
    ksn_proc_status st;unsigned singles=0;
    do{st=ksn_proc_step(&vr);singles++;}while(st==KSN_PROC_RUNNING);
    REQ(st==KSN_PROC_DONE);
    REQ(vp.steps==vr.steps&&vd.steps==vr.steps&&singles==vr.steps);
    const ksn_proc_frame *fs[2]={&f_debug,&f_vm};
    for(unsigned i=0;i<2;i++){
        REQ(fs[i]->count==f_plan.count&&fs[i]->raster_steps==f_plan.raster_steps);
        REQ(!memcmp(fs[i]->segments,f_plan.segments,f_plan.count*sizeof f_plan.segments[0]));
    }
    REQ(!memcmp(vp.reg,vr.reg,sizeof vr.reg));
    REQ(f_plan.raster_steps<=KSN_PROC_RASTER_STEPS&&vr.steps<=KSN_PROC_STEPS);
    if(f_plan.raster_steps>draw_raster_max)draw_raster_max=f_plan.raster_steps;
    if(vr.steps>draw_steps_max)draw_steps_max=vr.steps;
    frame_steps+=vr.steps;frame_draws++;
    if(s->program.count>frame_instr_max)frame_instr_max=s->program.count;
    append(&cand_plan,&f_plan);append(&cand_vm,&f_vm);
    if(s->has_points){
        const unsigned n=s->points;
        ksn_proc_points_affine_scalar((KsnProcPointDst){s->sx,s->sy},(KsnProcPointSrc){s->x,s->y},n,&s->coeff);
        ksn_proc_points_affine_pie((KsnProcPointDst){s->px,s->py},(KsnProcPointSrc){s->x,s->y},n,&s->coeff);
        KsnProcPointsDecision dec;
        REQ(ksn_proc_plan_run_points_affine(&s->plan,(KsnProcPointDst){s->dx,s->dy},(KsnProcPointSrc){s->x,s->y},n,&dec));
        REQ(!memcmp(s->sx,s->px,n*2)&&!memcmp(s->sy,s->py,n*2));
        REQ(!memcmp(s->sx,s->dx,n*2)&&!memcmp(s->sy,s->dy,n*2));
        ksn_proc_frame typed={.ready=true};
        for(unsigned i=0;i<n;i++){
            REQ(s->sx[i]>=-480&&s->sx[i]<=720&&s->sy[i]>=-480&&s->sy[i]<=720);
            if(s->sx[i]<pt_lo)pt_lo=s->sx[i];
            if(s->sy[i]<pt_lo)pt_lo=s->sy[i];
            if(s->sx[i]>pt_hi)pt_hi=s->sx[i];
            if(s->sy[i]>pt_hi)pt_hi=s->sy[i];
            if(i){
                int ddx=abs(s->sx[i]-s->sx[i-1]),ddy=abs(s->sy[i]-s->sy[i-1]);
                typed.raster_steps=(uint16_t)(typed.raster_steps+(ddx>ddy?ddx:ddy)+1);
                typed.segments[typed.count++]=(ksn_proc_segment){s->sx[i-1],s->sy[i-1],s->sx[i],s->sy[i],s->color};
            }
        }
        append(&cand_plan,&typed);append(&cand_vm,&typed);
        frame_points+=n;
    }
    return JS_UNDEFINED;
}
static JSValue js_cap_commit(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;
    REQ(cand_plan.count==cand_vm.count&&!memcmp(cand_plan.segments,cand_vm.segments,cand_plan.count*sizeof cand_plan.segments[0]));
    cand_plan.ready=cand_vm.ready=true;
    for(unsigned i=0;i<240*135;i++)pix_plan[i]=pix_vm[i]=cur_bg;
    REQ(ksn_proc_render_band(&cand_plan,pix_plan,0,135));
    for(int y=0;y<135;y+=8)REQ(ksn_proc_render_band(&cand_vm,pix_vm+y*240,y,y+8<=135?8:135-y));
    REQ(!memcmp(pix_plan,pix_vm,sizeof pix_plan));
    for(size_t i=0;i<sizeof pix_plan;i++){pixel_hash^=((const unsigned char *)pix_plan)[i];pixel_hash*=1099511628211ull;}
    frames_checked++;
    scene_stats *st=stat_for(scene);
    st->frames++;
    if(frame_draws>st->draws_max)st->draws_max=frame_draws;
    if(cand_plan.count>st->seg_max)st->seg_max=cand_plan.count;
    st->seg_sum+=cand_plan.count;st->raster_sum+=cand_plan.raster_steps;
    if(cand_plan.raster_steps>st->raster_max)st->raster_max=cand_plan.raster_steps;
    if(draw_raster_max>st->draw_raster_max)st->draw_raster_max=draw_raster_max;
    if(draw_steps_max>st->draw_steps_max)st->draw_steps_max=draw_steps_max;
    if(frame_steps>st->frame_steps_max)st->frame_steps_max=frame_steps;
    if(live_plans>st->live_max)st->live_max=live_plans;
    if(frame_instr_max>st->instr_max)st->instr_max=frame_instr_max;
    if(frame_points>st->points_max)st->points_max=frame_points;
    if(csv)fprintf(csv,"%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",tick,scene,frame_draws,cand_plan.count,
                   cand_plan.raster_steps,draw_raster_max,draw_steps_max,frame_steps,live_plans,frame_points,
                   frame_instr_max,frame_regs);
    draw_raster_max=draw_steps_max=0;
    return JS_UNDEFINED;
}

/* ---- logs: the app's DERBY markers drive the script */
static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s)return JS_UNDEFINED;
    printf("  [%5u] %s\n",tick,s);
    if(!strncmp(s,"DERBY SCENE ",12)){
        const char *n=s+12,*sp=strchr(n,' ');
        size_t k=sp?(size_t)(sp-n):strlen(n);
        if(k>=sizeof scene)k=sizeof scene-1;
        memcpy(scene,n,k);scene[k]=0;scene_t=0;
    }
    if(!strncmp(s,"DERBY FINISH ",13)&&finishes<3)snprintf(finish[finishes++],sizeof finish[0],"%s",s+13);
    if(!strncmp(s,"DERBY LOADED",12))loaded_seen++;
    if(!strncmp(s,"DERBY SAVE",10))saves++;
    if(!strncmp(s,"DERBY GO",8)){go_seen++;if(strstr(s,"LOADSTALL"))framefails++;}
    if(!strncmp(s,"DERBY SLOW",10))slow_at=tick;
    if(!strncmp(s,"DERBY LEAD",10))lead_logs++;
    if(!strncmp(s,"DERBY FRAMEFAIL",15)||!strncmp(s,"DERBY LOADFAIL",14))framefails++;
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static unsigned tones;
static JSValue js_tone(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;tones++;return JS_UNDEFINED;
}
static bool eval(const char *src,size_t n,const char *name){
    JSValue v=JS_Eval(ctx,src,n,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok){
        JSValue e=JS_GetException(ctx);const char *t=JS_ToCString(ctx,e);
        printf("  %s threw: %s\n",name,t?t:"?");if(t)JS_FreeCString(ctx,t);
        JSValue st=JS_GetPropertyStr(ctx,e,"stack");const char *ss=JS_IsString(st)?JS_ToCString(ctx,st):NULL;
        if(ss){printf("%s\n",ss);JS_FreeCString(ctx,ss);}
        JS_FreeValue(ctx,st);JS_FreeValue(ctx,e);exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *cc;while(JS_ExecutePendingJob(rt,&cc)>0){}
    pocket_kasane_end_turn();
    return ok;
}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)o;memcpy(panel+y*240,p,rows*240*sizeof *p);return KSN_OK;
}
static void present(uint64_t now_us){
    pocket_kasane_set_animation_time(now_us);
    (void)pocket_kasane_advance(now_us);
    ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats st;
    if(pocket_kasane_present(&port,&st)!=KSN_OK)bad_present++;
    else pocket_kasane_animations_presented(now_us);
}
static const char *ppm_dir;
static void ppm(const char *tag){
    if(!ppm_dir)return;
    char name[600];snprintf(name,sizeof name,"%s/derby_%05u_%s.ppm",ppm_dir,tick,tag);
    FILE *o=fopen(name,"wb");if(!o)return;
    fprintf(o,"P6\n240 135\n255\n");
    for(size_t i=0;i<240*135;i++){
        uint16_t p=panel[i];
        unsigned char rgb[3]={(unsigned char)((((p>>11)&31u)*255u+15u)/31u),
            (unsigned char)((((p>>5)&63u)*255u+31u)/63u),(unsigned char)(((p&31u)*255u+15u)/31u)};
        fwrite(rgb,1,3,o);
    }
    fclose(o);
}
static size_t heap_live_peak;
static size_t heap_used(void){JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);return (size_t)m.malloc_size;}

/* One turn: pump keys, frame(buttons), present. */
static void frame(unsigned buttons){
    pocket_input_pump(0);
    frame_regs=0;
    char call[32];snprintf(call,sizeof call,"frame(%u)",buttons);
    eval(call,strlen(call),"frame.js");
    if(frame_regs>frame_reg_max)frame_reg_max=frame_regs;
    present((uint64_t)tick*33333u);
    size_t h=heap_used();(void)h;
    JS_RunGC(rt);
    h=heap_used();if(h>heap_live_peak)heap_live_peak=h;
    tick++;scene_t++;
}
static void tap(const char *k){edge(k,true);frame(0);edge(k,false);frame(0);}
static void run_until(const char *s,unsigned limit){
    unsigned n=0;
    while(strcmp(scene,s)){frame(0);if(++n>limit)FAIL("scene %s not reached from %s",s,scene);}
}

static const char PRELUDE[]=
    "globalThis.console={log:globalThis.__log};"
    "(function(){const P=kasane.procedural,R=P.register,U=P.unregister,B=P.beginFrame,D=P.draw,C=P.commit;"
    "P.register=function(p,q){const h=q?R.call(P,p,q):R.call(P,p);__reg(h,p,q);return h};"
    "P.unregister=function(h){U.call(P,h);__unreg(h)};"
    "P.beginFrame=function(c,s){const r=s===undefined?B.call(P,c):B.call(P,c,s);__begin(c);return r};"
    "P.draw=function(h,i){D.call(P,h,i);__draw(h,i)};"
    "P.commit=function(){C.call(P);__commit()};})();"
    "globalThis.__store={'derby.v1':{v:1,pts:1500,race:3}};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,"
    "capabilities:{get:n=>({name:n,supported:true,available:true})},memory:{info:()=>({internalFreeBytes:40000})},"
    "audio:{cue:()=>true,tone:s=>{__tone(s.frequencyHz);return Promise.resolve()}},"
    "storage:{get:k=>Promise.resolve(k in __store?{value:JSON.parse(JSON.stringify(__store[k])),revision:1}:null),"
    "set:(k,v)=>{__store[k]=JSON.parse(JSON.stringify(v));return Promise.resolve({revision:1})}}};";

int main(int argc,char **argv){
    const char *path=argc>1?argv[1]:"apps/derby/derby_watch.js";
    FILE *f=fopen(path,"rb");if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<16];size_t n=fread(src,1,sizeof src-1,f);fclose(f);src[n]=0;
    ppm_dir=getenv("DERBY_PPM");
    if(getenv("DERBY_TIER"))tier_env=atoi(getenv("DERBY_TIER"));
    if(getenv("DERBY_CSV"))csv=fopen(getenv("DERBY_CSV"),"w");
    if(csv)fprintf(csv,"tick,scene,draws,segments,raster,draw_raster_max,draw_steps_max,frame_steps,live_plans,points,instr_max,registered\n");
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    const char *lim=getenv("DERBY_HEAP_LIMIT");
    if(lim){size_t l=(size_t)strtoul(lim,NULL,0);JS_SetMemoryLimit(rt,l);if(l/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,l/2);}
    pocket_kasane_install(ctx,NULL);
    pocket_input_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    static const struct {const char *n;JSCFunction *f;int a;} fns[]={
        {"__log",js_log,1},{"__reg",js_cap_reg,3},{"__unreg",js_cap_unreg,1},{"__begin",js_cap_begin,1},
        {"__draw",js_cap_draw,2},{"__commit",js_cap_commit,0},{"__tone",js_tone,1}};
    for(unsigned i=0;i<sizeof fns/sizeof fns[0];i++)
        JS_SetPropertyStr(ctx,g,fns[i].n,JS_NewCFunction(ctx,fns[i].f,fns[i].n,fns[i].a));
    JS_FreeValue(ctx,g);
    eval(PRELUDE,strlen(PRELUDE),"prelude.js");
    JS_RunGC(rt);
    const size_t before=cur_bytes;peak_bytes=cur_bytes;
    if(getenv("DERBY_COMPILE_ONLY")){
        /* The compile alone: its peak and what the compiled script keeps. */
        JSValue fn=JS_Eval(ctx,src,n,path,JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
        const size_t peak=peak_bytes;JS_RunGC(rt);
        printf("compile %s: source %zu B, peak +%zu, compiled +%zu\n",JS_IsException(fn)?"FAILED":"ok",n,
               peak-before,cur_bytes-before);
        JS_FreeValue(ctx,fn);return 0;
    }
    edge("enter",true);                     /* the key that launched it */
    bool ok=eval(src,n,path);
    const size_t eval_peak=peak_bytes;
    present(0);
    JS_RunGC(rt);
    const size_t after_eval=cur_bytes;
    printf("%s eval; source %zu B\n",ok?"ok  ":"FAIL",n);
    if(!ok)return 1;
    if(getenv("DERBY_EVAL_ONLY")){
        JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);JS_DumpMemoryUsage(stdout,&m,rt);
        printf("guest heap (TLSF model): before eval %zu, peak during eval %zu (+%zu), after eval %zu (+%zu)\n",
               before,eval_peak,eval_peak-before,after_eval,after_eval-before);
        return 0;
    }
    edge("enter",false);
    REQ(loaded_seen==1);                    /* the stored points and race number */
    /* Paddock: let the plans load and the odds settle, cycle the tier. */
    for(unsigned i=0;i<20;i++)frame(0);
    unsigned taps=tier_env==1?3:(unsigned)(tier_env+2)%3;   /* tab cycles LIGHT MID HEAVY */
    for(unsigned i=0;i<taps;i++)tap("tab");
    for(unsigned i=0;i<30;i++)frame(0);
    ppm("pad");
    tap("d");tap("d");tap("e");tap("s");tap("e");
    for(unsigned i=0;i<4;i++)frame(0);
    tap("1");
    run_until("gate",10);
    for(unsigned i=0;i<6;i++)frame(0);
    ppm("gate");
    run_until("race",400);
    frame(0);frame(0);ppm("start");
    /* Race: WIDE, then CLOSE on the pick, FIELD, WIDE again. */
    bool full=tier_env==1;
    unsigned lead_shot=0;
    while(!strcmp(scene,"race")){
        if(scene_t==200)edge("/",true);
        if(scene_t==201)edge("/",false);
        if(scene_t==450)edge("/",true);
        if(scene_t==451)edge("/",false);
        if(scene_t==650)edge("/",true);
        if(scene_t==651)edge("/",false);
        unsigned leads=lead_logs;
        frame(0);
        if(scene_t==120)ppm("wide");
        if(scene_t==300)ppm("close");
        if(scene_t==520)ppm("field");
        if(leads!=lead_logs&&!lead_shot&&scene_t>660){ppm("lead");lead_shot=1;}
        if(slow_at&&tick==slow_at+14)ppm("slow");
        if(scene_t>3000)FAIL("race did not end");
    }
    REQ(!strcmp(scene,"photo"));
    for(unsigned i=0;i<14;i++)frame(0);
    ppm("photo");
    for(unsigned i=0;i<60;i++)frame(0);
    ppm("photo_zoom");
    run_until("res",200);
    for(unsigned i=0;i<40;i++)frame(0);
    ppm("result");
    if(full){
        /* Replay: same seed, no camera changes; it must finish identically. */
        tap("r");
        run_until("race",400);
        run_until("photo",3000);
        run_until("res",200);
        REQ(finishes==2);
        if(strcmp(finish[0],finish[1]))FAIL("replay differs:\n  %s\n  %s",finish[0],finish[1]);
        for(unsigned i=0;i<10;i++)frame(0);
        tap("1");
        run_until("pad",10);
        for(unsigned i=0;i<40;i++)frame(0);
        frame(0x2000);                      /* Back: the save turn */
        REQ(saves==1);
    }
    if(getenv("DERBY_MEMDUMP")){JS_RunGC(rt);JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);JS_DumpMemoryUsage(stdout,&m,rt);}
    JSValue gs=JS_GetGlobalObject(ctx),store=JS_GetPropertyStr(ctx,gs,"__store"),rec=JS_GetPropertyStr(ctx,store,"derby.v1");
    JSValue js=JS_JSONStringify(ctx,rec,JS_UNDEFINED,JS_UNDEFINED);const char *jss=JS_ToCString(ctx,js);
    printf("stored %s\n",jss?jss:"?");
    if(jss)JS_FreeCString(ctx,jss);
    JS_FreeValue(ctx,js);JS_FreeValue(ctx,rec);JS_FreeValue(ctx,store);JS_FreeValue(ctx,gs);

    printf("\nscene  frames draws_max seg_max seg_avg raster_max raster_avg draw_raster_max draw_steps_max "
           "frame_steps_max plans_live_max instr_max points_max\n");
    for(unsigned i=0;i<nstats;i++){
        const scene_stats *s=&stats[i];
        printf("%-6s %6u %9u %7u %7.1f %10u %10.0f %15u %14u %15u %14u %9u %10u\n",s->name,s->frames,s->draws_max,
               s->seg_max,(double)s->seg_sum/s->frames,s->raster_max,(double)s->raster_sum/s->frames,
               s->draw_raster_max,s->draw_steps_max,s->frame_steps_max,s->live_max,s->instr_max,s->points_max);
    }
    unsigned seg=0,ras=0,dras=0,dst=0,pts=0;
    for(unsigned i=0;i<nstats;i++){
        if(stats[i].seg_max>seg)seg=stats[i].seg_max;
        if(stats[i].raster_max>ras)ras=stats[i].raster_max;
        if(stats[i].draw_raster_max>dras)dras=stats[i].draw_raster_max;
        if(stats[i].draw_steps_max>dst)dst=stats[i].draw_steps_max;
        if(stats[i].points_max>pts)pts=stats[i].points_max;
    }
    printf("\nlimits used (max over the run): segments %u/1024 (%.0f%%), raster %u/65535 (%.0f%%), "
           "draw raster %u/8192 (%.0f%%), draw steps %u/10000 (%.0f%%), instructions %u/64, registers %u/16, "
           "loop depth %u/8, inputs %u/8, live plans %u/32, typed points per frame %u (one batch <=128), "
           "point coords %d..%d (-480..720), surfaces 1/2\n",
           seg,100.0*seg/1024,ras,100.0*ras/65535,dras,100.0*dras/8192,dst,100.0*dst/10000,g_instr_max,g_regs_max,
           g_depth_max,g_inputs_max,live_max,pts,pt_lo,pt_hi);
    static const char *const OPN[]={"SET","INPUT","ADD","MUL","SIN","REPEAT","END","MOVE","PLOT","LINE",
        "REPEAT_REG","BREAK_IF_GT","PLOT_COLOR_REG","LINE_COLOR_REG","CUBIC"};
    printf("ops used:");for(unsigned o=0;o<15;o++)if(ops_used&(1u<<o))printf(" %s",OPN[o]);
    printf("\nops unused:");for(unsigned o=0;o<15;o++)if(!(ops_used&(1u<<o)))printf(" %s",OPN[o]);
    printf("\nplans: registered %u, unregistered %u, live max %u, most registrations in one frame %u\n",
           registered,unregistered,live_max,frame_reg_max);
    printf("frames %u (procedural frames checked %u): exceptions=%u bad_present=%u failures=%u tones=%u\n",
           tick,frames_checked,exceptions,bad_present,framefails,tones);
    printf("finish: %s\n",finishes?finish[0]:"-");
    if(finishes>1)printf("replay: %s\n",finish[1]);
    printf("pixels hash %016llx\n",(unsigned long long)pixel_hash);
    printf("guest heap (TLSF model, %zu-bit host): before eval %zu, peak during eval %zu (+%zu), after eval %zu, "
           "live peak after GC %zu, whole-run peak %zu; device limit 163840\n",sizeof(void *)*8,before,eval_peak,
           eval_peak-before,after_eval,heap_live_peak,peak_bytes);
    bool pass=!exceptions&&!bad_present&&!framefails&&ops_used==0x7fffu&&frame_reg_max<=1&&go_seen>=1;
    if(full)pass=pass&&finishes==2&&saves==1&&tones>0;
    if(csv)fclose(csv);
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",pass?"DERBY_HOST PASS":"DERBY_HOST FAIL");
    return pass?0:1;
}
