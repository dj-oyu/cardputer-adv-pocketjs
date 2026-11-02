#include "pocket_proc.h"
#ifdef KSN_PROC_HOST_TEST
ksn_result pocket_kasane_proc_publish(void);
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage);
void pocket_kasane_invalidate(void);
JSValue pocket_kasane_proc_resource(JSContext *ctx);
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface);
#else
#include "pocket_kasane.h"
#endif
#include "pocket_api.h"
#include "ui/kasane/ksn_procedural.h"
#include "ui/kasane/ksn_shiki.h"
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#endif
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef KASANE_PROC_JS_DIAGNOSTIC
#include "esp_cpu.h"
static uint32_t image_band_count,image_band_cycles,image_span_count,image_span_cycles;
void pocket_proc_image_prof_read(uint32_t *band_count,uint32_t *band_cycles,
                                 uint32_t *span_count,uint32_t *span_cycles){
    if(band_count)*band_count=image_band_count;
    if(band_cycles)*band_cycles=image_band_cycles;
    if(span_count)*span_count=image_span_count;
    if(span_cycles)*span_cycles=image_span_cycles;
    image_band_count=image_band_cycles=image_span_count=image_span_cycles=0;
}
#endif
#ifdef KASANE_MEGADEMO_TRACE
/* Separate from the JS diagnostic above, which is read per 30-frame window:
 * the trace needs these per host turn, and it times the JS-visible calls
 * (register includes reading the arrays, prepare is the analysis alone). */
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
static pocket_proc_trace trace;
/* Survives trace_take(): a commit and its view update can be turns apart. */
static bool trace_split;
void pocket_proc_trace_take(pocket_proc_trace *out){
    if(out)*out=trace;
    trace=(pocket_proc_trace){0};
}
void pocket_proc_trace_view(void){trace.view_n++;trace_split=false;}
void pocket_proc_trace_presented(void){if(trace_split)trace.split_n++;}
#endif
// Counts this file's allocations when CONFIG_POCKET_SHIKI_MEMSTAT is on
// (docs/kasane/shiki.md sec.4.5); nothing otherwise.
#define SHIKI_MS_FILE SHIKI_MS_PROC
#include "shiki_memstat.h"
// Frames, the engine's state and programs come from the Shiki heap when the
// session has one (kasane.shiki, docs/kasane/shiki-spec.md sec.9.2), from the
// system heap otherwise.
#include "shiki_heap.h"

/* Slots are reusable after unregister(); handles are not (next_handle only
 * grows), so a released handle can never alias a later program. */
#define PROC_HANDLES 32u
#define PROC_SURFACES 2u
/* A surface's candidate and committed frames hold only the segments it
 * declared (createSurface({maxSegments})), allocated as the ksn_proc_frame
 * header plus that many entries. The full 1,024-entry frame is 10,246 B, and
 * a second surface needs two of those as separate contiguous blocks: DERBY
 * WATCH's heap had a largest free block of 10,240 B, so the second surface
 * could not exist at all (docs/kasane/surface-segment-cap.md). Nothing reads a
 * frame past count (render_band, damage, image spans), and nothing copies or
 * clears a whole ksn_proc_frame here, which is what makes the short
 * allocation safe; scratch and the VM stay full size because one plan may
 * emit up to KSN_PROC_SEGMENTS before draw() checks the surface's room. */
_Static_assert(offsetof(ksn_proc_frame,segments)+
               KSN_PROC_SEGMENTS*sizeof(ksn_proc_segment)==sizeof(ksn_proc_frame),
               "segments must be the frame's last member, with no tail padding");
static size_t frame_bytes(unsigned segments){
    return offsetof(ksn_proc_frame,segments)+(size_t)segments*sizeof(ksn_proc_segment);
}

/* plan is a ksn_shiki_plan (pocket.kasane.shiki, pocket_shiki_api.c), the
 * only kind since v2.0 removed the float VM. Handles never exceed
 * PROC_HANDLE_MAX. */
#define PROC_HANDLE_MAX ((1u<<30)-1u)
typedef struct {
    uint32_t handle;
    void *plan;
} proc_slot;
static proc_slot slots[PROC_HANDLES];
static uint32_t next_handle;
/* The Shiki engine's state for draw (pocket_proc_shiki_draw), taken with the
 * frames on the first Shiki draw. */
