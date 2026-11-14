/* apps/derby/derby_watch.js as the firmware runs it, on the host: the real
 * QuickJS, the real pocket.kasane (view + shiki) and the real renderer
 * presenting into a 240x135 panel after every frame(), with key edges fed
 * through the real keymap_poll() and keystate into pocket.input.keys.
 *
 * A scripted game: paddock (pick #3, bet, cycle the load tier), gate, race
 * with camera changes, slow motion, photo, result, a replay of the same race
 * with no camera changes (must finish identically: the camera cannot change
 * the race), the next race, and the Back turn (frame(0x2000) saves).
 *
 * Every Shiki draw (shiki.draw, drawMany, and pocket.derby's native draws
 * through pocket_proc_draw_hook) is replayed here through the same engine
 * into a frame of our own, and at each commit that frame (rendered whole)
 * must equal, pixel for pixel, the frame the adapter committed (read back in
 * 8-row bands through pocket_proc_backdrop): no draw reaches the panel
 * unseen, so the screen checks below see everything that was drawn. Every
 * op of the built-in Shiki table must be drawn at least once, at most one
 * program may be registered per frame, and per-frame statistics are written.
 * (The float procedural VM and its three-way and typed-point oracles went
 * with Shiki v2.0; the engine's own correctness is the Shiki contract's.)
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
 * "首振りカメラ"): a panning unit's view of the screen gets the face checks
 * too (tag 3, the rect inside the face). Their frames are counted as scene
 * "pan". (Their projection error is tools/kasane_ir/q16_error.mjs's.) Env DERBY_PAN=a[,b,c] sets the
 * app's PAN, DERBY_JS runs a script after the app's, DERBY_PANFACE=1 checks
 * only the panning units' faces (the screen moved), DERBY_SHOTS=tick:tag,...
 * writes panels at those ticks.
 * DERBY_EACH=<js> runs before every frame (e.g. a camera held: cam=2;man=9),
 * DERBY_LEADSHOTS=<m>:<tag>,... writes a panel when the player's race's
 * leader first passes m (the oval's preview, run_derby.py --ppm).
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
#include "pocket_shiki_frame.h"
#include "pocket_derby.h"
#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "keymap.h"
#include "app_chunks_host.h"
#include "app_registry.h"
#include "ksn_shiki.h"
#include "shiki_heap.h"
#include "pocket_shiki_api.h"   /* the windows go before the guest */
#include "pocket_app_load.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_render.h"
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
 * the heap at the start of the call (docs/apps/derby-camera-history.md). */
static size_t n_alloc,b_alloc,turn_hi;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;
    if(cur_bytes>turn_hi)turn_hi=cur_bytes;}
/* DERBY_ALLOC_TRACE=<file>: the allocations of frame() alone (inside
 * __churn) in tools/test_stress_app.c's STRESS_ALLOC_TRACE format -- "m
 * <usable> <id>", "f <id>", "r <old> <new> <usable>", "F <frame>" -- for
 * the block cache's size classes (tools/vmtest/prof/cache_classes.py). The
 * harness's own per-frame eval stays out: the device calls frame() directly. */
static FILE *alloc_trace;
static bool trace_on;
static uint32_t trace_id=1,trace_frame;
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;n_alloc++;b_alloc+=tlsf_len(n);
    h->pad[0]=trace_id++;
    if(trace_on)fprintf(alloc_trace,"m %zu %zu\n",tlsf_len(n),h->pad[0]);
    pk_count(tlsf_len(n),0);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;
    if(trace_on)fprintf(alloc_trace,"f %zu\n",h->pad[0]);
    pk_count(0,tlsf_len(h->n));free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n,old_id=h->pad[0];
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    n_alloc++;b_alloc+=tlsf_len(n);
    q->pad[0]=trace_id++;
    if(trace_on)fprintf(alloc_trace,"r %zu %zu %zu\n",old_id,q->pad[0],tlsf_len(n));
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

/* ---- Shiki draws (derby_prog.js @shiki, the built-in table): each run again
 * here through the same engine, its segments appended to the candidate frame
 * in draw order, so that at commit the frame can be compared with the one
 * the adapter built. The engine's own correctness is the Shiki contract's
 * (tools/kasane_contract, shiki_diff.mjs), not this harness's. */
typedef struct { int handle; ksn_shiki_plan *plan; } sspec;
#define SSPECS 32u
static sspec sspecs[SSPECS];
static unsigned shiki_draws;
static sspec *sfind(int h){for(unsigned i=0;i<SSPECS;i++)if(sspecs[i].plan&&sspecs[i].handle==h)return &sspecs[i];return NULL;}

static unsigned live_plans,live_max,registered,unregistered,frame_regs,frame_reg_max;
static ksn_proc_frame cand,front;
static uint16_t pix_cand[240*135],pix_dev[240*135],mask[240*135];
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
static unsigned frame_draws,frame_steps,draw_raster_max,draw_steps_max,frame_instr_max,frames_checked,frames_unchecked;
static uint64_t pixel_hash=1469598103934665603ull;
static int frame_cm;   /* the camera (cm) of the frame being drawn, for the CSV */
static FILE *csv;

typedef struct {
    char name[16];
    unsigned frames,draws_max,seg_max,raster_max,draw_raster_max,draw_steps_max,frame_steps_max,live_max,instr_max;
    unsigned long long seg_sum,raster_sum;
} scene_stats;
static scene_stats stats[8];
static unsigned nstats;
static scene_stats *stat_for(const char *n){
    for(unsigned i=0;i<nstats;i++)if(!strcmp(stats[i].name,n))return &stats[i];
    REQ(nstats<8);
    snprintf(stats[nstats].name,sizeof stats[0].name,"%s",n);
    return &stats[nstats++];
}

static double num(JSValueConst v){double d=NAN;JS_ToFloat64(ctx,&d,v);return d;}
static double at(JSValueConst a,unsigned i){JSValue v=JS_GetPropertyUint32(ctx,a,i);double d=num(v);JS_FreeValue(ctx,v);return d;}
static unsigned len(JSValueConst a){JSValue v=JS_GetPropertyStr(ctx,a,"length");unsigned n=(unsigned)num(v);JS_FreeValue(ctx,v);return n;}

