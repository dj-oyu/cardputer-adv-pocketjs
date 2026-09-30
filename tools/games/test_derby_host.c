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
 * The demo (attract) mode, before the scripted game at MID: the idle clock
 * is pocket.time.now(), which this harness drives (33.3 ms a frame, 100 ms
 * to stand for dropped frames, or one jump), so 14.9 s and 15 s are exact.
 * Three whole demos, then one cut short by a key in each scene, and a Back
 * during a demo at the end; the player's points, race, pick, stake, next
 * seed and the store must come through untouched, no tone, cue or
 * storage.set may happen inside a demo, and the DEMO node (read from the
 * panel) must blink inside a demo and be absent outside it. The curtain
 * (a black rect faded by the wall clock): the idle paddock darkens over the
 * last 500 ms before the switch, the switch frame is black but for DEMO, and
 * after a key the paddock is black on the key's frame (the game already back)
 * and untouched 300 ms on; its frames go to the dissolve contact sheet.
 *
 * The big screen (docs/apps/derby-watch.md "大型画面と演出カメラ"): the 'vis'
 * draw must fill exactly the face's interior row by row and ring it one pixel
 * outside; every segment drawn after it must lie wholly on the face (the
 * feed) or put no pixel on it (what stands in front of the screen); the
 * view's lettering nodes (the refs clipped to the whole panel) must lie on
 * the face of the frame they are shown with. The demo races go through the
 * director and the screen too, and are checked the same way.
 *
 * The panning units (WIDE far from the leader, apps/derby/README.md
 * "首振りカメラ"): each point their VM series (prail, t0/t1, pk) emit is
 * compared, unrounded, with the same program run in double with exact
 * reciprocals (Newton's error, <0.1 px on the panel); a panning unit's view
 * of the screen gets the face checks too (tag 3, the rect inside the face).
 * Their frames are counted as scene "pan". Env DERBY_PAN=a[,b,c] sets the
 * app's PAN, DERBY_JS runs a script after the app's, DERBY_PANFACE=1 checks
 * only the panning units' faces (the screen moved), DERBY_SHOTS=tick:tag,...
 * writes panels at those ticks, DERBY_NDEBUG=1 prints each new worst point.
 *
 *   python3 tools/games/run_derby.py            (WSL)
 * Env: DERBY_TIER=0|2 (LIGHT or HEAVY, no replay), DERBY_PPM=<dir>,
 *      DERBY_CSV=<file>, DERBY_HEAP_LIMIT=<bytes>, DERBY_NOCAM=1 (no camera
 *      keys: the director alone), DERBY_PICK=<0..7>,
 *      DERBY_NORMAL=<race>,<pick> (play that race by hand with that pick and
 *      stop at the result: the reference a demo race must match),
 *      DERBY_HW=<uint32> (what pocket.random.seed() returns; fixed by default).
 */
#include "pocket_kasane.h"
#include "pocket_proc.h"
#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "keymap.h"
#include "app_chunks_host.h"
#include "app_registry.h"
#include "pocket_app_load.h"
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
#include <time.h>
#include "pocket_sub_impl.inc"

/* ---- the guest allocator as the device charges it (TLSF lengths), as in
 * tools/kasane_contract/test_megademo_app_host.c. */
typedef struct { size_t n; size_t pad[3]; } pk_hdr;
static size_t cur_bytes,peak_bytes;
/* The churn of the game's frame() alone (__churn(1) .. __churn(0) around
 * it): allocations and reallocations, their bytes, and the high water above
 * the heap at the start of the call (docs/apps/derby-pan-memory.md). */
static size_t n_alloc,b_alloc,turn_hi;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;
    if(cur_bytes>turn_hi)turn_hi=cur_bytes;}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;n_alloc++;b_alloc+=tlsf_len(n);
    pk_count(tlsf_len(n),0);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;pk_count(0,tlsf_len(h->n));free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    n_alloc++;b_alloc+=tlsf_len(n);
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
static unsigned tick,exceptions,bad_present,framefails,screen_seen,vision_shot,wide_shot,head_at;
static char scene[16]="boot";
static unsigned scene_t;
static char finish[3][512];
static unsigned finishes,loaded_seen,saves,go_seen,slow_at,lead_logs;
/* Demo bookkeeping, fed by the DERBY DEMO / SCENE / PICK / TIER logs. */
static bool in_demo;
static unsigned demo_starts,demo_ends,demo_end_tick,picks,picks_at,tiers,tiers_at,results;
static double now_ms=1000,dt_ms=1000.0/30,frame_ms,demo_start_ms;
static char demo_start[160],demo_end[160],demo_path[96],demo_finish[3][512],demo_pick[3][96],demo_result[3][128];
static char pad_seed[64],save_line[128],last_pick[96],last_result[128];
static double demo_end_ms;
static double at_start[3];                  /* storage.set calls, tones, cues when the demo began */
static unsigned demo_on,demo_off,demo_px_outside;
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
static ksn_proc_frame f_plan,f_debug,f_vm,cand_plan,cand_vm,front;
static uint16_t pix_plan[240*135],pix_vm[240*135],mask[240*135];
static uint16_t cur_bg;
/* The big screen: the 'vis' plan fills its face (the app's vrect()) and
 * rings it with the bezel; the feed is drawn over the face; the view's
 * lettering follows it. */
static unsigned cur_surface,s1_frames,s0_frames,vis_drawn,vis_seg_end,vis_frames,occluded_frames,
                occluded_pixels,rect_checked,rect_mismatch,refs_max,cmds_max,bezel_bad,feed_segments,
                feed_frames,overlay_frames;
static int vis_in[4],last_vis_in[4];
static unsigned head_frames;
static bool last_vis;
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
/* __reg(handle, rows, points) for a plan registered as rows, or
 * __reg(handle, name, args, points) for a built-in plan by name
 * (docs/kasane/flash-plan.md): its rows are the table's with the arguments
 * written in, the program the array path would have registered. */