static ksn_shiki_vm *shiki_vm;
/* What a frame shows on each 8-row band of the image: a hash of the ordered
 * entries that touch the band (its pixels are a function of the backdrop
 * colour and that list alone, ksn_proc_render_band) and their columns. Two
 * frames whose band hashes match draw the band alike, so commit() damages
 * only the bands whose hash changed (pocket_proc_band_damage). */
#define PROC_BANDS ((KSN_PROC_H+7)/8)
typedef struct {
    uint32_t hash[PROC_BANDS];
    uint8_t x0[PROC_BANDS],x1[PROC_BANDS];
} proc_band_sig;
typedef struct {
    ksn_proc_frame *candidate,*committed;
    proc_band_sig candidate_sig,committed_sig;
    uint16_t candidate_color,committed_color;
    bool pending,has_committed,repair_required,image_mode;
    bool sig_ready; /* candidate_sig describes the candidate (commit .. next beginFrame) */
    uint32_t handle;
    ksn_rect damage;
    uint16_t max_segments; /* 0 = KSN_PROC_SEGMENTS (no option) */
} proc_surface;
static proc_surface surfaces[PROC_SURFACES];
static uint32_t next_surface_handle;
static unsigned building_surface,pending_surface;
/* Plan execution and image rasterization are synchronous owner-task phases.
 * Rasterization reads candidate/committed frames, never this workspace. */
#define PROC_IMAGE_BAND_ROWS 8u
typedef union {
    ksn_proc_frame frame;
    uint16_t band[KSN_PROC_W*PROC_IMAGE_BAND_ROWS];
} proc_workspace;
_Static_assert(sizeof(proc_workspace)==sizeof(ksn_proc_frame),
               "image raster cache must not enlarge the VM workspace");
static proc_workspace *workspace;
static bool building;
/* A source row can be requested repeatedly by scaled image draws. Cache the
 * whole eight-row raster band so adjacent rows share one segment traversal. */
static uint16_t image_band_y;
static const ksn_proc_frame *image_band_frame;
static uint16_t image_band_color;
static unsigned image_band_surface;
static bool image_band_valid;
/* JS array elements may be accessors or Proxy traps. Nested calls must not
 * mutate a plan slot or frame while an outer call is still reading fields. */
static bool js_call_active;

static JSValue failure(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst value,double *out){
    return JS_IsNumber(value)&&JS_ToFloat64(ctx,out,value)==0&&isfinite(*out);
}
static bool integer(JSContext *ctx,JSValueConst value,uint32_t max,uint32_t *out){
    double n;
    if(!number(ctx,value,&n)||n<0||n>max||n!=floor(n))return false;
    *out=(uint32_t)n;return true;
}
static unsigned surface_segments(const proc_surface *surface){
    return surface->max_segments?surface->max_segments:KSN_PROC_SEGMENTS;
}
static bool buffers(proc_surface *surface){
    const size_t bytes=frame_bytes(surface_segments(surface));
    if(!surface->candidate)surface->candidate=shiki_or_sys_calloc(1,bytes);
    if(!workspace)workspace=shiki_or_sys_calloc(1,sizeof *workspace);
    if(!surface->committed)surface->committed=shiki_or_sys_calloc(1,bytes);
    return surface->candidate&&workspace&&surface->committed;
}
static bool any_pending(void){
    for(unsigned i=0;i<PROC_SURFACES;i++)if(surfaces[i].pending)return true;
    return false;
}
static bool any_repair(void){
    for(unsigned i=0;i<PROC_SURFACES;i++)if(surfaces[i].repair_required)return true;
    return false;
}
static bool surface_index(JSContext *ctx,JSValueConst value,unsigned *index){
    uint32_t handle;
    if(!integer(ctx,value,INT32_MAX,&handle))return false;
    if(handle==0){*index=0;return true;}
    for(unsigned i=1;i<PROC_SURFACES;i++)if(surfaces[i].handle==handle){
        *index=i;return true;
    }
    return false;
}
/* Handle 0 is never issued and is also what an empty slot holds, so a
 * lookup must require a plan, not just a matching number. */