/* __begin(colour, surface): after shiki.beginFrame succeeded. */
static JSValue js_cap_begin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    memset(&cand,0,sizeof cand);
    cur_bg=(uint16_t)num(argv[0]);
    cur_surface=argc>1&&num(argv[1])?1u:0u;
    vis_drawn=0;
    frame_draws=frame_steps=frame_instr_max=0;
    return JS_UNDEFINED;
}
static void append(ksn_proc_frame *dst,const ksn_proc_frame *f){
    REQ((unsigned)dst->count+f->count<=KSN_PROC_SEGMENTS);
    memcpy(&dst->segments[dst->count],f->segments,f->count*sizeof f->segments[0]);
    dst->count=(uint16_t)(dst->count+f->count);
    dst->ext|=f->ext;
    REQ((unsigned)dst->raster_steps+f->raster_steps<=UINT16_MAX);
    dst->raster_steps=(uint16_t)(dst->raster_steps+f->raster_steps);
}
static double pan_f_lo=1e9,pan_f_hi,cur_pan_f;
static unsigned pan_frames,pan_face_frames;
/* The checks a tagged draw feeds: 1 the screen's face, 2 HEAD ON's still,
 * 3 a panning unit's bezel. in64: the draw's inputs; fp: its segments. */
static void tagged(unsigned tag,const double *in64,const ksn_proc_frame *fp){
    if(tag==2&&!cur_surface)head_frames++;
    if(tag==3){
        /* A panning unit's view of the screen: its bezel (hl, the face's two
         * edges joined) closes the face; the rect inside it is the app's vr
         * rule, recomputed here from the edges [x, 1/Z'] of the draw, held
         * as the vis inputs are (vr less one on the left and top). */
        REQ(!cur_surface&&!vis_drawn);
        vis_drawn=1;vis_seg_end=cand.count;pan_face_frames++;
        const double xl=in64[0],rl=in64[1],xr=in64[2],rr=in64[3];
        vis_in[0]=(int)floor(fmin(xl,xr)+.5)-1;vis_in[1]=(int)ceil(28-10*fmin(rl,rr))-1;
        vis_in[2]=(int)floor(fmax(xl,xr)+.5);vis_in[3]=28;
    }
    if(tag==1){
        REQ(!cur_surface&&!vis_drawn&&fp->count>=4);
        vis_drawn=1;vis_seg_end=cand.count;
        for(unsigned i=0;i<4;i++)vis_in[i]=(int)in64[i];
        /* The face is filled row by row, exactly its interior, then the first
         * bezel ring runs one pixel outside it. */
        const ksn_proc_segment *g=fp->segments;
        const int a=vis_in[0],b=vis_in[1],c2=vis_in[2],d=vis_in[3],rows=d-b-1;
        REQ(rows>0&&(unsigned)rows+4<=fp->count);
        for(int r=0;r<rows;r++)
            if(!(g[r].x0==a+1&&g[r].x1==c2-1&&g[r].y0==b+1+r&&g[r].y1==b+1+r))bezel_bad++;
        g+=rows;
        if(!(g[0].x0==a&&g[0].y0==b&&g[0].x1==c2&&g[0].y1==b&&g[1].x1==c2&&g[1].y1==d&&
             g[2].x1==a&&g[2].y1==d&&g[3].x1==a&&g[3].y1==b))bezel_bad++;
    }
}
/* pocket.derby's draws (pocket_shiki_frame.c calls this after each one that
 * succeeded): replayed like the app's shiki.draw, tagged as the JS wrapper
 * tags them (__nt: -2 the screen's face, -3 a panning unit's hl, else -1).
 * (ser() draws with integer inputs only: the hook below.) */
static bool ser_oracle;
static double ser_buf[1<<16];
static unsigned ser_n;
static bool ser_overflow;
static unsigned long ser_calls,ser_fuzz_calls,ser_draws,ser_fuzz_draws;
static void sdraw_run(sspec *s,const double *in,const int32_t *fx,unsigned n,unsigned tag);
void pocket_proc_draw_hook(uint32_t handle,const double *in,unsigned n){
    if(pocket_proc_hook_dry)return;
    JSValue g=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,g,"__nt"),h=JS_NewInt32(ctx,(int32_t)handle);
    JSValue r=JS_Call(ctx,fn,g,1,&h);
    REQ(!JS_IsException(r));
    const double f=num(r);
    JS_FreeValue(ctx,r);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,g);
    const unsigned tag=f==-2?1u:f==-3&&n>7&&in[7]==10565?3u:0u;
    /* pocket_proc_draw_numbers takes Shiki handles only (v2.0). */
    sspec *ss=sfind((int)handle);REQ(ss);
    sdraw_run(ss,in,NULL,n,tag);
}
/* pocket.derby.runners' draws (pocket_proc_draw_fixed): integer inputs, as
 * the app's drawMany with an Int32Array, replayed untagged as those were. */
void pocket_proc_draw_fixed_hook(uint32_t handle,const int32_t *fx,unsigned n){
    REQ(n<=KSN_SHIKI_INPUTS);
    /* The integer ser()'s draws (pocket.derby.ser): the raw Q values,
     * turned into numbers by __sercmp, which knows the plan. */
    if(ser_oracle){
        if(ser_n+9<=sizeof ser_buf/sizeof ser_buf[0]){
            ser_buf[ser_n++]=handle;
            for(unsigned i=0;i<8;i++)ser_buf[ser_n++]=i<n?fx[i]:0;
        }else ser_overflow=true;
    }
    if(pocket_proc_hook_dry)return;
    double in[KSN_SHIKI_INPUTS];
    for(unsigned i=0;i<n;i++)in[i]=fx[i];
    sspec *ss=sfind((int)handle);REQ(ss);
    sdraw_run(ss,in,fx,n,0);
}
static JSValue js_serbegin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;
    ser_n=0;ser_overflow=false;
    return JS_UNDEFINED;
}
static JSValue js_serdry(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;
    pocket_proc_hook_dry=JS_ToBool(ctx,argv[0]);
    return JS_UNDEFINED;
}
/* The integer path's tolerance (docs/apps/derby-ser-division.md sec.5.1 and
 * the user's decision): both lists turned into the points the plans put on
 * the screen (posts' feet and tops, the turf stripes' ends, the stands' tier
 * ends; as tools/games/test_derby_serq.c), matched both ways, on-screen
 * points only. Recorded: the distances; gated: a call the JS draws on the
 * screen and the C not at all, and calls with a point over 2 px apart (a
 * tie flipped: one post or stripe) past 1 in 10,000 (checked at the end). */