static JSValue js_cap_reg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;
    spec *s=NULL;
    for(unsigned i=0;i<SLOTS&&!s;i++)if(!specs[i].handle)s=&specs[i];
    REQ(s);
    memset(s,0,sizeof *s);
    s->handle=(int)num(argv[0]);
    unsigned n;
    JSValueConst pts=JS_UNDEFINED;
    if(JS_IsString(argv[1])){
        const char *name=JS_ToCString(ctx,argv[1]);REQ(name);
        const ksn_proc_rom_entry *e=NULL;
        for(unsigned i=0;i<ksn_proc_rom_plans_count&&!e;i++)
            if(!strcmp(ksn_proc_rom_plans[i].name,name))e=&ksn_proc_rom_plans[i];
        JS_FreeCString(ctx,name);
        REQ(e);
        unsigned given=JS_IsArray(argv[2])?len(argv[2]):0;
        REQ(given==e->params);
        float arg[KSN_PROC_ROM_PARAMS];
        for(unsigned i=0;i<given;i++)arg[i]=(float)at(argv[2],i);
        n=e->count;
        memcpy(s->code,e->code,n*sizeof *s->code);
        const ksn_proc_binding b={e->patch,arg,e->patches,(uint8_t)given};
        REQ(ksn_proc_apply_binding(s->code,(uint8_t)n,&b));
        if(argc>3)pts=argv[3];
    }else{
        n=len(argv[1]);
        REQ(n>=1&&n<=KSN_PROC_CODE);
        for(unsigned i=0;i<n;i++){
            JSValue row=JS_GetPropertyUint32(ctx,argv[1],i);
            REQ(len(row)==6);
            s->code[i]=(ksn_proc_inst){(uint8_t)at(row,0),(uint8_t)at(row,1),(uint8_t)at(row,2),(uint8_t)at(row,3),
                                       (float)at(row,4),(uint16_t)at(row,5)};
            JS_FreeValue(ctx,row);
        }
        if(argc>2)pts=argv[2];
    }
    s->program=(ksn_proc_program){s->code,(uint8_t)n};
    REQ(ksn_proc_plan_prepare(&s->plan,&s->program));
    analyse(s);
    if(JS_IsObject(pts)){
        JSValueConst p=pts;
        JSValue x=JS_GetPropertyStr(ctx,p,"x"),y=JS_GetPropertyStr(ctx,p,"y"),
                k=JS_GetPropertyStr(ctx,p,"coeff"),col=JS_GetPropertyStr(ctx,p,"color");
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
    cur_surface=argc>1&&num(argv[1])?1u:0u;
    vis_drawn=0;
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
/* The panning units' series plans (prail, t0/t1, pk; tag 4): every point the
 * float VM emits against the same program run in double, where each Newton
 * step t=z*r; t=t+2; r'=r*t (operands in either order) is replaced by the
 * exact r'=-1/z and the inputs are the JS doubles (not rounded to float):
 * the pinhole-and-yaw projection the app set up, exactly. Compared before
 * the VM rounds to pixels. */
static double newton_max,newton_max_f,newton_screen,pan_f_lo=1e9,pan_f_hi,cur_pan_f;
static unsigned long newton_points;
static unsigned newton_draws,pan_frames,pan_face_frames;
static unsigned exact_run(const spec *s,const double *in,double *ex,double *ey,unsigned cap){
    double r[KSN_PROC_REGS]={0};
    struct {unsigned pc,left;} st[KSN_PROC_LOOP_DEPTH];
    unsigned depth=0,n=0,pc=0,end[KSN_PROC_CODE]={0},open[KSN_PROC_LOOP_DEPTH],od=0;
    const ksn_proc_inst *c=s->code;const unsigned count=s->program.count;
    for(unsigned i=0;i<count;i++){
        if(c[i].op==KSN_PROC_REPEAT||c[i].op==KSN_PROC_REPEAT_REG)open[od++]=i;
        else if(c[i].op==KSN_PROC_END){REQ(od);end[open[--od]]=i;}
    }
    while(pc<count){
        const ksn_proc_inst *i=&c[pc];
        switch(i->op){
        case KSN_PROC_SET:r[i->dst]=i->value;break;
        case KSN_PROC_INPUT:r[i->dst]=in[i->a];break;
        case KSN_PROC_ADD:r[i->dst]=r[i->a]+r[i->b];break;
        case KSN_PROC_SIN:r[i->dst]=sin(r[i->a]);break;
        case KSN_PROC_MUL:
            if(pc+2<count&&c[pc+1].op==KSN_PROC_ADD&&c[pc+1].dst==i->dst&&c[pc+2].op==KSN_PROC_MUL&&i->a!=i->dst&&i->b!=i->dst){
                const ksn_proc_inst *a=&c[pc+1],*m=&c[pc+2];
                const int two=a->a==i->dst?a->b:a->b==i->dst?a->a:-1;
                const int q=m->b==i->dst?m->a:m->a==i->dst?m->b:-1;
                if(two>=0&&r[two]==2.0&&(q==i->a||q==i->b)){
                    const unsigned z=q==i->a?i->b:i->a;
                    r[m->dst]=-1.0/r[z];r[i->dst]=1.0;pc+=3;continue;
                }
            }
            r[i->dst]=r[i->a]*r[i->b];break;
        case KSN_PROC_REPEAT:case KSN_PROC_REPEAT_REG:{
            const unsigned k=i->op==KSN_PROC_REPEAT?i->a:(unsigned)r[i->a];
            if(!k){pc=end[pc]+1;continue;}
            REQ(depth<KSN_PROC_LOOP_DEPTH);st[depth].pc=pc;st[depth].left=k;depth++;break;
        }
        case KSN_PROC_END:REQ(depth);if(--st[depth-1].left){pc=st[depth-1].pc+1;continue;}depth--;break;
        case KSN_PROC_BREAK_IF_GT:REQ(depth);if(r[i->a]>r[i->b]){pc=end[st[depth-1].pc]+1;depth--;continue;}break;
        case KSN_PROC_MOVE:case KSN_PROC_PLOT:case KSN_PROC_LINE:case KSN_PROC_PLOT_COLOR_REG:case KSN_PROC_LINE_COLOR_REG:
            REQ(n<cap);ex[n]=r[i->a];ey[n]=r[i->b];n++;break;
        default:REQ(!"an op the pan plans do not use");
        }
        pc++;
    }
    return n;
}
static JSValue js_cap_draw(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    spec *s=find((int)num(argv[0]));REQ(s);
    const unsigned tag=argc>2?(unsigned)num(argv[2]):0;
    static float vx[4096],vy[4096];
    static double ex[4096],ey[4096];
    unsigned vn=0;
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
    do{
        if(tag==4){
            const ksn_proc_inst *ci=&vr.program->code[vr.pc];
            if(ci->op==KSN_PROC_MOVE||ci->op==KSN_PROC_PLOT||ci->op==KSN_PROC_LINE||ci->op==KSN_PROC_PLOT_COLOR_REG||
               ci->op==KSN_PROC_LINE_COLOR_REG){REQ(vn<4096);vx[vn]=vr.reg[ci->a];vy[vn]=vr.reg[ci->b];vn++;}
        }
        st=ksn_proc_step(&vr);singles++;
    }while(st==KSN_PROC_RUNNING);
    REQ(st==KSN_PROC_DONE);
    if(tag==4){
        double in64[KSN_PROC_INPUTS]={0};
        for(unsigned i=0;i<ni;i++)in64[i]=at(argv[1],i);
        REQ(exact_run(s,in64,ex,ey,4096)==vn);
        const double f=argc>3?num(argv[3]):0;
        for(unsigned i=0;i<vn;i++){
            const double e=fmax(fabs(vx[i]-ex[i]),fabs(vy[i]-ey[i]));
            if(e>newton_max){newton_max=e;newton_max_f=f;}
            if(ex[i]>=0&&ex[i]<240&&ey[i]>=0&&ey[i]<135&&e>newton_screen){newton_screen=e;
                if(getenv("DERBY_NDEBUG"))printf("NEWTON %.4f plan %u pt %u/%u f %.0f in %g %g %g %g %g %g %g %g ex %.2f %.2f\n",e,s->program.count,i,vn,f,in64[0],in64[1],in64[2],in64[3],in64[4],in64[5],in64[6],in64[7],ex[i],ey[i]);}
        }
        newton_points+=vn;newton_draws++;
    }
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
    if(tag==2&&!cur_surface)head_frames++;
    if(tag==3){
        /* A panning unit's view of the screen: its bezel (hl, the face's two
         * edges joined) closes the face; the rect inside it is the app's vr
         * rule, recomputed here from the edges [x, 1/Z'] of the draw, held
         * as the vis inputs are (vr less one on the left and top). */
        REQ(!cur_surface&&!vis_drawn);
        vis_drawn=1;vis_seg_end=cand_plan.count;pan_face_frames++;
        const double xl=at(argv[1],0),rl=at(argv[1],1),xr=at(argv[1],2),rr=at(argv[1],3);
        vis_in[0]=(int)floor(fmin(xl,xr)+.5)-1;vis_in[1]=(int)ceil(28-10*fmin(rl,rr))-1;
        vis_in[2]=(int)floor(fmax(xl,xr)+.5);vis_in[3]=28;
    }
    if(tag==1){
        REQ(!cur_surface&&!vis_drawn&&f_plan.count>=4);
        vis_drawn=1;vis_seg_end=cand_plan.count;
        for(unsigned i=0;i<4;i++)vis_in[i]=(int)at(argv[1],i);
        /* The face is filled row by row, exactly its interior, then the first
         * bezel ring runs one pixel outside it. */
        const ksn_proc_segment *g=f_plan.segments;
        const int a=vis_in[0],b=vis_in[1],c2=vis_in[2],d=vis_in[3],rows=d-b-1;
        REQ(rows>0&&(unsigned)rows+4<=f_plan.count);
        for(int r=0;r<rows;r++)
            if(!(g[r].x0==a+1&&g[r].x1==c2-1&&g[r].y0==b+1+r&&g[r].y1==b+1+r))bezel_bad++;
        g+=rows;
        if(!(g[0].x0==a&&g[0].y0==b&&g[0].x1==c2&&g[0].y1==b&&g[1].x1==c2&&g[1].y1==d&&
             g[2].x1==a&&g[2].y1==d&&g[3].x1==a&&g[3].y1==b))bezel_bad++;
    }
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
    (void)c;(void)self;
    /* argv[0]: the panning unit's f, 0 for a side unit. */
    cur_pan_f=argc?num(argv[0]):0;
    if(cur_pan_f>0){pan_frames++;if(cur_pan_f<pan_f_lo)pan_f_lo=cur_pan_f;if(cur_pan_f>pan_f_hi)pan_f_hi=cur_pan_f;}
    REQ(cand_plan.count==cand_vm.count&&!memcmp(cand_plan.segments,cand_vm.segments,cand_plan.count*sizeof cand_plan.segments[0]));
    cand_plan.ready=cand_vm.ready=true;
    for(unsigned i=0;i<240*135;i++)pix_plan[i]=pix_vm[i]=cur_bg;
    REQ(ksn_proc_render_band(&cand_plan,pix_plan,0,135));
    for(int y=0;y<135;y+=8)REQ(ksn_proc_render_band(&cand_vm,pix_vm+y*240,y,y+8<=135?8:135-y));
    REQ(!memcmp(pix_plan,pix_vm,sizeof pix_plan));
    for(size_t i=0;i<sizeof pix_plan;i++){pixel_hash^=((const unsigned char *)pix_plan)[i];pixel_hash*=1099511628211ull;}
    frames_checked++;
    if(cur_surface)s1_frames++;
    else{
        s0_frames++;
        last_vis=vis_drawn;
        /* DERBY_PANFACE: the screen was moved (DERBY_JS) for a panning unit
         * to see it; the side units' faces are checked in the other runs. */
        if(vis_drawn&&(!getenv("DERBY_PANFACE")||cur_pan_f>0)){
            memcpy(last_vis_in,vis_in,sizeof vis_in);vis_frames++;
            /* After the bezel come the feed (every segment wholly inside the
             * face) and the objects in front of the screen. Rasterize every
             * segment that is not wholly inside and count its pixels on the
             * face: a front object over the screen, or the feed leaking. */
            const int X0=vis_in[0]+1,Y0=vis_in[1]+1,X1=vis_in[2]-1,Y1=vis_in[3]-1;
            memset(&front,0,sizeof front);
            unsigned inside=0;
            for(unsigned i=vis_seg_end;i<cand_plan.count;i++){
                ksn_proc_segment g=cand_plan.segments[i];
                if(g.x0>=X0&&g.x0<=X1&&g.x1>=X0&&g.x1<=X1&&g.y0>=Y0&&g.y0<=Y1&&g.y1>=Y0&&g.y1<=Y1){inside++;continue;}
                g.color=0xffff;front.segments[front.count++]=g;
            }
            feed_segments+=inside;if(inside)feed_frames++;
            front.ready=true;
            memset(mask,0,sizeof mask);
            REQ(ksn_proc_render_band(&front,mask,0,135));
            unsigned hit=0;
            for(int y=vis_in[1]+1;y<vis_in[3];y++)for(int x=vis_in[0]+1;x<vis_in[2];x++)
                if(x>=0&&x<240&&y>=0&&y<135&&mask[y*240+x])hit++;
            if(hit){occluded_frames++;occluded_pixels+=hit;
                if(occluded_frames<=5)printf("  on the face: tick %u scene %s face %d,%d..%d,%d %u px\n",tick,scene,
                                             vis_in[0]+1,vis_in[1]+1,vis_in[2],vis_in[3],hit);}
        }
    }
    scene_stats *st=stat_for(cur_surface?"feed":cur_pan_f>0?"pan":scene);
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
    if(csv)fprintf(csv,"%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",tick,scene,cur_surface,vis_drawn,frame_draws,cand_plan.count,
                   cand_plan.raster_steps,draw_raster_max,draw_steps_max,frame_steps,live_plans,frame_points,
                   frame_instr_max,frame_regs);
    draw_raster_max=draw_steps_max=0;
    return JS_UNDEFINED;
}

/* ---- logs: the app's DERBY markers drive the script */
static unsigned tones;
static double gnum(const char *name){
    JSValue g=JS_GetGlobalObject(ctx),v=JS_GetPropertyStr(ctx,g,name);double d=num(v);
    JS_FreeValue(ctx,v);JS_FreeValue(ctx,g);return d;
}
/* __hr(): wall time in us, for profiling the guest's functions from a
 * DERBY_JS wrapper (host only; the device has no such clock). */
static JSValue js_hr(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return JS_NewFloat64(c,t.tv_sec*1e6+t.tv_nsec*1e-3);
}
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
        if(in_demo&&strlen(demo_path)+k+2<sizeof demo_path){strcat(demo_path," ");strcat(demo_path,scene);}
        /* The next race the player would run: race number and seed. */
        const char *r=strstr(s," race=");
        if(!in_demo&&!strcmp(scene,"pad")&&r){const char *e=strstr(r," plans=");
            snprintf(pad_seed,sizeof pad_seed,"%.*s",e?(int)(e-r):(int)strlen(r),r);}
    }
    if(!strncmp(s,"DERBY DEMO START ",17)){
        REQ(!in_demo);in_demo=true;demo_starts++;demo_start_ms=frame_ms;demo_path[0]=0;
        snprintf(demo_start,sizeof demo_start,"%s",s+17);
        at_start[0]=gnum("__sets");at_start[1]=tones;at_start[2]=gnum("__cues");
    }
    if(!strncmp(s,"DERBY DEMO END ",15)){
        REQ(in_demo);in_demo=false;demo_ends++;demo_end_tick=tick;demo_end_ms=frame_ms;
        snprintf(demo_end,sizeof demo_end,"%s",s+15);
        /* Silent and unsaved: nothing reached the speaker or the store. */
        REQ(gnum("__sets")==at_start[0]&&tones==at_start[1]&&gnum("__cues")==at_start[2]);
        /* The player's points, race, pick and stake, exactly as put aside. */
        if(strcmp(demo_start,demo_end))FAIL("demo changed the player:\n  start %s\n  end   %s",demo_start,demo_end);
    }
    if(!strncmp(s,"DERBY PICK ",11)){picks++;picks_at=tick;
        if(in_demo){if(demo_starts<=3)snprintf(demo_pick[demo_starts-1],sizeof demo_pick[0],"%s",s+6);}
        else snprintf(last_pick,sizeof last_pick,"%s",s+6);}
    if(!strncmp(s,"DERBY RESULT ",13)){results++;
        if(in_demo){if(demo_starts<=3)snprintf(demo_result[demo_starts-1],sizeof demo_result[0],"%s",s+6);}
        else snprintf(last_result,sizeof last_result,"%s",s+6);}
    if(!strncmp(s,"DERBY TIER",10)){tiers++;tiers_at=tick;}
    if(!strncmp(s,"DERBY FINISH ",13)){
        if(in_demo){if(demo_starts<=3)snprintf(demo_finish[demo_starts-1],sizeof demo_finish[0],"%s",s+13);}
        else if(finishes<3)snprintf(finish[finishes++],sizeof finish[0],"%s",s+13);
    }
    if(!strncmp(s,"DERBY LOADED",12))loaded_seen++;
    if(!strncmp(s,"DERBY SAVE",10)){saves++;snprintf(save_line,sizeof save_line,"%s",s+6);}
    if(!strncmp(s,"DERBY GO",8)){go_seen++;if(strstr(s,"LOADSTALL"))framefails++;}
    if(!strncmp(s,"DERBY SLOW",10))slow_at=tick;
    if(!strncmp(s,"DERBY LEAD",10))lead_logs++;
    if(!strncmp(s,"DERBY FRAMEFAIL",15)||!strncmp(s,"DERBY LOADFAIL",14))framefails++;
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
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