static proc_slot *find_slot(uint32_t handle){
    if(!handle)return NULL;
    for(unsigned i=0;i<PROC_HANDLES;i++)
        if(slots[i].plan&&slots[i].handle==handle)return &slots[i];
    return NULL;
}
static ksn_rect frame_damage(const proc_surface *surface){
    if(!surface->has_committed||surface->candidate_color!=surface->committed_color)
        return (ksn_rect){0,0,KSN_PROC_W,KSN_PROC_H};
    int x0=KSN_PROC_W,y0=KSN_PROC_H,x1=0,y1=0;
    const ksn_proc_frame *frames[]={surface->candidate,surface->committed};
    for(unsigned f=0;f<2;f++){
      unsigned i=0,cost;
      ksn_proc_segment geometry;
      bool pattern;
      /* Geometry only: a pattern line's parameters are not coordinates. */
      while(ksn_proc_frame_next(frames[f],&i,&geometry,&pattern,&cost)){
        const ksn_proc_segment *seg=&geometry;
        int ax=seg->x0<seg->x1?seg->x0:seg->x1;
        int bx=seg->x0>seg->x1?seg->x0:seg->x1;
        int ay=seg->y0<seg->y1?seg->y0:seg->y1;
        int by=seg->y0>seg->y1?seg->y0:seg->y1;
        if(ax<x0)x0=ax;
        if(ay<y0)y0=ay;
        if(bx+1>x1)x1=bx+1;
        if(by+1>y1)y1=by+1;
      }
    }
    /* VM and typed segments may lie anywhere in -480..720. Clamping each edge
     * keeps x0<=x1 and y0<=y1 for any non-empty set; a set wholly off one side
     * collapses to a zero-width rectangle on that edge. */
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>KSN_PROC_W)x1=KSN_PROC_W;
    if(y1>KSN_PROC_H)y1=KSN_PROC_H;
    return (ksn_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};
}
static void band_signature(const ksn_proc_frame *frame,proc_band_sig *sig){
    for(unsigned b=0;b<PROC_BANDS;b++){sig->hash[b]=2166136261u;sig->x0[b]=255;sig->x1[b]=0;}
    unsigned i=0,cost;
    ksn_proc_segment geometry;
    bool pattern;
    for(;;){
        const unsigned start=i;
        if(!ksn_proc_frame_next(frame,&i,&geometry,&pattern,&cost))break;
        const int ya=geometry.y0<geometry.y1?geometry.y0:geometry.y1,yb=geometry.y0^geometry.y1^ya;
        int xa=geometry.x0<geometry.x1?geometry.x0:geometry.x1,xb=(geometry.x0^geometry.x1^xa)+1;
        if(yb<0||ya>=KSN_PROC_H||xb<=0||xa>=KSN_PROC_W)continue;
        if(xa<0)xa=0;
        if(xb>KSN_PROC_W)xb=KSN_PROC_W;
        const unsigned b0=(unsigned)(ya<0?0:ya)/8u,b1=(unsigned)(yb>=KSN_PROC_H?KSN_PROC_H-1:yb)/8u;
        for(unsigned b=b0;b<=b1;b++){
            /* The raw entries, a pattern line's parameters included. */
            const uint8_t *bytes=(const uint8_t *)&frame->segments[start];
            uint32_t h=sig->hash[b];
            for(size_t k=0;k<(size_t)(i-start)*sizeof frame->segments[0];k++)h=(h^bytes[k])*16777619u;
            sig->hash[b]=h;
            if(xa<sig->x0[b])sig->x0[b]=(uint8_t)xa;
            if(xb>sig->x1[b])sig->x1[b]=(uint8_t)xb;
        }
    }
}
bool pocket_proc_band_damage(unsigned index,uint32_t *bands,int16_t x0[],int16_t x1[]){
#ifdef POCKET_PROC_NO_BAND_DAMAGE
    /* Host A/B only (run_megademo_app_host.py --no-band-damage). */
    (void)bands;(void)x0;(void)x1;(void)index;
    return false;
#endif
    if(index>=PROC_SURFACES||!surfaces[index].sig_ready)return false;
    const proc_surface *s=&surfaces[index];
    const bool all=!s->has_committed||s->candidate_color!=s->committed_color;
    *bands=0;
    for(unsigned b=0;b<PROC_BANDS;b++){
        const proc_band_sig *n=&s->candidate_sig,*o=&s->committed_sig;
        if(!all&&n->hash[b]==o->hash[b])continue;
        *bands|=1u<<b;
        if(all){x0[b]=0;x1[b]=KSN_PROC_W;continue;}
        /* The columns either frame drew on the band. */
        const unsigned a=n->x0[b]<o->x0[b]?n->x0[b]:o->x0[b],z=n->x1[b]>o->x1[b]?n->x1[b]:o->x1[b];
        x0[b]=(int16_t)(a<z?a:0);x1[b]=(int16_t)(a<z?z:0);
    }
    return true;
}
static JSValue begin_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.beginFrame";
    uint32_t color;
    unsigned index=0;
    if((argc!=1&&argc!=2)||!integer(ctx,argv[0],UINT16_MAX,&color)||
       (argc==2&&!surface_index(ctx,argv[1],&index)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected RGB565 and optional live surface ID");
    if(any_pending()||any_repair())
        return failure(ctx,op,POCKET_ERR_BUSY,"previous frame awaits presentation or repair");
    proc_surface *surface=&surfaces[index];
    if(!buffers(surface))return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"frame allocation failed");
    /* Header only: the frame may be shorter than sizeof(ksn_proc_frame). */
    surface->candidate->count=0;
    surface->candidate->raster_steps=0;
    surface->candidate->ready=false;
    surface->candidate->ext=0;
    surface->sig_ready=false;
    surface->candidate_color=(uint16_t)color;
    building_surface=index;
    building=true;
    return JS_UNDEFINED;
}
/* Native drawing straight into the candidate (pocket_shape.c): draw()'s
 * frame limits without a plan or the VM. */
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    if(js_call_active)return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    const ksn_proc_frame *c=surfaces[building_surface].candidate;
    *mark=(pocket_proc_mark){c->count,c->raster_steps,c->ext,(uint8_t)building_surface};
    image_band_valid=false;
    return JS_UNDEFINED;
}
JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,uint16_t color){
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    if(x0< -480||x0>720||y0< -480||y0>720||x1< -480||x1>720||y1< -480||y1>720){
        building=false;
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"segment outside -480..720");
    }
    ksn_proc_frame *c=surfaces[building_surface].candidate;
    const unsigned dx=(unsigned)abs(x1-x0),dy=(unsigned)abs(y1-y0),cost=(dx>dy?dx:dy)+1u;
    if(c->count>=surface_segments(&surfaces[building_surface])||
       cost>(unsigned)UINT16_MAX-c->raster_steps){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"frame drawing limit");
    }
    c->segments[c->count++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,color};
    c->raster_steps=(uint16_t)(c->raster_steps+cost);
    return JS_UNDEFINED;
}
void pocket_proc_native_rollback(const pocket_proc_mark *mark){
    ksn_proc_frame *c=surfaces[mark->surface<PROC_SURFACES?mark->surface:0].candidate;
    if(!c)return;
    c->count=mark->count;c->raster_steps=mark->raster;c->ext=mark->ext;
}
static JSValue commit_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.shiki.commit";
    if(argc||!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    proc_surface *surface=&surfaces[building_surface];
    surface->damage=frame_damage(surface);
    band_signature(surface->candidate,&surface->candidate_sig);
    surface->sig_ready=true;
    if(building_surface&&!surface->image_mode)
        return failure(ctx,op,POCKET_ERR_BUSY,"resource(surface ID) required");
    ksn_result result=building_surface||surface->image_mode?
        pocket_kasane_proc_publish_at(building_surface,surface->damage):
        pocket_kasane_proc_publish();
    if(result!=KSN_OK)return failure(ctx,op,result==KSN_OOM?POCKET_ERR_OUT_OF_MEMORY:
                                   POCKET_ERR_BUSY,"Kasane presentation unavailable");
    surface->candidate->ready=true;
    surface->pending=true;
    pending_surface=building_surface;
    image_band_valid=false;
    building=false;
#ifdef KASANE_MEGADEMO_TRACE
    trace.commit_n++;trace_split=true;
#endif
    return JS_UNDEFINED;
}
typedef JSValue (*proc_js_method)(JSContext *,JSValueConst,int,JSValueConst *);
static JSValue guarded(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                       proc_js_method method){
    if(js_call_active)
        return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    js_call_active=true;
    JSValue result=method(ctx,self,argc,argv);
    js_call_active=false;
    return result;
}
static JSValue js_begin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,begin_impl);
}
#ifdef POCKET_PROC_DRAW_HOOK
bool pocket_proc_hook_dry;
#endif
/* One draw for a native caller (pocket_derby.c): a Shiki handle, the inputs
 * converted by sec.2.4 of the spec. */