enum{SK_PRAIL,SK_PT,SK_HL,SK_CROWD};
typedef struct{double x,y;}spt;
static unsigned ser_points(const double *d,unsigned n,bool turf,uint32_t hl,uint32_t cr,bool fixed,spt *P,unsigned max){
    unsigned m=0;
    for(unsigned i=0;i+9<=n;i+=9){
        const uint32_t h=(uint32_t)d[i];
        const int kind=h==hl?SK_HL:h==cr?SK_CROWD:turf?SK_PT:SK_PRAIL;
        const uint8_t *q=kind==SK_HL?derby_q_hl:kind==SK_CROWD?derby_q_crowd:kind==SK_PT?derby_q_pt:derby_q_prail;
        double in[8];
        for(unsigned k=0;k<8;k++)in[k]=fixed?d[i+1+k]/(double)(1u<<q[k]):d[i+1+k];
        if(kind==SK_PRAIL||kind==SK_PT){
            const int cnt=(int)in[4];
            for(int j=0;j<=cnt&&m+2<max;j++){
                if(kind==SK_PT&&j==cnt)break;
                const double px=in[0]+j*in[1],pz=in[2]+j*in[3],qq=-1/pz;
                P[m++]=(spt){px*qq,28+6*qq};
                if(kind==SK_PRAIL)P[m++]=(spt){px*qq,28+qq*in[7]};
                else{const double qf=-1/(pz+in[7]);P[m++]=(spt){(px+in[6])*qf,28+6*qf};}
            }
        }else if(kind==SK_HL&&m+4<max){
            P[m++]=(spt){in[0],28+6*in[1]};P[m++]=(spt){in[2],28+6*in[3]};
            P[m++]=(spt){in[0],28+(6-2.4*in[6])*in[1]};P[m++]=(spt){in[2],28+(6-2.4*in[6])*in[3]};
        }
    }
    return m;
}
static unsigned long tol_calls,tol_pts,tol_hist[6],tol_swaps,tol_missing,tol_draw_diff;
static double tol_worst;
static bool spt_on(spt p){return p.x>=0&&p.x<=240&&p.y>=-20&&p.y<=155;}
static double spt_match(const spt *A,unsigned na,const spt *B,unsigned nb,unsigned *on){
    double w=0;
    for(unsigned i=0;i<na;i++){
        if(!spt_on(A[i]))continue;
        ++*on;
        double best=1e9;
        for(unsigned j=0;j<nb;j++){const double d=fmax(fabs(A[i].x-B[j].x),fabs(A[i].y-B[j].y));if(d<best)best=d;}
        tol_pts++;tol_hist[best<=.01?0:best<=.1?1:best<=.5?2:best<=1?3:best<=2?4:5]++;
        if(best>w)w=best;
    }
    return w;
}
static spt tol_a[8192],tol_b[8192];
static double tol_rec[1<<16];
/* __sercmp(rec, fuzz, turf, hl, crowd): the JS ser()'s draws [handle, 8
 * inputs]... against the C's by the tolerance above (the C's double series,
 * once compared bit for bit, is gone: docs/apps/derby-ser-division.md sec.13). */
static JSValue js_sercmp(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;
    const unsigned n=len(argv[0]);
    const bool fz=JS_ToBool(ctx,argv[1]);
    if(fz){ser_fuzz_calls++;ser_fuzz_draws+=n/9;}else{ser_calls++;ser_draws+=n/9;}
    REQ(!ser_overflow);
    REQ(argc>=5&&n<=sizeof tol_rec/sizeof tol_rec[0]);
    for(unsigned i=0;i<n;i++)tol_rec[i]=at(argv[0],i);
    const bool turf=JS_ToBool(ctx,argv[2]);
    const uint32_t hl=(uint32_t)num(argv[3]),cr=(uint32_t)num(argv[4]);
    const unsigned na=ser_points(tol_rec,n,turf,hl,cr,false,tol_a,8192),nb=ser_points(ser_buf,ser_n,turf,hl,cr,true,tol_b,8192);
    unsigned oa=0,ob=0;
    const double w=fmax(spt_match(tol_a,na,tol_b,nb,&oa),spt_match(tol_b,nb,tol_a,na,&ob));
    tol_calls++;
    if(n!=ser_n)tol_draw_diff++;
    if(oa&&!ob)tol_missing++;
    if(w>2){
        if(tol_swaps++<8)printf("SER_TOL %.2f px%s: JS %u draws, C %u draws\n",w,fz?" (fuzz)":"",n/9,ser_n/9);
    }
    if(w>tol_worst)tol_worst=w;
    ser_n=0;
    return JS_UNDEFINED;
}
static JSValue js_cap_sreg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;
    int32_t h=0;REQ(JS_ToInt32(c,&h,argv[0])==0);
    /* A built-in name, or the program's bytes (a Uint8Array) with its data
     * windows (argv[3], each carrying __d = [type, q, k_rec, records]). */
    const uint8_t *code=NULL;size_t clen=0;
    ksn_shiki_region regs[KSN_SHIKI_REGIONS];unsigned nr=0;
    if(JS_IsString(argv[1])){
        const char *name=JS_ToCString(c,argv[1]);REQ(name);
        unsigned count=0;const ksn_shiki_rom_entry *rom=ksn_shiki_rom_table(&count);
        for(unsigned k=0;k<count;k++)if(!strcmp(rom[k].name,name)){code=rom[k].code;clen=rom[k].len;}
        JS_FreeCString(c,name);
    }else{
        code=JS_GetUint8Array(c,&clen,argv[1]);
        if(argc>3&&JS_IsArray(argv[3])){
            nr=len(argv[3]);REQ(nr<=KSN_SHIKI_REGIONS);
            for(unsigned k=0;k<nr;k++){
                JSValue w=JS_GetPropertyUint32(c,argv[3],k),d=JS_GetPropertyStr(c,w,"__d");
                size_t off=0,blen=0,bpe=0,size=0;
                JSValue buf=JS_GetTypedArrayBuffer(c,w,&off,&blen,&bpe);REQ(!JS_IsException(buf));
                uint8_t *mem=JS_GetArrayBuffer(c,&size,buf);REQ(mem);
                regs[k]=(ksn_shiki_region){mem+off,(uint32_t)at(d,3),(uint8_t)at(d,0),(uint8_t)at(d,1),(uint8_t)at(d,2)};
                JS_FreeValue(c,buf);JS_FreeValue(c,d);JS_FreeValue(c,w);
            }
        }
    }
    REQ(code);
    sspec *s=NULL;for(unsigned i=0;i<SSPECS&&!s;i++)if(!sspecs[i].plan)s=&sspecs[i];
    REQ(s);
    /* argv[2]: the registration arguments (an array, or undefined). */
    double args[KSN_SHIKI_ARGS];
    unsigned na=argc>2&&JS_IsArray(argv[2])?len(argv[2]):0;
    REQ(na<=KSN_SHIKI_ARGS);
    for(unsigned k=0;k<na;k++)args[k]=at(argv[2],k);
    s->plan=malloc(ksn_shiki_plan_bytes_for(code,clen));REQ(s->plan);
    REQ(ksn_shiki_register(code,clen,args,na,regs,nr,s->plan));
    s->handle=h;live_plans++;if(live_plans>live_max)live_max=live_plans;registered++;frame_regs++;
    REQ(live_plans<=32);
    return JS_UNDEFINED;
}
static JSValue js_cap_sunreg(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;
    int32_t h=0;REQ(JS_ToInt32(c,&h,argv[0])==0);
    sspec *s=sfind(h);REQ(s);
    free(s->plan);s->plan=NULL;live_plans--;unregistered++;
    return JS_UNDEFINED;
}
/* Doubles handed to the native surfaces (docs/apps/derby-integer.md sec.1,
 * condition 3): a float-tagged number among a call's arguments, one level
 * into arrays, and a Float32/Float64Array argument, counted by surface. */