/* The DEMO text node's box and colour (0xfffb96ff in RGB565), read back from
 * the panel: the only way to see what the view actually showed. */
#define DEMO_PX 0xffd2u
static unsigned demo_pixels(void){
    unsigned n=0;
    for(int y=109;y<120;y++)for(int x=212;x<238;x++)n+=panel[y*240+x]==DEMO_PX;
    return n;
}
/* The curtain (docs/apps/derby-watch.md "デモの幕"). Pixels read back from the
 * panel: yellow HUD text (the paddock's bottom line is 0xfffb96ff) survives
 * only where no curtain darkens it; a raised curtain leaves nothing lit but
 * the DEMO node's box. */
static unsigned yellow_rows(int y0,int y1){
    unsigned n=0;
    for(int y=y0;y<y1;y++)for(int x=0;x<240;x++)n+=panel[y*240+x]==DEMO_PX;
    return n;
}
static unsigned lit_outside_demo(void){
    unsigned n=0;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)
        n+=panel[y*240+x]!=0&&!(y>=109&&y<120&&x>=212&&x<238);
    return n;
}
/* Contact-sheet frames, ms from the switch into the demo: the idle paddock
 * going to black, held black, the demo's paddock coming in. */
static const double FX_AT[]={-600,-450,-300,-150,0,350,700,800,900,1034};
static unsigned fx_next,curtain_frames;
static void fx_shot(double ms){
    char tag[8];
    while(fx_next<sizeof FX_AT/sizeof FX_AT[0]&&ms>=FX_AT[fx_next]-1e-6){
        snprintf(tag,sizeof tag,"fx%02u",++fx_next);ppm(tag);
    }
}
static unsigned refs_pad_max;
/* After every turn: the screen node's state against the bezel of the surface-0
 * frame on show (a surface-1 turn leaves both as they were), and the view's
 * refs and commands. */
