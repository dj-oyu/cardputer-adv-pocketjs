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
#include "ui/kasane/ksn_proc_plan.h"
#include <math.h>
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
#ifdef KASANE_BGCOST_TRACE
#include <stdio.h>
#include "esp_log.h"
static uint32_t bg_steps,bg_commit_us,bg_view_us;
void pocket_proc_bgcost_view_us(uint32_t us){bg_view_us+=us;}
/* Per slot, over one BGP window; bg_last is the slot draw_impl ran so that
 * js_draw can charge its wall time to it. */
static struct { uint32_t handle,n,us,steps,seg; } bg_plan[32];
static int bg_last=-1;
#endif

/* Slots are reusable after unregister(); handles are not (next_handle only
 * grows), so a released handle can never alias a later plan. Worst case with
 * every slot holding a 128-point batch is 32*(plan+points) heap: see
 * docs/kasane/procedural-ir-experiment.md for the byte count. A batch pays
 * for its own point count, not for the cap (points_bytes() below). */
#define PROC_HANDLES 32u
/* A multiple of 8 keeps whole PIE blocks; the dispatcher handles any tail. */
#define PROC_POINT_MAX 128u
/* Typed points share the VM's coordinate bound (ksn_procedural.c coordinate()):
 * off-panel geometry is legal, the band renderer clips it, and a bounded
 * range keeps every segment's raster walk bounded. */
#define PROC_POINT_LOW (-480)
#define PROC_POINT_HIGH 720
#define PROC_SURFACES 2u

/* One malloc per batch: this header, then four planes (x, y, out_x, out_y)
 * of points_stride(count) elements each. It used to be four 128-element
 * arrays whatever the count (1,055 B with the alignment slack), which is what
 * put 32 plans of 40 points into OUT_OF_MEMORY next to a large guest
 * (docs/kasane/procedural-limits-device.md sec.3).
 *
 * The stride is the count rounded up to whole eight-lane blocks, so every
 * plane begins 16-byte aligned: the dispatcher sends an unaligned plane to the
 * scalar path, so misalignment would cost speed, never memory safety. The
 * round-up is not overrun room -- the PIE kernel loads and stores only
 * [0, count) (whole blocks by PIE, the tail scalar), which
 * tools/kasane_contract/test_proc_points_pie_host.c pins with a guard block. */
typedef struct {
    uint16_t count,color;
} proc_points;
static size_t points_stride(unsigned count){return ((size_t)count+7u)&~(size_t)7u;}
static size_t points_bytes(unsigned count){
    /* +15: malloc aligns to 4 or 8, and the planes need 16. */
    return sizeof(proc_points)+15u+4u*points_stride(count)*sizeof(int16_t);
}
static int16_t *points_plane(const proc_points *p,unsigned k){
    uintptr_t base=((uintptr_t)(p+1)+15u)&~(uintptr_t)15u;
    return (int16_t *)base+k*points_stride(p->count);
}
enum {PLANE_X,PLANE_Y,PLANE_OUT_X,PLANE_OUT_Y};
typedef struct {
    uint32_t handle;
    ksn_proc_plan *plan;
    proc_points *points;
} proc_slot;
static proc_slot slots[PROC_HANDLES];
static uint32_t next_handle;
static uint32_t scalar_batches,pie_batches;
typedef struct {
    ksn_proc_frame *candidate,*committed;
    uint16_t candidate_color,committed_color;
    bool pending,has_committed,repair_required,image_mode;
    uint32_t handle;
    ksn_rect damage;
} proc_surface;
static proc_surface surfaces[PROC_SURFACES];
static uint32_t next_surface_handle;
static unsigned building_surface,pending_surface;
static ksn_proc_frame *scratch;
static ksn_proc_vm *vm;
static bool building;
/* A source row can be requested repeatedly by scaled image draws. Cache the
 * whole eight-row raster band so adjacent rows share one segment traversal. */