enum{FT_DRAW,FT_SDRAW,FT_DERBY,FT_REF,FT_N};
static const char *const FT_NAME[FT_N]={"draw","drawMany","derby","ref"};
static unsigned long ft_calls[FT_N],ft_floats[FT_N],ft_bad_calls[FT_N];
static char ft_first[FT_N][96];
static unsigned ft_value(JSContext *c,JSValueConst v,int depth){
    if(JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)))return 1;
    if(!JS_IsObject(v))return 0;
    const JSTypedArrayEnum t=JS_GetTypedArrayType(v);
    if(t==JS_TYPED_ARRAY_FLOAT64||t==JS_TYPED_ARRAY_FLOAT32)return 1;
    if(depth||!JS_IsArray(v))return 0;
    int64_t n=0;
    JSValue len=JS_GetPropertyStr(c,v,"length");
    JS_ToInt64(c,&n,len);JS_FreeValue(c,len);
    unsigned f=0;
    for(int64_t i=0;i<n&&i<256;i++){JSValue e=JS_GetPropertyUint32(c,v,(uint32_t)i);f+=ft_value(c,e,1);JS_FreeValue(c,e);}
    return f;
}
/* __ftag(kind, name, args): args is the call's arguments object or an array. */
static JSValue js_ftag(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    int32_t k=0;
    if(argc<3||JS_ToInt32(c,&k,argv[0])||k<0||k>=FT_N)return JS_UNDEFINED;
    int64_t n=0;
    JSValue len=JS_GetPropertyStr(c,argv[2],"length");
    JS_ToInt64(c,&n,len);JS_FreeValue(c,len);
    unsigned f=0;
    for(int64_t i=0;i<n&&i<16;i++){JSValue e=JS_GetPropertyUint32(c,argv[2],(uint32_t)i);f+=ft_value(c,e,0);JS_FreeValue(c,e);}
    ft_calls[k]++;
    if(f){
        ft_floats[k]+=f;
        if(!ft_bad_calls[k]++){const char *s=JS_ToCString(c,argv[1]);snprintf(ft_first[k],sizeof ft_first[k],"%s",s?s:"?");JS_FreeCString(c,s);}
    }
    return JS_UNDEFINED;
}
static JSValue js_cap_sdraw(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;
    int32_t h=0;REQ(JS_ToInt32(c,&h,argv[0])==0);
    sspec *s=sfind(h);REQ(s);
    const unsigned n=len(argv[1]);REQ(n<=KSN_SHIKI_INPUTS);
    /* argv[2]: the inputs came as an Int32Array (each input's Q, unconverted). */
    const bool fixed=argc>2&&JS_ToBool(c,argv[2]);
    double in[KSN_SHIKI_INPUTS];
    int32_t fx[KSN_SHIKI_INPUTS];
    for(unsigned i=0;i<n;i++){in[i]=at(argv[1],i);fx[i]=(int32_t)in[i];}
    int32_t tag=0;
    if(argc>3)JS_ToInt32(c,&tag,argv[3]);
    sdraw_run(s,in,fixed?fx:NULL,n,(unsigned)tag);
    return JS_UNDEFINED;
}
static uint64_t shiki_ops_drawn;
static void sdraw_run(sspec *s,const double *in,const int32_t *fx,unsigned n,unsigned tag){
    static ksn_shiki_vm vm;
    static ksn_proc_frame f;
    REQ((fx?ksn_shiki_draw_fixed(s->plan,fx,n,&vm,f.segments):ksn_shiki_draw(s->plan,in,n,&vm,f.segments))==KSN_SHIKI_DONE);
    f.count=vm.count;f.raster_steps=vm.raster;f.ext=vm.ext;
    for(unsigned k=0;k<s->plan->count;k++)shiki_ops_drawn|=1ull<<s->plan->code[k].op;
    append(&cand,&f);
    frame_draws++;frame_steps+=vm.steps;shiki_draws++;
    if(vm.raster>draw_raster_max)draw_raster_max=vm.raster;
    if(vm.steps>draw_steps_max)draw_steps_max=vm.steps;
    if(s->plan->count>frame_instr_max)frame_instr_max=s->plan->count;
    if(tag)tagged(tag,in,&f);
}
static uint32_t sh_used_max,sh_largest_min=UINT32_MAX;
static JSValue js_cap_commit(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;
    {uint32_t b=0,u=0,l=0;shiki_heap_info(&b,&u,&l);if(u>sh_used_max)sh_used_max=u;if(l<sh_largest_min)sh_largest_min=l;}
    /* argv[0]: the panning unit's f, 0 for a side unit. */
    cur_pan_f=argc?num(argv[0]):0;
    if(cur_pan_f>0){pan_frames++;if(cur_pan_f<pan_f_lo)pan_f_lo=cur_pan_f;if(cur_pan_f>pan_f_hi)pan_f_hi=cur_pan_f;}
    /* The replayed frame, whole, against the one the adapter committed, as
     * the presenter reads it (8-row bands): the same segments in the same
     * order, or a draw went past this harness. Surface 0 only (the backdrop;
     * DERBY draws nothing else). */
    cand.ready=true;
    for(unsigned i=0;i<240*135;i++)pix_cand[i]=cur_bg;
    REQ(ksn_proc_render_band(&cand,pix_cand,0,135));
    if(!cur_surface){
        for(unsigned y=0;y<135;y+=8)REQ(pocket_proc_backdrop(NULL,(uint16_t)y,(uint16_t)(y+8<=135?8:135-y),pix_dev+y*240)==KSN_OK);
        if(memcmp(pix_cand,pix_dev,sizeof pix_cand)){
            unsigned d=0;for(unsigned i=0;i<240*135;i++)d+=pix_cand[i]!=pix_dev[i];
            FAIL("tick %u scene %s: the replayed frame (%u segments) differs from the committed one in %u px",tick,scene,cand.count,d);
        }
    }else frames_unchecked++;
    uint64_t fh=1469598103934665603ull;   /* this frame's pixels alone (the CSV's fh) */
    if(csv){JSValue v=JS_Eval(c,"cm",2,"cm.js",0);frame_cm=(int)num(v);JS_FreeValue(c,v);}
    for(size_t i=0;i<sizeof pix_cand;i++){pixel_hash^=((const unsigned char *)pix_cand)[i];pixel_hash*=1099511628211ull;
        fh^=((const unsigned char *)pix_cand)[i];fh*=1099511628211ull;}
    if(!cur_surface)frames_checked++;
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
            unsigned i=vis_seg_end,cost;
            ksn_proc_segment g;
            bool pattern;
            while(ksn_proc_frame_next(&cand,&i,&g,&pattern,&cost)){
                if(g.x0>=X0&&g.x0<=X1&&g.x1>=X0&&g.x1<=X1&&g.y0>=Y0&&g.y0<=Y1&&g.y1>=Y0&&g.y1<=Y1){inside++;continue;}
                if(getenv("DERBY_DBGFACE")&&occluded_frames<1&&!(g.x1<X0&&g.x0<X0)&&!(g.y0>Y1&&g.y1>Y1)&&!(g.y0<Y0&&g.y1<Y0))
                    printf("  front seg %d,%d-%d,%d\n",g.x0,g.y0,g.x1,g.y1);
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
    if(cand.count>st->seg_max)st->seg_max=cand.count;
    st->seg_sum+=cand.count;st->raster_sum+=cand.raster_steps;
    if(cand.raster_steps>st->raster_max)st->raster_max=cand.raster_steps;
    if(draw_raster_max>st->draw_raster_max)st->draw_raster_max=draw_raster_max;
    if(draw_steps_max>st->draw_steps_max)st->draw_steps_max=draw_steps_max;
    if(frame_steps>st->frame_steps_max)st->frame_steps_max=frame_steps;
    if(live_plans>st->live_max)st->live_max=live_plans;
    if(frame_instr_max>st->instr_max)st->instr_max=frame_instr_max;
    if(csv)fprintf(csv,"%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%016llx\n",tick,scene,cur_surface,vis_drawn,frame_draws,cand.count,
                   cand.raster_steps,draw_raster_max,draw_steps_max,frame_steps,live_plans,0u,
                   frame_instr_max,frame_regs,frame_cm,(unsigned long long)fh);
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
/* KSN_STRIP_ROWS=4: the port of design 0 (docs/platform/lcd-dma-driver.md
 * 9.5): 4-row pieces, the halves of the one strip in turn, poisoned before
 * each is handed out. The panel digest over every presented frame must not
 * move against the 8-row port. */
static uint16_t strip_rows_arm=8;
static unsigned strip_half;
static uint32_t strip_panel_digest=2166136261u;
static uint16_t *get_strip(void *o){
    (void)o;
    if(strip_rows_arm!=4)return strip_pixels;
    uint16_t *half=strip_pixels+(strip_half++&1u)*4u*240u;
    for(unsigned i=0;i<4u*240u;i++)half[i]=0xdead;
    return half;
}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)o;memcpy(panel+y*240,p,rows*240*sizeof *p);return KSN_OK;
}
static void present(uint64_t now_us){
    pocket_kasane_set_animation_time(now_us);
    (void)pocket_kasane_advance(now_us);
    ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,.strip_rows=strip_rows_arm,.text=&ksn_font_port};
    ksn_render_stats st;
    if(pocket_kasane_present(&port,&st)!=KSN_OK)bad_present++;
    else{
        pocket_kasane_animations_presented(now_us);
        for(size_t i=0;i<240u*135u;i++){strip_panel_digest^=panel[i];strip_panel_digest*=16777619u;}
    }
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
    if(alloc_trace){trace_on=num(argv[0])!=0;if(trace_on)fprintf(alloc_trace,"F %u\n",++trace_frame);}
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
    char call[512];snprintf(call,sizeof call,"__turn=1;__now=%.3f;%s__churn(1);frame(%u);__churn(0);__probe([].concat.apply([],__ov.map(r=>"
                            "[r.__v?1:0].concat(r.__r||[0,0,0,0]))),__refs,kasane.stats().displayed.commands)%s",now_ms,
                            getenv("DERBY_EACH")?getenv("DERBY_EACH"):"",buttons,
                            getenv("DERBY_LEADSHOTS")?";__L=scene==='race'?Math.max.apply(null,rs.x):-1":"");
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
#ifdef POCKET_DERBY_SER_PROBE
    /* The device's DERBY_SER line, every 30 turns (KSN_HOST_CFLAGS=
     * -DPOCKET_DERBY_SER_PROBE; the host's cycles read 0). */
    if(!(tick%30u)){printf("[tick %u %s] ",tick,scene);pocket_derby_ser_probe_log(30);putchar('\n');}