static int probe_w;
/* Per scene: frames, allocations, bytes, and the largest rise of one call. */
static const char *const CHURN_SCENE[]={"pad","gate","race","photo","res"};
static struct { unsigned frames; size_t n,b,hi,gc; } churn[5];
static size_t churn_n,churn_b,churn_base;
static JSValue js_churn(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    if(num(argv[0])){churn_n=n_alloc;churn_b=b_alloc;churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;}
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){
        churn[i].frames++;churn[i].n+=n_alloc-churn_n;churn[i].b+=b_alloc-churn_b;
        if(turn_hi-churn_base>churn[i].hi)churn[i].hi=turn_hi-churn_base;
    }
    return JS_UNDEFINED;
}
static JSValue js_probe(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    unsigned refs=(unsigned)num(argv[1]),cmds=(unsigned)num(argv[2]);
    if(refs>refs_max)refs_max=refs;
    if(!strcmp(scene,"pad")&&refs>refs_pad_max)refs_pad_max=refs;
    if(cmds>cmds_max)cmds_max=cmds;
    /* argv[0]: [visible, x0, y0, x1, y1] per lettering node. Shown only in a
     * frame whose face was drawn, and then inside the face. */
    const unsigned n=len(argv[0])/5;
    probe_w=last_vis?last_vis_in[2]-last_vis_in[0]-1:0;
    bool any=false;
    for(unsigned k=0;k<n;k++){
        if(!at(argv[0],5*k))continue;
        any=true;rect_checked++;
        const int x0=(int)at(argv[0],5*k+1),y0=(int)at(argv[0],5*k+2),x1=(int)at(argv[0],5*k+3),y1=(int)at(argv[0],5*k+4);
        if(!last_vis||x0<last_vis_in[0]+1||y0<last_vis_in[1]+1||x1>last_vis_in[2]||y1>last_vis_in[3]){
            rect_mismatch++;
            if(rect_mismatch<=5)printf("  lettering off the face: tick %u scene %s node %d,%d..%d,%d face %d,%d..%d,%d (%s)\n",
                tick,scene,x0,y0,x1,y1,last_vis_in[0]+1,last_vis_in[1]+1,last_vis_in[2],last_vis_in[3],last_vis?"drawn":"not drawn");
        }
    }
    if(any)overlay_frames++;
    return JS_UNDEFINED;
}