#define PROC_IMAGE_BAND_ROWS 8u
static uint16_t image_band[KSN_PROC_W*PROC_IMAGE_BAND_ROWS],image_band_y;
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
static bool signed_integer(JSContext *ctx,JSValueConst value,int32_t low,int32_t high,int32_t *out){
    double n;
    if(!number(ctx,value,&n)||n<low||n>high||n!=floor(n))return false;
    *out=(int32_t)n;return true;
}
static bool array_length(JSContext *ctx,JSValueConst array,uint32_t *length){
    if(!JS_IsArray(array))return false;
    JSValue v=JS_GetPropertyStr(ctx,array,"length");
    if(JS_IsException(v))return false;
    bool ok=integer(ctx,v,UINT32_MAX,length);
    JS_FreeValue(ctx,v);return ok;
}
static bool entry(JSContext *ctx,JSValueConst row,ksn_proc_inst *out){
    uint32_t length,fields[4];double value;
    if(!array_length(ctx,row,&length)||length!=6)return false;
    for(unsigned j=0;j<4;j++){
        JSValue v=JS_GetPropertyUint32(ctx,row,j);
        bool ok=!JS_IsException(v)&&integer(ctx,v,UINT8_MAX,&fields[j]);
        JS_FreeValue(ctx,v);if(!ok)return false;
    }
    JSValue v=JS_GetPropertyUint32(ctx,row,4);
    bool ok=!JS_IsException(v)&&number(ctx,v,&value)&&
            isfinite((float)value);
    JS_FreeValue(ctx,v);if(!ok)return false;
    uint32_t color;
    v=JS_GetPropertyUint32(ctx,row,5);
    ok=!JS_IsException(v)&&integer(ctx,v,UINT16_MAX,&color);
    JS_FreeValue(ctx,v);if(!ok)return false;
    *out=(ksn_proc_inst){(uint8_t)fields[0],(uint8_t)fields[1],
                         (uint8_t)fields[2],(uint8_t)fields[3],
                         (float)value,(uint16_t)color};
    return true;
}
typedef enum {POINTS_OK,POINTS_INVALID,POINTS_NO_MEMORY} points_result;
/* Sizes the batch from its own length, so the allocation happens after the
 * descriptor's shape is known and before any element is read. */