#endif
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
    "(function(){const S=kasane.shiki,SR=S.register,SU=S.unregister,SM=S.drawMany,SD=S.draw,B=S.beginFrame,C=S.commit;"
    /* The app's pc (the panning unit, f at 4) is a global of its scripts. */
    "const pf=()=>{try{return pc?pc[4]:0}catch(e){return 0}};"
    "S.beginFrame=function(c,s){const r=s===undefined?B.call(S,c):B.call(S,c,s);__begin(c,s?1:0);return r};"
    "S.commit=function(){C.call(S);__commit(pf())};"
    /* Register by name or by bytes, unregister: the replay's own plans. */
    "S.register=function(n,a,o){const h=SR.call(S,n,a,o);__sreg(h,n,a,o&&o.data);return h};"
    "S.unregister=function(h){SU.call(S,h);__sunreg(h)};"
    /* data(): the window remembers its shape, for __sreg's regions. */
    "const SA=S.data;S.data=function(d){const w=SA.call(S,d);w.__d=[d.type==='i16'?0:d.type==='u16'?1:2,d.q,d.record,d.records];return w};"
    /* pocket.derby's native draws (pocket_proc_draw_hook): -2 the screen's
     * face (vis), -3 a panning unit's hl (its bezel when the colour is
     * 10565), else -1. */
    "globalThis.__nt=h=>{const L=globalThis.derby&&derby.L;return !L?-1:h===L.vis?-2:h===L.hl?-3:-1};"
    /* draw(handle, ...numbers) or draw(handle, array): one replayed draw.
     * Tags: 1 the screen's face (vis), 2 HEAD ON's still, 3 a panning
     * unit's bezel on the screen (hl in 10565). */
    "S.draw=function(h,i){__ftag(0,'sdraw',arguments);SD.apply(S,arguments);let a=Array.isArray(i)?i:[].slice.call(arguments,1);while(a.length&&a[a.length-1]===undefined)a=a.slice(0,-1);"
    "const L=globalThis.derby&&derby.L,t=L?h===L.vis?1:h===L.hd?2:h===L.hl&&a[7]===10565?3:0:0;__sdraw(h,a,false,t)};"
    /* drawMany: each entry one replayed draw, untagged. */
    "S.drawMany=function(H,I,s){__ftag(1,'drawMany',arguments);SM.call(S,H,I,s);for(let i=0;i<H.length;i++)__sdraw(H[i],Array.from(I.subarray(i*s,i*s+s)),I instanceof Int32Array)};})();"
    /* The draw references one replace() exposes (the limit is 32), and the
     * screen's lettering: the refs the app clips to the whole panel (setRect
     * keeps a clip), with their last rect and visibility. */
    "globalThis.__refs=0;globalThis.__ov=[];"
    "(function(){const K=kasane,RP=K.replace,PA=K.patch;"
    "function ref(r){const p=Object.getPrototypeOf(r);if(p.__w)return;p.__w=1;const sr=p.setRect,sv=p.setVisible,sc=p.setClip;"
    "p.setClip=function(t,b){if(!b[0]&&!b[1]&&b[2]===240&&b[3]===135&&!this.__o){this.__o=1;this.__v=true;this.__r=null;__ov.push(this)}return sc.call(this,t,b)};"
    "p.setRect=function(t,b){__ftag(3,'setRect',[b]);if(this.__o)this.__r=b.slice();return sr.call(this,t,b)};"
    "p.setVisible=function(t,v){if(this.__o)this.__v=v;return sv.call(this,t,v)}}"
    "function wrap(t){const p=Object.getPrototypeOf(t);if(p.__w)return;p.__w=1;"
    "for(const n of ['image','rect','text']){const f=p[n];p[n]=function(sp){const r=f.call(this,sp);__refs++;ref(r);return r}}}"
    "K.replace=function(f){return RP.call(K,t=>{wrap(t);__refs=0;__ov=[];return f(t)})};"
    "K.patch=function(f){return PA.call(K,t=>{wrap(t);return f(t)})};})();"
    "globalThis.__turn=0;globalThis.__now=0;globalThis.__sets=0;globalThis.__cues=0;"
    "globalThis.__store={'derby.v1':{v:1,pts:1500,race:__race}};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,app:globalThis.app,derby:(()=>{const d=globalThis.__pd,o={};for(const n of Object.getOwnPropertyNames(d))o[n]=typeof d[n]==='function'?function(){__ftag(2,n,arguments);return d[n].apply(d,arguments)}:d[n];return o})(),time:{now:()=>__now},"
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
    if(getenv("KSN_STRIP_ROWS"))strip_rows_arm=(uint16_t)atoi(getenv("KSN_STRIP_ROWS"));
    /* KSN_TEXT_EXTENT=0: text walks its whole declared box; the digest must not move. */
    if(getenv("KSN_TEXT_EXTENT"))g_ksn_text_extent=atoi(getenv("KSN_TEXT_EXTENT"));
    if(getenv("KSN_TEXT_RUNS"))g_ksn_text_runs=atoi(getenv("KSN_TEXT_RUNS"));
    if(getenv("KSN_PLAIN_WALK"))g_ksn_proc_plain_walk=atoi(getenv("KSN_PLAIN_WALK"));
    {extern int g_pocket_proc_ranges;if(getenv("KSN_PROC_RANGES"))g_pocket_proc_ranges=atoi(getenv("KSN_PROC_RANGES"));}
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
    if(csv)fprintf(csv,"tick,scene,surface,screen,draws,segments,raster,draw_raster_max,draw_steps_max,frame_steps,live_plans,points,instr_max,registered,cm,fh\n");
    /* The game's chunks (pocket.app.load), read from the list the firmware
     * build reads; the entry is still the file named above. */
    if(app_chunks_host_read(list))return 2;
    atexit(app_chunks_host_clear);
    app_registry_select("local.derby");
    if(getenv("DERBY_ALLOC_TRACE"))alloc_trace=fopen(getenv("DERBY_ALLOC_TRACE"),"w");
    rt=JS_NewRuntime2(&PEAK_MF,NULL);ctx=JS_NewContext(rt);
    // As pocketjs_guest_create: apps run hardened (docs/vm/harden.md).
    // POCKET_NO_HARDEN=1 skips it, for host A/B only.
    if(!ctx||(!getenv("POCKET_NO_HARDEN")&&JS_PocketHarden(ctx)<0)){fprintf(stderr,"JS_PocketHarden failed\n");abort();}
    host_capabilities_clear();
    const char *lim=getenv("DERBY_HEAP_LIMIT");
    if(lim){size_t l=(size_t)strtoul(lim,NULL,0);JS_SetMemoryLimit(rt,l);if(l/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,l/2);}
    /* The Shiki heap, as app_session.c takes it for an app naming
     * kasane.shiki (DERBY does): pocket.kasane then has shiki. */
    REQ(shiki_heap_begin(0));
    pocket_kasane_install(ctx,NULL);
    {   /* pocket.derby (the firmware's lazy namespace, built here at once). */
        JSValue g=JS_GetGlobalObject(ctx),ns=JS_NewObject(ctx);
        REQ(pocket_derby_fill(ctx,ns,NULL)==ESP_OK);
        JS_SetPropertyStr(ctx,g,"__pd",ns);JS_FreeValue(ctx,g);
    }
    pocket_input_install(ctx,NULL);
    if(pocket_app_load_install(ctx)!=ESP_OK)return 2;
    JSValue g=JS_GetGlobalObject(ctx);
    static const struct {const char *n;JSCFunction *f;int a;} fns[]={
        {"__log",js_log,1},{"__hr",js_hr,0},{"__begin",js_cap_begin,2},
        {"__sreg",js_cap_sreg,4},{"__sunreg",js_cap_sunreg,1},{"__sdraw",js_cap_sdraw,4},{"__ftag",js_ftag,3},{"__serbegin",js_serbegin,0},{"__sercmp",js_sercmp,2},{"__serdry",js_serdry,1},{"__commit",js_cap_commit,1},{"__tone",js_tone,1},{"__probe",js_probe,4},{"__churn",js_churn,1}};
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
    if(getenv("DERBY_PAN")){char b[96];snprintf(b,sizeof b,"[%s].forEach((v,i)=>PAN[i]=v|0)",getenv("DERBY_PAN"));
        REQ(eval(b,strlen(b),"pan.js"));}
    /* DERBY_JS: any script run after the app's (e.g. moving the screen so
     * that a panning unit sees it: run_derby.py). */
    if(getenv("DERBY_JS"))REQ(eval(getenv("DERBY_JS"),strlen(getenv("DERBY_JS")),"env.js"));
    /* DERBY_SER_SCALAR=1: the series' rotation and cull on the scalar path
     * (the up key's B side; docs/apps/derby-ser-division.md sec.13) instead of
     * the PIE kernels' (here their scalar reference, DERBY_SERPIE_HOST):
     * run_derby.py requires the same pixels. */
    if(getenv("DERBY_SER_SCALAR")){const char *m="pocket.derby.mode(1)";REQ(eval(m,strlen(m),"mode.js"));}
    /* DERBY_SER_ORACLE=1: the JS ser() of vm/main beside the C on every call,
     * then DERBY_SER_FUZZ random cases (default 4000) before the game. */
    if(getenv("DERBY_SER_ORACLE")){
        static char oracle[16384];
        FILE *of=fopen("tools/games/derby_ser_oracle.js","rb");REQ(of);
        const size_t on=fread(oracle,1,sizeof oracle-1,of);fclose(of);oracle[on]=0;
        REQ(eval(oracle,on,"derby_ser_oracle.js"));
        ser_oracle=true;
        char fz[96];
        snprintf(fz,sizeof fz,"__serfuzz(%d,%u)",getenv("DERBY_SER_FUZZ")?atoi(getenv("DERBY_SER_FUZZ")):4000,hw);
        REQ(eval(fz,strlen(fz),"fuzz.js"));
        printf("ser oracle fuzz: %lu calls, %lu draws\n",ser_fuzz_calls,ser_fuzz_draws);
    }
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
        pocket_input_reset();pocket_shiki_api_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
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
        /* DERBY_LEADSHOTS=<m>:<tag>,...: a panel when the leader first passes m. */
        static unsigned shot_done;
        unsigned shot_k=0;
        for(const char *p=getenv("DERBY_LEADSHOTS");p&&*p;shot_k++){
            unsigned m=0;char tag[32]={0};
            if(sscanf(p,"%u:%31[^,]",&m,tag)==2&&!(shot_done&1u<<shot_k)&&gnum("__L")>=m){shot_done|=1u<<shot_k;ppm(tag);
                printf("LEADSHOT %s %u lead %.1f f %.0f\n",tag,tick,gnum("__L"),cur_pan_f);}
            p=strchr(p,',');if(p)p++;
        }
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
           "frame_steps_max plans_live_max instr_max\n");
    for(unsigned i=0;i<nstats;i++){
        const scene_stats *s=&stats[i];
        printf("%-6s %6u %9u %7u %7.1f %10u %10.0f %15u %14u %15u %14u %9u\n",s->name,s->frames,s->draws_max,
               s->seg_max,(double)s->seg_sum/s->frames,s->raster_max,(double)s->raster_sum/s->frames,
               s->draw_raster_max,s->draw_steps_max,s->frame_steps_max,s->live_max,s->instr_max);
    }
    unsigned seg=0,ras=0,dras=0,dst=0,ins=0;
    for(unsigned i=0;i<nstats;i++){
        if(stats[i].seg_max>seg)seg=stats[i].seg_max;
        if(stats[i].raster_max>ras)ras=stats[i].raster_max;
        if(stats[i].draw_raster_max>dras)dras=stats[i].draw_raster_max;
        if(stats[i].draw_steps_max>dst)dst=stats[i].draw_steps_max;
        if(stats[i].instr_max>ins)ins=stats[i].instr_max;
    }
    printf("\nlimits used (max over the run): segments %u/%u (%.0f%%), raster %u/65535 (%.0f%%), "
           "draw raster %u/%u (%.0f%%), draw steps %u/%u (%.0f%%), instructions %u/%u, live plans %u/32, surfaces 1/2\n",
           seg,KSN_PROC_SEGMENTS,100.0*seg/KSN_PROC_SEGMENTS,ras,100.0*ras/65535,dras,KSN_SHIKI_RASTER,100.0*dras/KSN_SHIKI_RASTER,
           dst,KSN_SHIKI_STEPS,100.0*dst/KSN_SHIKI_STEPS,ins,KSN_SHIKI_CODE,live_max);
    printf("\nscreen: frames with the face %u (feed drawn in %u, %u segments wholly inside), fill/bezel errors %u, "
           "frames with anything else on the face %u (%u px), lettering shown in %u frames (%u node checks, %u off the "
           "face), refs max %u/32, commands max %u/80, HEAD ON frames %u\n",vis_frames,feed_frames,feed_segments,bezel_bad,
           occluded_frames,occluded_pixels,overlay_frames,rect_checked,rect_mismatch,refs_max,cmds_max,head_frames);
    printf("\npanning units: frames %u, f %.0f..%.0f, the screen in view %u\n",pan_frames,
           pan_frames?pan_f_lo:0,pan_f_hi,pan_face_frames);
    for(unsigned k=0;k<FT_N;k++)
        printf("boundary %s: %lu calls, %lu with doubles (%lu doubles)%s%s\n",FT_NAME[k],ft_calls[k],ft_bad_calls[k],
               ft_floats[k],ft_bad_calls[k]?", first: ":"",ft_first[k]);
    printf("\nplans: registered %u, unregistered %u, live max %u, most registrations in one frame %u, shiki draws %u\n",
           registered,unregistered,live_max,frame_reg_max,shiki_draws);
    printf("frames %u (committed frames checked against the replay %u, other surfaces %u): exceptions=%u "
           "bad_present=%u failures=%u tones=%u\n",tick,frames_checked,frames_unchecked,exceptions,bad_present,framefails,tones);
    printf("finish: %s\n",finishes?finish[0]:"-");
    if(finishes>1)printf("replay: %s\n",finish[1]);
    printf("pixels hash %016llx\n",(unsigned long long)pixel_hash);