/* One turn: pump keys, frame(buttons), present. */
static void frame(unsigned buttons){
    pocket_input_pump(0);
    frame_regs=0;
    frame_ms=round(now_ms*1000)/1000;   /* what the guest reads (%.3f) */
    char call[256];snprintf(call,sizeof call,"__turn=1;__now=%.3f;__churn(1);frame(%u);__churn(0);__probe([].concat.apply([],__ov.map(r=>"
                            "[r.__v?1:0].concat(r.__r||[0,0,0,0]))),__refs,kasane.stats().displayed.commands)",now_ms,buttons);
    now_ms+=dt_ms;
    eval(call,strlen(call),"frame.js");
    if(frame_regs>frame_reg_max)frame_reg_max=frame_regs;
    present((uint64_t)tick*33333u);
    /* DERBY_SHOTS=<tick>:<tag>,...: panels at those ticks (run_derby.py
     * --ppm: the side-only run at the ticks of the panning shots). */
    for(const char *p=getenv("DERBY_SHOTS");p&&*p;){
        unsigned k=0;char tag[32]={0};
        if(sscanf(p,"%u:%31[^,]",&k,tag)==2&&k==tick)ppm(tag);
        p=strchr(p,',');if(p)p++;
    }
    unsigned dp=demo_pixels();
    if(in_demo){if(dp>20)demo_on++;else{REQ(!dp);demo_off++;}}
    /* The Back turn ends a demo without drawing: the app is leaving. */
    else if(dp&&!(buttons&0x2000))demo_px_outside++;
    size_t h=heap_used();
    JS_RunGC(rt);
    /* What only the cycle collector frees: on the device it waits until the
     * heap is near the limit (quickjs.c js_trigger_gc), so a cycle made every
     * frame or plan stays resident there while this harness frees it here. */
    {size_t g=heap_used();for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i]))churn[i].gc+=h>g?h-g:0;}
    h=heap_used();if(h>heap_live_peak)heap_live_peak=h;
    tick++;scene_t++;
}
static void tap(const char *k){edge(k,true);frame(0);edge(k,false);frame(0);}
static void run_until(const char *s,unsigned limit){
    unsigned n=0;
    while(strcmp(scene,s)){frame(0);if(++n>limit)FAIL("scene %s not reached from %s",s,scene);}
}

/* ---- the demo */
static const char *const DEMO_PATH=" pad gate race photo res";
static char last_end[160];
static void wait_start(void){
    unsigned n0=demo_starts,i=0;
    while(demo_starts==n0){frame(0);if(++i>2000)FAIL("no demo");}
}
/* Idle has been counting since the demo ended: one frame 15 s later starts
 * the next (the clock may jump; only the wall clock counts). */
static void jump_start(void){
    now_ms+=15000;
    unsigned n0=demo_starts;frame(0);REQ(demo_starts==n0+1);
    /* Nothing touched the player between two demos. */
    if(strcmp(demo_start,last_end))FAIL("player changed between demos:\n  %s\n  %s",last_end,demo_start);
}
static size_t pad_heap[16];
static unsigned pad_plans[16],pads;
/* After a demo: back at the paddock with the player's next race. */
static void after_demo(const char *seed0){
    snprintf(last_end,sizeof last_end,"%s",demo_end);
    REQ(!strcmp(scene,"pad"));
    for(unsigned i=0;i<30;i++)frame(0);
    REQ(!strcmp(scene,"pad")&&!in_demo);
    if(strcmp(pad_seed,seed0))FAIL("next race changed: %s -> %s",seed0,pad_seed);
    JS_RunGC(rt);
    if(pads<16){pad_heap[pads]=heap_used();pad_plans[pads++]=live_plans;}
    printf("  after demo %u: plans live %u, guest heap after GC %zu\n",demo_ends,live_plans,heap_used());
}
static void full_demo(const char *seed0,bool shots){
    unsigned e0=demo_ends,i=0;
    while(demo_ends==e0){
        frame(0);
        if(shots){
            /* After the curtain (t 36 and 52: DEMO lit, then blinked off). */
            fx_shot(frame_ms-demo_start_ms);
            if(!strcmp(scene,"pad")&&scene_t==37)ppm("demo_on");
            if(!strcmp(scene,"pad")&&scene_t==53)ppm("demo_off");
            if(!strcmp(scene,"race")&&scene_t==385)ppm("demo_race");
            if(!strcmp(scene,"res")&&scene_t==40)ppm("demo_res");
        }
        if(++i>4000)FAIL("demo did not end");
    }
    REQ(!strcmp(demo_path,DEMO_PATH));
    after_demo(seed0);
}
static void demo_phase(void){
    char seed0[64];snprintf(seed0,sizeof seed0,"%s",pad_seed);
    /* 1. The idle clock: a held key (q, which the game ignores) and a tap
     * shorter than a frame (r, pressed and released between two turns)
     * both restart it; 14.9 s does not start a demo, the 15 s frame does. */
    edge("q",true);frame(0);edge("q",false);
    const double t0=frame_ms;
    while(now_ms<=t0+5000)frame(0);
    ppm("title");
    while(now_ms<=t0+14000)frame(0);
    edge("r",true);edge("r",false);frame(0);
    const double t1=frame_ms;
    /* The curtain comes down over the last FX[0] ms of the idle paddock: the
     * HUD's yellow is untouched 600 ms before the switch, gone in the last
     * frame before it. */
    unsigned y600=0,ylast=0;
    while(now_ms<=t1+14900){
        frame(0);fx_shot(frame_ms-t1-15000);
        if(!y600&&frame_ms-t1>=14400)y600=yellow_rows(122,135)+1;
        ylast=yellow_rows(122,135);
        if(frame_ms-t1>14500)curtain_frames++;
    }
    REQ(!demo_starts&&frame_ms>t0+15000);
    wait_start();
    printf("  demo 1 started %.1f ms after the last key (frame %.1f ms)\n",demo_start_ms-t1,dt_ms);
    REQ(demo_start_ms-t1>=15000-1e-6&&demo_start_ms-t1<15000+dt_ms);
    /* The switch frame: black but for DEMO, which is lit. */
    fx_shot(0);
    const unsigned card=demo_pixels(),stray=lit_outside_demo();
    printf("  curtain: HUD yellow px 600 ms before %u, last idle frame %u (%u idle frames under it); switch frame "
           "DEMO %u px, lit elsewhere %u\n",y600-1,ylast,curtain_frames,card,stray);
    REQ(y600>1&&!ylast&&card>20&&!stray);
    /* 2. A whole demo, normal speed. */
    full_demo(seed0,true);
    /* 3. At 10 fps the idle is still 15 s of wall clock, not 450 frames. */
    dt_ms=100;
    wait_start();
    printf("  demo 2 started %.1f ms after demo 1 ended (frame %.1f ms)\n",demo_start_ms-demo_end_ms,dt_ms);
    REQ(demo_start_ms-demo_end_ms>=15000-1e-6&&demo_start_ms-demo_end_ms<15000+dt_ms+1e-6);
    if(strcmp(demo_start,last_end))FAIL("player changed between demos");
    dt_ms=1000.0/30;
    full_demo(seed0,false);
    jump_start();
    full_demo(seed0,false);
    /* 4. A key anywhere ends the demo that frame, and is spent doing so:
     * d (pick), tab (TIER), the pad bits of frame(buttons), 1 (PICK, or
     * NEXT RACE with a save), e (stake). */
    static const struct {const char *scene;unsigned at;const char *key;unsigned buttons;} CUT[]={
        {"pad",20,"d",0},{"gate",5,"tab",0},{"race",130,NULL,0x80},{"race",0,"1",0},{"photo",30,"e",0},{"res",40,"1",0}};
    for(unsigned c=0;c<sizeof CUT/sizeof CUT[0];c++){
        jump_start();
        const unsigned s0=slow_at;unsigned i=0;
        for(;;){
            bool here=!strcmp(scene,CUT[c].scene)&&(CUT[c].at?scene_t==CUT[c].at:slow_at!=s0&&tick>=slow_at+10);
            if(here)break;
            frame(0);if(++i>4000)FAIL("cut %u: %s not reached",c,CUT[c].scene);
        }
        const unsigned p0=picks,t0_=tiers,e0=demo_ends;const double w0=gnum("__sets");
        if(c==2)ppm("ex1");
        if(CUT[c].key){edge(CUT[c].key,true);frame(0);}else frame(CUT[c].buttons);
        REQ(demo_ends==e0+1&&demo_end_tick==tick-1&&!strcmp(scene,"pad"));
        if(CUT[c].key)edge(CUT[c].key,false);
        /* The game is back this frame; only the picture fades in: black on
         * the key's frame, the HUD untouched FX[2] = 300 ms (9 frames) on. */
        const unsigned lit=lit_outside_demo()+demo_pixels();
        if(c==2)ppm("ex2");
        for(unsigned k=1;k<=10;k++){
            frame(0);
            if(c==2&&(k==3||k==6))ppm(k==3?"ex3":"ex4");
            if(k==9){REQ(yellow_rows(122,135)>0);if(c==2)ppm("ex5");}
        }
        printf("  cut %u: key frame lit px %u\n",c,lit);
        REQ(!lit);
        REQ(picks==p0&&tiers==t0_&&gnum("__sets")==w0&&!strcmp(scene,"pad"));
        printf("  cut %u: %s frame %u, %s\n",c,CUT[c].scene,CUT[c].at,CUT[c].key?CUT[c].key:"buttons 0x80");
        after_demo(seed0);
    }
    /* No growth over the demos: plans and the guest heap at the paddock. */
    for(unsigned i=1;i<pads;i++){
        REQ(pad_plans[i]==pad_plans[0]);
        if(pad_heap[i]>pad_heap[0]+1024)FAIL("guest heap grows over demos: %zu -> %zu",pad_heap[0],pad_heap[i]);
    }
    REQ(demo_on>0&&demo_off>0&&!demo_px_outside);
    for(unsigned i=0;i<3;i++)printf("DEMO_FINISH %u %s\nDEMO_PICK %u %s\nDEMO_RESULT %u %s\n",i+1,demo_finish[i],
                                   i+1,demo_pick[i],i+1,demo_result[i]);
}