JSValue pocket_proc_draw_numbers(JSContext *ctx,uint32_t handle,const double *in,unsigned n){
    const char *op="kasane.shiki.draw";
#ifdef POCKET_PROC_DRAW_HOOK
    if(pocket_proc_hook_dry){pocket_proc_draw_hook(handle,in,n);return JS_UNDEFINED;}
#endif
    if(handle>INT32_MAX||n>KSN_SHIKI_INPUTS)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected handle and 0..8 inputs");
    JSValue result=pocket_proc_shiki_draw_inputs(ctx,handle,in,NULL,n,0);
#ifdef POCKET_PROC_DRAW_HOOK
    if(!JS_IsException(result))pocket_proc_draw_hook(handle,in,n);
#endif
    return result;
}
static JSValue js_commit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,commit_impl);
}
/* {maxSegments: 1..1024} (absent or undefined: 0, the 1,024 default). */
static bool segments_option(JSContext *ctx,JSValueConst object,uint32_t *segments){
    *segments=0;
    JSValue v=JS_GetPropertyStr(ctx,object,"maxSegments");
    bool ok=!JS_IsException(v)&&(JS_IsUndefined(v)||
            (integer(ctx,v,KSN_PROC_SEGMENTS,segments)&&*segments>0));
    JS_FreeValue(ctx,v);
    if(!ok)*segments=0;
    return ok;
}
static JSValue resource_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.resource";
    unsigned index=0;
    /* resource({maxSegments}): surface 0's image, its two frames sized for
     * maxSegments (createSurface()'s option for surface 1). Decided before
     * surface 0's first beginFrame allocates them; after that only the same
     * value is accepted. */
    if(argc==1&&JS_IsObject(argv[0])){
        uint32_t segments=0;
        if(!segments_option(ctx,argv[0],&segments))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"maxSegments must be an integer 1..1024");
        if(surfaces[0].candidate&&surface_segments(&surfaces[0])!=(segments?segments:KSN_PROC_SEGMENTS))
            return failure(ctx,op,POCKET_ERR_BUSY,"surface 0's frames are already allocated");
        surfaces[0].max_segments=(uint16_t)segments;
        return pocket_kasane_proc_resource(ctx);
    }
    if(argc>1||(argc==1&&!surface_index(ctx,argv[0],&index)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                       "expected optional live surface ID or {maxSegments}");
    return index?pocket_kasane_proc_resource_at(ctx,index):pocket_kasane_proc_resource(ctx);
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,resource_impl);
}
/* createSurface() or createSurface({maxSegments: 1..1024}). The option only
 * shrinks the surface's two frames; it is enforced by draw() exactly as the
 * 1,024 default is (LIMIT_EXCEEDED "frame drawing limit", candidate kept
 * short of the draw). The limit is decided before the surface exists: a bad
 * option issues no handle. */