static points_result read_points(JSContext *ctx,JSValueConst descriptor,
                                 proc_points **out,KsnProcAffineQ14 *coeff){
    if(!JS_IsObject(descriptor))return POINTS_INVALID;
    JSValue kind=JS_GetPropertyStr(ctx,descriptor,"kind");
    const char *name=JS_IsString(kind)?JS_ToCString(ctx,kind):NULL;
    bool valid=name&&strcmp(name,"affineQ14Points")==0;
    if(name)JS_FreeCString(ctx,name);
    JS_FreeValue(ctx,kind);
    if(!valid)return POINTS_INVALID;
    JSValue x=JS_GetPropertyStr(ctx,descriptor,"x");
    JSValue y=JS_GetPropertyStr(ctx,descriptor,"y");
    JSValue matrix=JS_GetPropertyStr(ctx,descriptor,"coeff");
    JSValue color=JS_GetPropertyStr(ctx,descriptor,"color");
    uint32_t nx=0,ny=0,nc=0,shade=0;
    valid=array_length(ctx,x,&nx)&&array_length(ctx,y,&ny)&&
          array_length(ctx,matrix,&nc)&&nx==ny&&nx>=2&&nx<=PROC_POINT_MAX&&
          nc==6&&integer(ctx,color,UINT16_MAX,&shade);
    int32_t terms[6]={0};
    proc_points *points=NULL;
    bool no_memory=false;
    if(valid){
        points=malloc(points_bytes(nx));
        no_memory=!points;
        valid=!no_memory;
    }
    if(valid){
        points->count=(uint16_t)nx;
        int16_t *px_plane=points_plane(points,PLANE_X);
        int16_t *py_plane=points_plane(points,PLANE_Y);
        for(uint32_t i=0;i<nx&&valid;i++){
            JSValue vx=JS_GetPropertyUint32(ctx,x,i);
            JSValue vy=JS_GetPropertyUint32(ctx,y,i);
            int32_t px=0,py=0;
            valid=!JS_IsException(vx)&&!JS_IsException(vy)&&
                  signed_integer(ctx,vx,INT16_MIN,INT16_MAX,&px)&&
                  signed_integer(ctx,vy,INT16_MIN,INT16_MAX,&py);
            JS_FreeValue(ctx,vx);JS_FreeValue(ctx,vy);
            px_plane[i]=(int16_t)px;py_plane[i]=(int16_t)py;
        }
        for(unsigned i=0;i<6&&valid;i++){
            JSValue value=JS_GetPropertyUint32(ctx,matrix,i);
            valid=!JS_IsException(value)&&signed_integer(ctx,value,
                    i<4?INT16_MIN:INT32_MIN,i<4?INT16_MAX:INT32_MAX,&terms[i]);
            JS_FreeValue(ctx,value);
        }
    }
    JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);
    JS_FreeValue(ctx,matrix);JS_FreeValue(ctx,color);
    if(!valid){free(points);return no_memory?POINTS_NO_MEMORY:POINTS_INVALID;}
    points->color=(uint16_t)shade;
    *coeff=(KsnProcAffineQ14){(int16_t)terms[0],(int16_t)terms[1],
                              (int16_t)terms[2],(int16_t)terms[3],
                              terms[4],terms[5]};
    *out=points;
    return POINTS_OK;
}
static bool buffers(proc_surface *surface){
    if(!surface->candidate)surface->candidate=calloc(1,sizeof *surface->candidate);
    if(!scratch)scratch=calloc(1,sizeof *scratch);
    if(!surface->committed)surface->committed=calloc(1,sizeof *surface->committed);
    if(!vm)vm=calloc(1,sizeof *vm);
    return surface->candidate&&scratch&&surface->committed&&vm;
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
    for(unsigned f=0;f<2;f++)for(unsigned i=0;i<frames[f]->count;i++){
        const ksn_proc_segment *seg=&frames[f]->segments[i];
        int ax=seg->x0<seg->x1?seg->x0:seg->x1;
        int bx=seg->x0>seg->x1?seg->x0:seg->x1;
        int ay=seg->y0<seg->y1?seg->y0:seg->y1;
        int by=seg->y0>seg->y1?seg->y0:seg->y1;
        if(ax<x0)x0=ax;
        if(ay<y0)y0=ay;
        if(bx+1>x1)x1=bx+1;
        if(by+1>y1)y1=by+1;
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
static JSValue register_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.register";
    uint32_t count;
    if((argc!=1&&argc!=2)||!array_length(ctx,argv[0],&count)||
       !count||count>KSN_PROC_CODE)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected 1..64 instructions");
    /* The limit is decided before any work or allocation: a full table must
     * answer LIMIT_EXCEEDED, and a low heap used to turn the 33rd plan into
     * OUT_OF_MEMORY because the plan and its points were allocated first.
     * The slot stays free while the arrays below are read: a getter that calls
     * back into procedural is refused by guarded(). */
    unsigned slot=0;
    while(slot<PROC_HANDLES&&slots[slot].plan)slot++;
    if(slot==PROC_HANDLES||next_handle==INT32_MAX)
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"procedural plan limit");
    ksn_proc_inst code[KSN_PROC_CODE];
    for(uint32_t i=0;i<count;i++){
        JSValue row=JS_GetPropertyUint32(ctx,argv[0],i);
        bool ok=!JS_IsException(row)&&entry(ctx,row,&code[i]);
        JS_FreeValue(ctx,row);
        if(!ok)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid instruction entry");
    }
    ksn_proc_plan *plan=calloc(1,sizeof *plan);
    if(!plan)return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"plan allocation failed");
    const ksn_proc_program program={code,(uint8_t)count};