/* The device interrupts a source evaluation after 2 s (app_session.c). The
 * host is far faster, so this catches only a loop that never ends, which is
 * what the device hit; without it the harness would hang instead of failing. */
static double eval_deadline;
static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int eval_interrupt(JSRuntime *r,void *u){(void)r;(void)u;return eval_deadline>0&&now_s()>eval_deadline;}

static const char PRELUDE[]=
    "globalThis.console={log:globalThis.__log};"
    "(function(){const P=kasane.procedural,R=P.register,U=P.unregister,B=P.beginFrame,D=P.draw,C=P.commit;"
    "P.register=function(p,q,r){const h=R.apply(P,arguments);typeof p==='string'?__reg(h,p,q,r):__reg(h,p,q);return h};"
    "P.unregister=function(h){U.call(P,h);__unreg(h)};"
    "P.beginFrame=function(c,s){const r=s===undefined?B.call(P,c):B.call(P,c,s);__begin(c,s?1:0);return r};"
    /* Tags: 1 the screen's face (vis), 2 HEAD ON's still, 3 a panning unit's
     * bezel on the screen (hl in 10565), 4 the panning series (Newton). The
     * app's pc (the panning unit, f at 4) is a global of its scripts. */
    "const pf=()=>{try{return pc?pc[4]:0}catch(e){return 0}};"
    "P.draw=function(h,i){try{D.call(P,h,i)}catch(e){console.log('DERBY DRAWFAIL '+Object.keys(derby.L).find(k=>derby.L[k]===h)+' '+JSON.stringify(i));throw e}"
    "const L=globalThis.derby&&derby.L,t=L?h===L.vis?1:h===L.hd?2:h===L.hl&&i[7]===10565?3:"
    "h===L.prail||h===L.t0||h===L.t1||h===L.pk?4:0:0;__draw(h,i,t,t>2?pf():0)};"
    "P.commit=function(){C.call(P);__commit(pf())};})();"
    /* The draw references one replace() exposes (the limit is 32), and the
     * screen's lettering: the refs the app clips to the whole panel (setRect
     * keeps a clip), with their last rect and visibility. */
    "globalThis.__refs=0;globalThis.__ov=[];"
    "(function(){const K=kasane,RP=K.replace,PA=K.patch;"
    "function ref(r){const p=Object.getPrototypeOf(r);if(p.__w)return;p.__w=1;const sr=p.setRect,sv=p.setVisible,sc=p.setClip;"
    "p.setClip=function(t,b){if(!b[0]&&!b[1]&&b[2]===240&&b[3]===135&&!this.__o){this.__o=1;this.__v=true;this.__r=null;__ov.push(this)}return sc.call(this,t,b)};"
    "p.setRect=function(t,b){if(this.__o)this.__r=b.slice();return sr.call(this,t,b)};"
    "p.setVisible=function(t,v){if(this.__o)this.__v=v;return sv.call(this,t,v)}}"
    "function wrap(t){const p=Object.getPrototypeOf(t);if(p.__w)return;p.__w=1;"
    "for(const n of ['image','rect','text']){const f=p[n];p[n]=function(sp){const r=f.call(this,sp);__refs++;ref(r);return r}}}"
    "K.replace=function(f){return RP.call(K,t=>{wrap(t);__refs=0;__ov=[];return f(t)})};"
    "K.patch=function(f){return PA.call(K,t=>{wrap(t);return f(t)})};})();"
    "globalThis.__turn=0;globalThis.__now=0;globalThis.__sets=0;globalThis.__cues=0;"
    "globalThis.__store={'derby.v1':{v:1,pts:1500,race:__race}};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,app:globalThis.app,time:{now:()=>__now},"
    /* The device samples the native heap at the start of a turn, so while the
     * source evaluates internalFreeBytes is null (2026-09-30: a startup loop
     * waiting on it spun into the 2 s evaluation deadline). Same here. */
    "capabilities:{get:n=>({name:n,supported:true,available:true})},"
    "memory:{info:()=>({internalFreeBytes:__turn?40000:null})},"
    "random:{seed:()=>__hw},"
    "audio:{cue:()=>{__cues++;return true},tone:s=>{__tone(s.frequencyHz);return Promise.resolve()}},"
    "storage:{get:k=>Promise.resolve(k in __store?{value:JSON.parse(JSON.stringify(__store[k])),revision:1}:null),"
    "set:(k,v)=>{__sets++;__store[k]=JSON.parse(JSON.stringify(v));return Promise.resolve({revision:1})}}};";