static JSValue create_surface_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.createSurface";
    uint32_t segments=0;
    if(argc>1)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                             "expected optional {maxSegments}");
    if(argc==1&&!JS_IsUndefined(argv[0])){
        if(!JS_IsObject(argv[0]))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected optional {maxSegments}");
        if(!segments_option(ctx,argv[0],&segments))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                           "maxSegments must be an integer 1..1024");
    }
    if(surfaces[1].handle||next_surface_handle==INT32_MAX)
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"two surfaces maximum");
    surfaces[1].max_segments=(uint16_t)segments;
    surfaces[1].handle=++next_surface_handle;
    return JS_NewInt32(ctx,(int32_t)surfaces[1].handle);
}
static JSValue js_create_surface(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,create_surface_impl);
}
/* ---- Shiki programs (pocket.kasane.shiki, pocket_shiki_api.c) */
uint32_t pocket_proc_shiki_attach(void *plan){
    unsigned slot=0;
    while(slot<PROC_HANDLES&&slots[slot].plan)slot++;
    if(slot==PROC_HANDLES||next_handle==PROC_HANDLE_MAX)return 0;
    const uint32_t handle=++next_handle;
    slots[slot]=(proc_slot){.handle=handle,.plan=plan};
    return handle;
}
void *pocket_proc_shiki_plan(uint32_t handle){
    proc_slot *slot=find_slot(handle);
    return slot?slot->plan:NULL;
}
bool pocket_proc_shiki_detach(uint32_t handle){
    /* As unregister(): legal between beginFrame() and commit(), since draw
     * copies the segments out and keeps nothing of the plan. */
    proc_slot *slot=find_slot(handle);
    if(!slot)return false;
    ksn_shiki_plan_dispose(slot->plan);
    shiki_or_sys_free(slot->plan);
    *slot=(proc_slot){0};
    return true;
}
pocket_shiki_draw_prof pocket_shiki_prof;
static pocket_shiki_turn shiki_turn;
void pocket_shiki_turn_take(pocket_shiki_turn *out){*out=shiki_turn;shiki_turn=(pocket_shiki_turn){0};}
uint32_t pocket_cycles(void){
#ifdef ESP_PLATFORM
    return esp_cpu_get_cycle_count();
#else
    return 0;
#endif
}
JSValue pocket_proc_shiki_draw(JSContext *ctx,uint32_t handle,const double *in,unsigned n){
    return pocket_proc_shiki_draw_inputs(ctx,handle,in,NULL,n,0);
}
JSValue pocket_proc_shiki_draw_inputs(JSContext *ctx,uint32_t handle,const double *in,const int32_t *fixed,unsigned n,
                                      unsigned index){
    const char *op="kasane.shiki.draw";
    const uint32_t t_in=pocket_cycles();
    if(js_call_active)return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    proc_slot *slot=find_slot(handle);
    if(!slot)return failure(ctx,op,POCKET_ERR_CLOSED,"stale Shiki handle");
    if(!shiki_vm&&!(shiki_vm=shiki_or_sys_calloc(1,sizeof *shiki_vm))){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"no room for the engine's state");
    }
    ksn_proc_frame *candidate=surfaces[building_surface].candidate;
    const unsigned room=surface_segments(&surfaces[building_surface]);
    ksn_proc_segment *scratch=workspace->frame.segments;
    image_band_valid=false;
    pocket_shiki_draw_prof *pf=&pocket_shiki_prof;
    if(pf->on){const uint32_t t=pocket_cycles();pf->lookup+=t-pf->mark;pf->mark=t;}
    const uint32_t t_engine=pocket_cycles();
    /* Real inputs: sec.2.4's conversion (ksn_shiki_draw's), then the draw by
     * its number (DRAW_INDEX, Shiki v2.0 draft sec.4.1). */
    ksn_shiki_status s;
    if(fixed)s=ksn_shiki_draw_index(slot->plan,fixed,n,index,shiki_vm,scratch);
    else{
        int32_t fx[KSN_SHIKI_INPUTS]={0};
        const ksn_shiki_plan *pl=slot->plan;
        bool good=true;
        for(unsigned k=0;k<pl->inputs&&good;k++)
            good=ksn_shiki_from_double(k<n?in[k]:0.0,pl->input_q[k],&fx[k]);
        if(good)s=ksn_shiki_draw_index(pl,fx,pl->inputs,index,shiki_vm,scratch);
        else{memset(shiki_vm,0,sizeof *shiki_vm);s=KSN_SHIKI_BADINPUT;}
    }
    if(pf->on){const uint32_t t=pocket_cycles();pf->engine+=t-pf->mark;pf->mark=t;}
    shiki_turn.engine_cy+=pocket_cycles()-t_engine;
    shiki_turn.steps+=shiki_vm->steps;
    shiki_turn.draws++;
    if(s!=KSN_SHIKI_DONE){
        building=false;
        return failure(ctx,op,s==KSN_SHIKI_LIMIT?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,
                       s==KSN_SHIKI_BADINPUT?"non-finite input":"Shiki execution failed");
    }
    if((unsigned)shiki_vm->count>room-candidate->count||
       (unsigned)shiki_vm->raster>(unsigned)UINT16_MAX-candidate->raster_steps){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"frame drawing limit");
    }
    memcpy(&candidate->segments[candidate->count],scratch,shiki_vm->count*sizeof *scratch);
    candidate->count+=shiki_vm->count;
    candidate->raster_steps+=shiki_vm->raster;
    if(shiki_vm->ext)candidate->ext=1;
    if(pf->on){pf->copy+=pocket_cycles()-pf->mark;pf->n++;}
    shiki_turn.total_cy+=pocket_cycles()-t_in;
    return JS_UNDEFINED;
}