#ifdef KASANE_MEGADEMO_TRACE
    int64_t prep_began=esp_timer_get_time();
    bool prepared=ksn_proc_plan_prepare(plan,&program);
    uint32_t prep_us=(uint32_t)(esp_timer_get_time()-prep_began);
    trace.prep_us+=prep_us;
    if(prep_us>trace.prep_max_us)trace.prep_max_us=prep_us;
    if(!prepared){
#else
    if(!ksn_proc_plan_prepare(plan,&program)){
#endif
        free(plan);
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid procedural program");
    }
    proc_points *points=NULL;
    if(argc==2&&!JS_IsUndefined(argv[1])&&!JS_IsNull(argv[1])){
        KsnProcAffineQ14 coeff;
        /* Cardputer ADV calibration: at n=8/16/40 the PIE path beat scalar
         * across 2048 runs, with exact output parity. The dispatcher still
         * falls back on hosts or an unavailable/unaligned backend. */
        const KsnProcPointsPolicy policy={true,8};
        points_result read=read_points(ctx,argv[1],&points,&coeff);
        if(read==POINTS_NO_MEMORY){free(plan);
            return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"point allocation failed");}
        if(read!=POINTS_OK||
           !ksn_proc_plan_register_points_affine(plan,&coeff,&policy)){
            free(points);free(plan);
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid typed point batch");
        }
    }
    uint32_t handle=++next_handle;
    slots[slot]=(proc_slot){handle,plan,points};
    return JS_NewInt32(ctx,(int32_t)handle);
}
static JSValue unregister_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.unregister";
    uint32_t handle;
    if(argc!=1||!integer(ctx,argv[0],INT32_MAX,&handle))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected one procedural handle");
    proc_slot *slot=find_slot(handle);
    if(!slot)return failure(ctx,op,POCKET_ERR_CLOSED,"stale procedural handle");
    /* Legal between beginFrame() and commit(): draw() copies the plan's
     * segments into the candidate and the VM runs from its own code copy, so
     * no frame, pending presentation or image band refers to a plan. */
    free(slot->plan);free(slot->points);
    *slot=(proc_slot){0};
    return JS_UNDEFINED;
}
static JSValue begin_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.beginFrame";
    uint32_t color;
    unsigned index=0;
    if((argc!=1&&argc!=2)||!integer(ctx,argv[0],UINT16_MAX,&color)||
       (argc==2&&!surface_index(ctx,argv[1],&index)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected RGB565 and optional live surface ID");
    if(any_pending()||any_repair())
        return failure(ctx,op,POCKET_ERR_BUSY,"previous frame awaits presentation or repair");
    proc_surface *surface=&surfaces[index];
    if(!buffers(surface))return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"frame allocation failed");
    memset(surface->candidate,0,sizeof *surface->candidate);
    surface->candidate_color=(uint16_t)color;
    building_surface=index;
    building=true;
    return JS_UNDEFINED;
}
static JSValue draw_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.draw";
    uint32_t handle,length;
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    ksn_proc_frame *candidate=surfaces[building_surface].candidate;
    if(argc!=2||!integer(ctx,argv[0],INT32_MAX,&handle)||
       !array_length(ctx,argv[1],&length)||length>KSN_PROC_INPUTS)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected handle and 0..8 inputs");
    proc_slot *slot=find_slot(handle);
    if(!slot)return failure(ctx,op,POCKET_ERR_CLOSED,"stale procedural handle");
    /* Short arrays are zero-padded, so a caller written for four inputs sees
     * exactly the frame it saw when four were required. */
    float input[KSN_PROC_INPUTS]={0};
    for(unsigned i=0;i<length;i++){
        JSValue v=JS_GetPropertyUint32(ctx,argv[1],i);double n;
        bool ok=!JS_IsException(v)&&number(ctx,v,&n)&&isfinite((float)n);
        JS_FreeValue(ctx,v);
        if(!ok)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"non-finite input");
        input[i]=(float)n;
    }
    ksn_proc_status status=ksn_proc_plan_begin(vm,slot->plan,input,scratch);
    if(status==KSN_PROC_RUNNING)status=ksn_proc_plan_run(vm,slot->plan,false);
    if(status!=KSN_PROC_DONE){
        building=false;
        return failure(ctx,op,status==KSN_PROC_LIMIT?POCKET_ERR_LIMIT_EXCEEDED:
                       POCKET_ERR_INVALID_ARGUMENT,"procedural execution failed");
    }
    unsigned typed_segments=0,typed_steps=0;
    KsnProcPointsDecision decision={0};
    if(slot->points){
        proc_points *p=slot->points;
        const KsnProcPointSrc src={points_plane(p,PLANE_X),points_plane(p,PLANE_Y)};
        const KsnProcPointDst dst={points_plane(p,PLANE_OUT_X),points_plane(p,PLANE_OUT_Y)};
        const int16_t *out_x=dst.x,*out_y=dst.y;
        if(!ksn_proc_plan_run_points_affine(slot->plan,dst,src,p->count,&decision)){
            building=false;
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"typed point run failed");
        }
        typed_segments=p->count-1;
        for(unsigned i=0;i<p->count;i++){
            if(out_x[i]<PROC_POINT_LOW||out_x[i]>PROC_POINT_HIGH||
               out_y[i]<PROC_POINT_LOW||out_y[i]>PROC_POINT_HIGH){
                building=false;
                return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                               "typed point outside -480..720");
            }
            if(i){
                int dx=abs(out_x[i]-out_x[i-1]);
                int dy=abs(out_y[i]-out_y[i-1]);
                typed_steps+=(unsigned)(dx>dy?dx:dy)+1u;
            }
        }
    }
    if((unsigned)scratch->count+typed_segments>(unsigned)KSN_PROC_SEGMENTS-candidate->count||
       (unsigned)scratch->raster_steps+typed_steps>(unsigned)UINT16_MAX-candidate->raster_steps){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"frame drawing limit");
    }
    memcpy(&candidate->segments[candidate->count],scratch->segments,
           scratch->count*sizeof scratch->segments[0]);
    candidate->count+=scratch->count;
    candidate->raster_steps+=scratch->raster_steps;
    if(slot->points){
        const proc_points *p=slot->points;
        const int16_t *out_x=points_plane(p,PLANE_OUT_X),*out_y=points_plane(p,PLANE_OUT_Y);
        for(unsigned i=1;i<p->count;i++)
            candidate->segments[candidate->count++]=(ksn_proc_segment){
                out_x[i-1],out_y[i-1],out_x[i],out_y[i],p->color};
        candidate->raster_steps+=(uint16_t)typed_steps;
        if(decision.backend==KSN_PROC_POINTS_PIE)pie_batches++;
        else scalar_batches++;
    }