#ifdef POCKET_PROC_VISIT_STATS
    {extern unsigned long long pocket_proc_visits[3][2];
     printf("proc visits 8-row: all=%llu B=%llu A=%llu | 4-row: all=%llu B=%llu A=%llu\n",
            pocket_proc_visits[0][0],pocket_proc_visits[1][0],pocket_proc_visits[2][0],
            pocket_proc_visits[0][1],pocket_proc_visits[1][1],pocket_proc_visits[2][1]);
     extern unsigned pocket_proc_visit_frames,pocket_proc_list_max[2],pocket_proc_entries_max;
     printf("proc frames=%u entries_max=%u list_max 8-row=%u 4-row=%u\n",pocket_proc_visit_frames,
            pocket_proc_entries_max,pocket_proc_list_max[0],pocket_proc_list_max[1]);}
#endif
    printf("strip_rows=%u panel_digest=%08x\n",(unsigned)strip_rows_arm,(unsigned)strip_panel_digest);
    printf("guest heap (TLSF model, %zu-bit host): before eval %zu, peak during eval %zu (+%zu), after eval %zu, "
           "live peak after GC %zu, whole-run peak %zu; device limit 163840\n",sizeof(void *)*8,before,eval_peak,
           eval_peak-before,after_eval,heap_live_peak,peak_bytes);
    for(unsigned i=0;i<5;i++)if(churn[i].frames)
        printf("guest heap churn in frame(), %s: %u frames, %.1f allocations and %.0f B a frame, largest rise in one "
               "frame %zu B, freed only by the cycle collector %zu B\n",CHURN_SCENE[i],churn[i].frames,
               (double)churn[i].n/churn[i].frames,(double)churn[i].b/churn[i].frames,churn[i].hi,churn[i].gc);
    /* Every op of the built-in Shiki table is drawn at least once. */
    uint64_t shiki_ops_rom=0;
    {unsigned count=0;const ksn_shiki_rom_entry *rom=ksn_shiki_rom_table(&count);
     for(unsigned k=0;k<count;k++){
         const size_t body=rom[k].len-12u*rom[k].code[6];   /* the instructions end the bytes (sec.3) */
         for(unsigned i=0;i<rom[k].code[6];i++)shiki_ops_rom|=1ull<<rom[k].code[body+12*i];}}
    printf("shiki heap: used max %u, largest free min %u (of %u)\n",sh_used_max,sh_largest_min,SHIKI_HEAP_DEFAULT_BYTES);
    printf("shiki ops: built-in %llx, drawn %llx\n",(unsigned long long)shiki_ops_rom,(unsigned long long)shiki_ops_drawn);
    bool pass=!exceptions&&!bad_present&&!framefails&&frames_checked&&(shiki_ops_drawn&shiki_ops_rom)==shiki_ops_rom&&shiki_draws>0&&frame_reg_max<=1&&go_seen>=1&&
              !rect_mismatch&&!bezel_bad&&!occluded_frames&&vis_frames&&feed_frames&&
              (getenv("DERBY_PANFACE")?pan_face_frames>0:overlay_frames>0)&&refs_max<=32&&cmds_max<=80;
    /* No cycles: the device would keep them until the heap is nearly full
     * (a recursive closure in the plan decoder left ~4 KB a race, LOADSTALL). */
    for(unsigned i=0;i<5;i++)pass=pass&&!churn[i].gc;
    if(full)pass=pass&&finishes==2&&saves==1&&tones>0&&demo_starts==10&&demo_ends==10;
    else pass=pass&&!demo_starts;
    if(csv)fclose(csv);
    if(ser_oracle){
        printf("ser oracle: %lu calls in the game, %lu draws, %lu fuzz calls\n",ser_calls,ser_draws,ser_fuzz_calls);
        pass=pass&&ser_fuzz_calls>0;
        /* The integer calls (tolerance). */
        printf("ser tolerance: %lu integer calls (another draw count %lu), on-screen points %lu: <=0.01 px %lu, <=0.1 %lu, "
               "<=0.5 %lu, <=1 %lu, <=2 %lu, >2 %lu; worst %.4f px; calls over 2 px %lu, drawn by the JS only %lu\n",
               tol_calls,tol_draw_diff,tol_pts,tol_hist[0],tol_hist[1],tol_hist[2],tol_hist[3],tol_hist[4],tol_hist[5],
               tol_worst,tol_swaps,tol_missing);
        pass=pass&&!tol_missing&&tol_swaps*10000<=tol_calls+9999;
    }
    /* The integer series' invariants (every run; sec.5.2). */
    {
        derby_ser_stats st;pocket_derby_ser_stats(&st);
        printf("ser integers: chords %u (drawn %u), draws %u, saturated %u, skipped %u, newton start %u step %u, counts %u, "
               "joins %u (over 1 px %u, worst %.4f px)\n",st.chords,st.drawn,st.draws,st.sat,st.bad,st.newton_start,
               st.newton_step,st.count_range,st.joins,st.join_bad,st.join_worst);
        pass=pass&&!st.sat&&!st.bad&&!st.newton_start&&!st.newton_step&&!st.count_range&&!st.join_bad&&
             (!getenv("DERBY_PAN")||st.draws>0);
    }
    pocket_input_reset();pocket_shiki_api_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s\n",pass?"DERBY_HOST PASS":"DERBY_HOST FAIL");
    return pass?0:1;
}