// The frame's functions on kasane.shiki (pocket_shiki_api.c).
static const JSCFunctionListEntry frame_methods[]={
    JS_CFUNC_DEF("beginFrame",1,js_begin),
    JS_CFUNC_DEF("commit",0,js_commit),
    JS_CFUNC_DEF("resource",0,js_resource),
    JS_CFUNC_DEF("createSurface",0,js_create_surface),
};
esp_err_t pocket_proc_frame_install(JSContext *ctx,JSValueConst obj){
    return JS_SetPropertyFunctionList(ctx,obj,frame_methods,
                                      (int)(sizeof frame_methods/sizeof frame_methods[0]))<0?ESP_ERR_NO_MEM:ESP_OK;
}
void pocket_proc_reset(void){
    for(unsigned i=0;i<PROC_HANDLES;i++){
        if(slots[i].plan)ksn_shiki_plan_dispose(slots[i].plan);
        shiki_or_sys_free(slots[i].plan);slots[i]=(proc_slot){0};
    }
    for(unsigned i=0;i<PROC_SURFACES;i++){
        shiki_or_sys_free(surfaces[i].candidate);shiki_or_sys_free(surfaces[i].committed);
        surfaces[i]=(proc_surface){0};
    }
    shiki_or_sys_free(workspace);workspace=NULL;
    shiki_or_sys_free(shiki_vm);shiki_vm=NULL;
    image_band_valid=false;image_band_frame=NULL;
    building=js_call_active=false;building_surface=pending_surface=0;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_band_count=image_band_cycles=image_span_count=image_span_cycles=0;
#endif
#ifdef KASANE_MEGADEMO_TRACE
    trace=(pocket_proc_trace){0};trace_split=false;
#endif
}
void pocket_proc_end_turn(void){
    /* A thrown JS frame must not carry an unfinished candidate into the next
     * owner turn. Previously committed or pending presentation stays intact.
     * Not called at the end of a host turn that the VM parked: that frame()
     * resumes in the continuation and still owns its candidate
     * (pocket_kasane_park_turn, end_guest_turn in app_session.c). */
    building=false;
}
bool pocket_proc_has_frame(void){
    const proc_surface *s=&surfaces[0];
    return s->pending||s->has_committed||s->repair_required;
}
bool pocket_proc_pending(void){return any_pending();}
bool pocket_proc_backdrop_pending(void){return surfaces[0].pending&&!surfaces[0].image_mode;}
bool pocket_proc_is_image_mode(void){return surfaces[0].image_mode;}
void pocket_proc_image_mode_at(unsigned index){
    if(index<PROC_SURFACES)surfaces[index].image_mode=true;
}
void pocket_proc_image_mode(void){pocket_proc_image_mode_at(0);}
static ksn_result image_span(void *ctx,uint16_t variant,uint16_t frame_number,
                             uint16_t y,uint16_t x,uint16_t count,
                             uint16_t *rgb565,uint8_t *alpha){
    uintptr_t key=(uintptr_t)ctx;
    if(key<1||key>PROC_SURFACES)return KSN_STALE;
    unsigned index=(unsigned)(key-1);
    proc_surface *surface=&surfaces[index];
    if(variant||frame_number||y>=KSN_PROC_H||x>KSN_PROC_W||
       count>KSN_PROC_W-x||(count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    uint32_t span_start=esp_cpu_get_cycle_count();
#endif
    const ksn_proc_frame *frame=surface->pending?surface->candidate:
        (surface->has_committed?surface->committed:NULL);
    uint16_t color=surface->pending?surface->candidate_color:
        (surface->has_committed?surface->committed_color:0);
    /* Before the first frame or after reset, a blank image needs no
     * workspace allocation, including when allocations would fail. */
    if(!frame){
        for(unsigned i=0;i<count;i++)rgb565[i]=color;
        memset(alpha,255,count);
        goto span_done;
    }
    uint16_t *image_band=workspace->band;
    uint16_t band_y=(uint16_t)(y&~(PROC_IMAGE_BAND_ROWS-1u));
    if(!image_band_valid||image_band_surface!=index||
       image_band_y!=band_y||image_band_frame!=frame||
       image_band_color!=color){
        image_band_valid=false;
        unsigned rows=KSN_PROC_H-band_y;
        if(rows>PROC_IMAGE_BAND_ROWS)rows=PROC_IMAGE_BAND_ROWS;
#if defined(KASANE_PROC_JS_DIAGNOSTIC)||defined(KASANE_MEGADEMO_TRACE)
        uint32_t band_start=esp_cpu_get_cycle_count();
#endif
        for(unsigned i=0;i<rows*KSN_PROC_W;i++)image_band[i]=color;
        if(frame&&!ksn_proc_render_band(frame,image_band,band_y,(int)rows))return KSN_INVALID;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
        image_band_cycles+=esp_cpu_get_cycle_count()-band_start;
        image_band_count++;
#endif
#ifdef KASANE_MEGADEMO_TRACE
        trace.band_cy+=esp_cpu_get_cycle_count()-band_start;
        trace.band_n++;
#endif
        image_band_surface=index;image_band_y=band_y;
        image_band_frame=frame;image_band_color=color;
        image_band_valid=true;
    }
    memcpy(rgb565,image_band+(unsigned)(y-band_y)*KSN_PROC_W+x,count*sizeof *rgb565);
    memset(alpha,255,count);
span_done:
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_span_cycles+=esp_cpu_get_cycle_count()-span_start;
    image_span_count++;
#endif
    return KSN_OK;
}
void pocket_proc_image_port_at(ksn_image_port *out,unsigned index){
    if(out&&index<PROC_SURFACES)*out=(ksn_image_port){.ctx=(void *)(uintptr_t)(index+1),
                              .width=KSN_PROC_W,.height=KSN_PROC_H,
                              .variants=1,.frames=1,.read_span=image_span,.opaque=true};
}
void pocket_proc_image_port(ksn_image_port *out){pocket_proc_image_port_at(out,0);}
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels){
    (void)ctx;
    if(!pixels||y>KSN_PROC_H||rows>KSN_PROC_H-y)return KSN_INVALID;
    const proc_surface *surface=&surfaces[0];
    const ksn_proc_frame *frame=surface->pending?surface->candidate:
        (surface->has_committed?surface->committed:NULL);
    uint16_t color=surface->pending?surface->candidate_color:
        (surface->has_committed?surface->committed_color:0);
    for(unsigned i=0;i<(unsigned)rows*KSN_PROC_W;i++)pixels[i]=color;
    return !frame||ksn_proc_render_band(frame,pixels,y,rows)?KSN_OK:KSN_INVALID;
}
void pocket_proc_present_result(ksn_result result){
    proc_surface *surface=&surfaces[pending_surface];
    bool repairing=false;
    for(unsigned i=0;i<PROC_SURFACES;i++)repairing|=surfaces[i].repair_required;
    if(!surface->pending&&!repairing)return;
    if(result==KSN_OK){
        if(surface->pending){
            ksn_proc_frame *old=surface->committed;
            surface->committed=surface->candidate;surface->candidate=old;
            surface->committed_color=surface->candidate_color;
            surface->committed_sig=surface->candidate_sig;
            surface->has_committed=true;surface->pending=false;
            image_band_valid=false;
        }
        for(unsigned i=0;i<PROC_SURFACES;i++)surfaces[i].repair_required=false;
    }else if(result==KSN_IO){
        image_band_valid=false;
        if(surface->image_mode){
            /* Kasane retries a submitted UI patch against the same image
             * resource. Keep its pixels paired with that ticket until ACK. */
            surface->repair_required=true;
        }else{
            surface->pending=false;
            /* A backdrop has no submitted image node: repaint the old frame. */
            surface->repair_required=true;
        }
        if(!surface->image_mode)pocket_kasane_invalidate();
    }
}
void pocket_proc_overlay_present_result(ksn_result result){
    if(any_pending()&&surfaces[pending_surface].image_mode)
        pocket_proc_present_result(result);
}