#ifdef KASANE_BGCOST_TRACE
    bg_steps+=vm->steps;
    bg_last=(int)(slot-slots);
    bg_plan[bg_last].handle=handle;
    bg_plan[bg_last].n++;
    bg_plan[bg_last].steps+=vm->steps;
    bg_plan[bg_last].seg+=scratch->count+typed_segments;
#endif
    return JS_UNDEFINED;
}
static JSValue commit_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.procedural.commit";
    if(argc||!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    proc_surface *surface=&surfaces[building_surface];
    surface->damage=frame_damage(surface);
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
        return failure(ctx,"kasane.procedural",POCKET_ERR_BUSY,
                       "reentrant procedural call");
    js_call_active=true;
    JSValue result=method(ctx,self,argc,argv);
    js_call_active=false;
    return result;
}
static JSValue js_register(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
#ifdef KASANE_MEGADEMO_TRACE
    int64_t began=esp_timer_get_time();
    JSValue result=guarded(ctx,self,argc,argv,register_impl);
    trace.reg_us+=(uint32_t)(esp_timer_get_time()-began);trace.reg_n++;
    return result;
#else
    return guarded(ctx,self,argc,argv,register_impl);
#endif
}
static JSValue js_unregister(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
#ifdef KASANE_MEGADEMO_TRACE
    trace.unreg_n++;
#endif
    return guarded(ctx,self,argc,argv,unregister_impl);
}
static JSValue js_begin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,begin_impl);
}
static JSValue js_draw(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
#ifdef KASANE_MEGADEMO_TRACE
    int64_t began=esp_timer_get_time();
    JSValue result=guarded(ctx,self,argc,argv,draw_impl);
    uint32_t us=(uint32_t)(esp_timer_get_time()-began);
    trace.draw_us+=us;trace.draw_n++;
    if(us>trace.draw_max_us)trace.draw_max_us=us;
#ifdef KASANE_BGCOST_TRACE
    if(bg_last>=0)bg_plan[bg_last].us+=us;
    bg_last=-1;
#endif
    return result;
#else
    return guarded(ctx,self,argc,argv,draw_impl);
#endif
}
static JSValue js_commit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
#ifdef KASANE_BGCOST_TRACE
    int64_t began=esp_timer_get_time();
    JSValue result=guarded(ctx,self,argc,argv,commit_impl);
    bg_commit_us+=(uint32_t)(esp_timer_get_time()-began);
    return result;
#else
    return guarded(ctx,self,argc,argv,commit_impl);