int main(int argc,char **argv){
    /* DERBY_APP_DIR: the app as the firmware build embeds it, its @plan
     * functions lowered (tools/games/run_derby.py makes the copy with
     * tools/kasane_ir/lower_plans.mjs); apps/derby itself does not run. */
    const char *app_dir=getenv("DERBY_APP_DIR")?getenv("DERBY_APP_DIR"):"apps/derby";
    static char entry[512],list[512];
    snprintf(entry,sizeof entry,"%s/derby_watch.js",app_dir);
    snprintf(list,sizeof list,"%s/chunks.txt",app_dir);
    const char *path=argc>1?argv[1]:entry;
    FILE *f=fopen(path,"rb");if(!f){printf("cannot open %s\n",path);return 2;}
    static char src[1<<16];size_t n=fread(src,1,sizeof src-1,f);fclose(f);src[n]=0;
    ppm_dir=getenv("DERBY_PPM");
    if(getenv("DERBY_TIER"))tier_env=atoi(getenv("DERBY_TIER"));
    if(getenv("DERBY_CSV"))csv=fopen(getenv("DERBY_CSV"),"w");
    if(csv)fprintf(csv,"tick,scene,surface,screen,draws,segments,raster,draw_raster_max,draw_steps_max,frame_steps,live_plans,points,instr_max,registered\n");
    /* The game's chunks (pocket.app.load), read from the list the firmware
     * build reads; the entry is still the file named above. */
    if(app_chunks_host_read(list))return 2;
    atexit(app_chunks_host_clear);
    app_registry_select("local.derby");
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);host_capabilities_clear();
    const char *lim=getenv("DERBY_HEAP_LIMIT");
    if(lim){size_t l=(size_t)strtoul(lim,NULL,0);JS_SetMemoryLimit(rt,l);if(l/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,l/2);}
    pocket_kasane_install(ctx,NULL);
    /* The built-in plans (the firmware's app_session.c does the same). */
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    pocket_input_install(ctx,NULL);
    if(pocket_app_load_install(ctx)!=ESP_OK)return 2;
    JSValue g=JS_GetGlobalObject(ctx);
    static const struct {const char *n;JSCFunction *f;int a;} fns[]={
        {"__log",js_log,1},{"__hr",js_hr,0},{"__reg",js_cap_reg,4},{"__unreg",js_cap_unreg,1},{"__begin",js_cap_begin,2},
        {"__draw",js_cap_draw,4},{"__commit",js_cap_commit,1},{"__tone",js_tone,1},{"__probe",js_probe,4},{"__churn",js_churn,1}};
    for(unsigned i=0;i<sizeof fns/sizeof fns[0];i++)
        JS_SetPropertyStr(ctx,g,fns[i].n,JS_NewCFunction(ctx,fns[i].f,fns[i].n,fns[i].a));
    JS_FreeValue(ctx,g);
    /* DERBY_NORMAL=<race>,<pick>: the stored race is the demo's, played by hand. */
    unsigned normal_race=0,normal_pick=0;
    if(getenv("DERBY_NORMAL")&&sscanf(getenv("DERBY_NORMAL"),"%u,%u",&normal_race,&normal_pick)!=2)return 2;
    /* DERBY_HW: what pocket.random.seed() returns. The device reads the
     * hardware RNG; here it is fixed so every run of the script is the same
     * game (and a demo race can be played again by hand). */
    const unsigned hw=getenv("DERBY_HW")?(unsigned)strtoul(getenv("DERBY_HW"),NULL,0):0x2545f491u;
    char race_js[96];snprintf(race_js,sizeof race_js,"globalThis.__race=%u;globalThis.__hw=%u",normal_race?normal_race:3u,hw);
    eval(race_js,strlen(race_js),"race.js");
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
    eval_deadline=now_s()+2;JS_SetInterruptHandler(rt,eval_interrupt,NULL);
    bool ok=eval(src,n,path);
    eval_deadline=0;
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
    /* DERBY_PAN=a[,b[,c]]: the app's PAN (the distance past which WIDE is a
     * panning unit, the leader's px, the f cap): 0 pans for every WIDE (f
     * from its least, 200), 1e9 never; 0,1 keeps f at 200, which brings the
     * screen into a panning unit's view. */
    if(getenv("DERBY_PAN")){char b[96];snprintf(b,sizeof b,"[%s].forEach((v,i)=>PAN[i]=v)",getenv("DERBY_PAN"));
        REQ(eval(b,strlen(b),"pan.js"));}
    /* DERBY_JS: any script run after the app's (e.g. moving the screen so
     * that a panning unit sees it: run_derby.py). */
    if(getenv("DERBY_JS"))REQ(eval(getenv("DERBY_JS"),strlen(getenv("DERBY_JS")),"env.js"));
    REQ(loaded_seen==1);                    /* the stored points and race number */
    /* Paddock: let the plans load and the odds settle, cycle the tier. */
    for(unsigned i=0;i<20;i++)frame(0);
    if(normal_race){
        /* By hand, with the demo's pick and its stake (100, the default). */
        for(unsigned i=1;i<normal_pick;i++)tap("d");
        tap("1");
        run_until("res",4000);
        printf("NORMAL_FINISH %s\nNORMAL_PICK %s\nNORMAL_RESULT %s\n",finish[0],last_pick,last_result);
        bool ok=!exceptions&&!bad_present&&!framefails&&finishes==1;
        pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        printf("%s\n",ok?"DERBY_HOST PASS":"DERBY_HOST FAIL");
        return ok?0:1;
    }
    bool full=tier_env==1;
    if(full)demo_phase();
    unsigned taps=tier_env==1?3:(unsigned)(tier_env+2)%3;   /* tab cycles LIGHT MID HEAVY */
    for(unsigned i=0;i<taps;i++)tap("tab");
    for(unsigned i=0;i<30;i++)frame(0);
    ppm("pad");
    /* DERBY_PICK: how many horses on from #1 (default 2, #3): the manual
     * CLOSE camera follows the pick, so #8 tries the widest close-up. */
    for(int i=getenv("DERBY_PICK")?atoi(getenv("DERBY_PICK")):2;i>0;i--)tap("d");
    tap("e");tap("s");tap("e");
    for(unsigned i=0;i<4;i++)frame(0);
    tap("1");
    run_until("gate",10);
    for(unsigned i=0;i<6;i++)frame(0);
    ppm("gate");
    run_until("race",400);
    frame(0);frame(0);ppm("start");
    /* Race: WIDE, then CLOSE on the pick, FIELD, WIDE again. */
    unsigned lead_shot=0;
    const unsigned head0=head_frames;         /* the demo races have their own */
    const bool keys=!getenv("DERBY_NOCAM");  /* DERBY_NOCAM: the director alone */
    while(!strcmp(scene,"race")){
        if(keys&&scene_t==200)edge("/",true);
        if(keys&&scene_t==201)edge("/",false);
        if(keys&&scene_t==450)edge("/",true);
        if(keys&&scene_t==451)edge("/",false);
        if(keys&&scene_t==650)edge("/",true);
        if(keys&&scene_t==651)edge("/",false);
        unsigned leads=lead_logs;
        frame(0);
        /* The screen: switching on, the director's VISION shot, its pan,
         * and WIDE after it (widths: VISION 153 px, WIDE 118, FIELD 68). */
        const int vw=probe_w;
        if(vw&&!screen_seen){screen_seen=tick;ppm("vision_on");}
        if(vw>=140&&screen_seen&&tick>=screen_seen+40&&!vision_shot){vision_shot=tick;ppm("vision");}
        if(vision_shot&&tick==vision_shot+30)ppm("pan");
        if(vision_shot&&vw>100&&vw<130&&!wide_shot){wide_shot=tick;ppm("wide_screen");}
        if(head_frames>head0&&!head_at)head_at=tick;
        if(head_at&&tick==head_at+24)ppm("head");
        if(scene_t==120)ppm("wide");
        if(scene_t==300)ppm("close");
        if(scene_t==520)ppm("field");
        if(leads!=lead_logs&&!lead_shot&&scene_t>660){ppm("lead");lead_shot=1;}
        /* The panning units, first seen near (f < 500), far (< 1000) and
         * zoomed (1000 and up), 10 frames into the shot. */
        static unsigned pan_at[3],pan_seen[3];
        if(cur_pan_f>0){
            const unsigned b=cur_pan_f<500?0:cur_pan_f<1000?1:2;
            if(!pan_seen[b]&&(!pan_at[b]||tick>pan_at[b]))pan_at[b]=tick+10;
            if(pan_at[b]==tick&&!pan_seen[b]){
                static const char *const PT[]={"pan_near","pan_far","pan_tele"};
                pan_seen[b]=1;ppm(PT[b]);printf("PANSHOT %s %u f %.0f\n",PT[b],tick,cur_pan_f);
            }
        }
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
        /* The replay's own cuts near the screen: CLOSE takes it out of
         * the frame, FIELD shows it small; then the director again. */
        unsigned on=0;
        while(!strcmp(scene,"race")){
            frame(0);
            if(probe_w&&!on)on=tick;
            if(on&&tick==on+40)edge("/",true);
            if(on&&tick==on+41)edge("/",false);
            if(on&&tick==on+48)ppm("close_off");
            if(on&&tick==on+70)edge("/",true);
            if(on&&tick==on+71)edge("/",false);
            if(on&&tick==on+80)ppm("field_screen");
            if(scene_t>3000)FAIL("replay did not end");
        }
        run_until("photo",3000);
        run_until("res",200);
        REQ(finishes==2);
        if(strcmp(finish[0],finish[1]))FAIL("replay differs:\n  %s\n  %s",finish[0],finish[1]);
        for(unsigned i=0;i<10;i++)frame(0);
        tap("1");
        run_until("pad",10);
        for(unsigned i=0;i<40;i++)frame(0);
        /* Back in the middle of a demo race: the save turn writes the
         * player's points and race, never the demo's. */
        now_ms+=15000;frame(0);REQ(in_demo);
        run_until("race",400);
        for(unsigned i=0;i<200;i++)frame(0);
        const unsigned e0=demo_ends;
        frame(0x2000);                      /* Back: the save turn */
        REQ(saves==1&&demo_ends==e0+1&&!in_demo);
        if(strcmp(save_line,"SAVE points=1350 race=4"))FAIL("Back saved %s",save_line);
    }
    if(getenv("DERBY_MEMDUMP")){JS_RunGC(rt);JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);JS_DumpMemoryUsage(stdout,&m,rt);}
    JSValue gs=JS_GetGlobalObject(ctx),store=JS_GetPropertyStr(ctx,gs,"__store"),rec=JS_GetPropertyStr(ctx,store,"derby.v1");
    JSValue js=JS_JSONStringify(ctx,rec,JS_UNDEFINED,JS_UNDEFINED);const char *jss=JS_ToCString(ctx,js);
    printf("stored %s\n",jss?jss:"?");
    if(full&&(!jss||strcmp(jss,"{\"v\":1,\"pts\":1350,\"race\":4}")))FAIL("store after the demos: %s",jss?jss:"?");
    if(jss)JS_FreeCString(ctx,jss);
    printf("demo: %u started, %u ended; DEMO node shown %u frames, hidden %u inside demos, %u frames outside with it; "
           "view refs max %u/32 (paddock %u), commands max %u/80\n",demo_starts,demo_ends,demo_on,demo_off,
           demo_px_outside,refs_max,refs_pad_max,cmds_max);
    REQ(!demo_px_outside&&refs_max<=32&&cmds_max<=80);
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
    printf("\nscreen: frames with the face %u (feed drawn in %u, %u segments wholly inside), fill/bezel errors %u, "
           "frames with anything else on the face %u (%u px), lettering shown in %u frames (%u node checks, %u off the "
           "face), refs max %u/32, commands max %u/80, HEAD ON frames %u\n",vis_frames,feed_frames,feed_segments,bezel_bad,
           occluded_frames,occluded_pixels,overlay_frames,rect_checked,rect_mismatch,refs_max,cmds_max,head_frames);
    printf("\npanning units: frames %u, f %.0f..%.0f, the screen in view %u; Newton: %lu points in %u draws, "
           "max error %.4f px (f %.0f) against the exact projection, %.4f px for points on the panel\n",pan_frames,
           pan_frames?pan_f_lo:0,pan_f_hi,pan_face_frames,newton_points,newton_draws,newton_max,newton_max_f,newton_screen);
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
    for(unsigned i=0;i<5;i++)if(churn[i].frames)
        printf("guest heap churn in frame(), %s: %u frames, %.1f allocations and %.0f B a frame, largest rise in one "
               "frame %zu B, freed only by the cycle collector %zu B\n",CHURN_SCENE[i],churn[i].frames,
               (double)churn[i].n/churn[i].frames,(double)churn[i].b/churn[i].frames,churn[i].hi,churn[i].gc);
    bool pass=!exceptions&&!bad_present&&!framefails&&ops_used==0x7fffu&&frame_reg_max<=1&&go_seen>=1&&
              !rect_mismatch&&!bezel_bad&&!occluded_frames&&vis_frames&&feed_frames&&
              (getenv("DERBY_PANFACE")?pan_face_frames>0:overlay_frames>0)&&refs_max<=32&&cmds_max<=80&&newton_screen<0.1;
    /* No cycles: the device would keep them until the heap is nearly full
     * (a recursive closure in the plan decoder left ~4 KB a race, LOADSTALL). */
    for(unsigned i=0;i<5;i++)pass=pass&&!churn[i].gc;
    if(full)pass=pass&&finishes==2&&saves==1&&tones>0&&demo_starts==10&&demo_ends==10;
    else pass=pass&&!demo_starts;
    if(csv)fclose(csv);
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",pass?"DERBY_HOST PASS":"DERBY_HOST FAIL");
    return pass?0:1;
}