#endif
}
static JSValue resource_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    unsigned index=0;
    if(argc>1||(argc==1&&!surface_index(ctx,argv[0],&index)))
        return failure(ctx,"kasane.procedural.resource",POCKET_ERR_INVALID_ARGUMENT,
                       "expected optional live surface ID");
    return index?pocket_kasane_proc_resource_at(ctx,index):pocket_kasane_proc_resource(ctx);
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,resource_impl);
}
static JSValue create_surface_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    if(argc)return failure(ctx,"kasane.procedural.createSurface",POCKET_ERR_INVALID_ARGUMENT,
                           "expected no arguments");
    if(surfaces[1].handle||next_surface_handle==INT32_MAX)
        return failure(ctx,"kasane.procedural.createSurface",POCKET_ERR_LIMIT_EXCEEDED,
                       "two surfaces maximum");
    surfaces[1].handle=++next_surface_handle;
    return JS_NewInt32(ctx,(int32_t)surfaces[1].handle);
}
static JSValue js_create_surface(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,create_surface_impl);
}
esp_err_t pocket_proc_install(JSContext *ctx,JSValueConst ns){
    JSValue procedural=JS_NewObject(ctx);
    if(JS_IsException(procedural))return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("register",1,js_register),
        JS_CFUNC_DEF("unregister",1,js_unregister),
        JS_CFUNC_DEF("beginFrame",1,js_begin),
        JS_CFUNC_DEF("draw",2,js_draw),
        JS_CFUNC_DEF("commit",0,js_commit),
        JS_CFUNC_DEF("resource",0,js_resource),
        JS_CFUNC_DEF("createSurface",0,js_create_surface),
    };
    if(JS_SetPropertyFunctionList(ctx,procedural,methods,
                                  (int)(sizeof methods/sizeof methods[0]))<0){
        JS_FreeValue(ctx,procedural);return ESP_ERR_NO_MEM;
    }
    return JS_SetPropertyStr(ctx,ns,"procedural",procedural)<0?ESP_ERR_NO_MEM:ESP_OK;
}
void pocket_proc_reset(void){
    for(unsigned i=0;i<PROC_HANDLES;i++){
        free(slots[i].plan);free(slots[i].points);slots[i]=(proc_slot){0};
    }
    for(unsigned i=0;i<PROC_SURFACES;i++){
        free(surfaces[i].candidate);free(surfaces[i].committed);
        surfaces[i]=(proc_surface){0};
    }
    free(scratch);free(vm);scratch=NULL;vm=NULL;
    image_band_valid=false;image_band_frame=NULL;
    building=js_call_active=false;building_surface=pending_surface=0;
    scalar_batches=pie_batches=0;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_band_count=image_band_cycles=image_span_count=image_span_cycles=0;
#endif
#ifdef KASANE_MEGADEMO_TRACE
    trace=(pocket_proc_trace){0};trace_split=false;
#endif
}
void pocket_proc_batch_counts(uint32_t *scalar,uint32_t *pie){
    if(scalar)*scalar=scalar_batches;
    if(pie)*pie=pie_batches;
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
    uint16_t band_y=(uint16_t)(y&~(PROC_IMAGE_BAND_ROWS-1u));
    if(!image_band_valid||image_band_surface!=index||
       image_band_y!=band_y||image_band_frame!=frame||
       image_band_color!=color){
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
#ifdef KASANE_BGCOST_TRACE
void pocket_proc_bgcost_take(pocket_proc_bgcost *out){
    pocket_proc_bgcost r={.steps=bg_steps,.commit_us=bg_commit_us,.view_us=bg_view_us};
    bg_steps=bg_commit_us=bg_view_us=0;
    const proc_surface *s=&surfaces[0];
    const ksn_proc_frame *f=s->pending?s->candidate:(s->has_committed?s->committed:NULL);
    if(f){
        r.seg=f->count;r.ras=f->raster_steps;
        for(unsigned i=0;i<f->count;i++){
            const ksn_proc_segment *g=&f->segments[i];
            int y0=g->y0<g->y1?g->y0:g->y1,y1=g->y0>g->y1?g->y0:g->y1;
            int dx=abs(g->x1-g->x0),dy=y1-y0,n=(dx>dy?dx:dy)+1;
            int a=y0<0?0:y0,b=y1>KSN_PROC_H-1?KSN_PROC_H-1:y1;
            if(a>b)continue;
            /* Bands (PROC_IMAGE_BAND_ROWS) the segment is walked in, and its
             * Bresenham iterations inside the panel's rows: the renderer
             * walks off-panel columns too, but never off-panel rows. */
            r.hits+=(unsigned)(b/PROC_IMAGE_BAND_ROWS-a/PROC_IMAGE_BAND_ROWS+1);
            r.walk+=(uint32_t)((int64_t)n*(b-a+1)/(dy+1));
        }
    }
    if(out)*out=r;
}
void pocket_proc_bgcost_plans_emit(void){
    char line[400];
    int at=snprintf(line,sizeof line,"BGP");
    for(unsigned i=0;i<32&&at<(int)sizeof line-48;i++){
        if(!bg_plan[i].n)continue;
        at+=snprintf(line+at,sizeof line-at," %u=%u/%u/%u/%u",(unsigned)bg_plan[i].handle,
                     (unsigned)bg_plan[i].n,(unsigned)bg_plan[i].us,
                     (unsigned)bg_plan[i].steps,(unsigned)bg_plan[i].seg);
    }
    memset(bg_plan,0,sizeof bg_plan);
    ESP_LOGI("app","%s",line);
}
#endif
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
