#include "pocket_proc.h"
#ifdef KSN_PROC_HOST_TEST
ksn_result pocket_kasane_proc_publish(void);
void pocket_kasane_invalidate(void);
JSValue pocket_kasane_proc_resource(JSContext *ctx);
#else
#include "pocket_kasane.h"
#endif
#include "pocket_api.h"
#include "ui/kasane/ksn_proc_plan.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PROC_HANDLES 16u
#define PROC_POINT_MAX 64u

typedef struct {
    uint16_t count,color;
    _Alignas(16) int16_t x[PROC_POINT_MAX],y[PROC_POINT_MAX];
    _Alignas(16) int16_t out_x[PROC_POINT_MAX],out_y[PROC_POINT_MAX];
} proc_points;
typedef struct {
    uint32_t handle;
    ksn_proc_plan *plan;
    void *point_allocation;
    proc_points *points;
} proc_slot;
static proc_slot slots[PROC_HANDLES];
static uint32_t next_handle;
static uint32_t scalar_batches,pie_batches;
static ksn_proc_frame *candidate,*scratch,*committed;
static ksn_proc_vm *vm;
static uint16_t candidate_color,committed_color;
static bool building,pending,has_committed,repair_required,image_mode;
/* The compositor usually requests multiple short spans from one source row. */
static uint16_t image_row[KSN_PROC_W],image_row_y;
static const ksn_proc_frame *image_row_frame;
static uint16_t image_row_color;
static bool image_row_valid;
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
static bool read_points(JSContext *ctx,JSValueConst descriptor,
                        proc_points *points,KsnProcAffineQ14 *coeff){
    if(!JS_IsObject(descriptor))return false;
    JSValue kind=JS_GetPropertyStr(ctx,descriptor,"kind");
    const char *name=JS_IsString(kind)?JS_ToCString(ctx,kind):NULL;
    bool valid=name&&strcmp(name,"affineQ14Points")==0;
    if(name)JS_FreeCString(ctx,name);
    JS_FreeValue(ctx,kind);
    if(!valid)return false;
    JSValue x=JS_GetPropertyStr(ctx,descriptor,"x");
    JSValue y=JS_GetPropertyStr(ctx,descriptor,"y");
    JSValue matrix=JS_GetPropertyStr(ctx,descriptor,"coeff");
    JSValue color=JS_GetPropertyStr(ctx,descriptor,"color");
    uint32_t nx=0,ny=0,nc=0,shade=0;
    valid=array_length(ctx,x,&nx)&&array_length(ctx,y,&ny)&&
          array_length(ctx,matrix,&nc)&&nx==ny&&nx>=2&&nx<=PROC_POINT_MAX&&
          nc==6&&integer(ctx,color,UINT16_MAX,&shade);
    int32_t terms[6]={0};
    if(valid){
        for(uint32_t i=0;i<nx&&valid;i++){
            JSValue vx=JS_GetPropertyUint32(ctx,x,i);
            JSValue vy=JS_GetPropertyUint32(ctx,y,i);
            int32_t px=0,py=0;
            valid=!JS_IsException(vx)&&!JS_IsException(vy)&&
                  signed_integer(ctx,vx,INT16_MIN,INT16_MAX,&px)&&
                  signed_integer(ctx,vy,INT16_MIN,INT16_MAX,&py);
            JS_FreeValue(ctx,vx);JS_FreeValue(ctx,vy);
            points->x[i]=(int16_t)px;points->y[i]=(int16_t)py;
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
    if(!valid)return false;
    points->count=(uint16_t)nx;points->color=(uint16_t)shade;
    *coeff=(KsnProcAffineQ14){(int16_t)terms[0],(int16_t)terms[1],
                              (int16_t)terms[2],(int16_t)terms[3],
                              terms[4],terms[5]};
    return true;
}
static bool buffers(void){
    if(!candidate)candidate=calloc(1,sizeof *candidate);
    if(!scratch)scratch=calloc(1,sizeof *scratch);
    if(!committed)committed=calloc(1,sizeof *committed);
    if(!vm)vm=calloc(1,sizeof *vm);
    return candidate&&scratch&&committed&&vm;
}
static JSValue register_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.register";
    uint32_t count;
    if((argc!=1&&argc!=2)||!array_length(ctx,argv[0],&count)||
       !count||count>KSN_PROC_CODE)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected 1..64 instructions");
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
    if(!ksn_proc_plan_prepare(plan,&program)){
        free(plan);
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid procedural program");
    }
    void *point_allocation=NULL;
    proc_points *points=NULL;
    if(argc==2&&!JS_IsUndefined(argv[1])&&!JS_IsNull(argv[1])){
        point_allocation=malloc(sizeof(proc_points)+15);
        if(!point_allocation){free(plan);
            return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"point allocation failed");}
        points=(proc_points *)(((uintptr_t)point_allocation+15u)&~(uintptr_t)15u);
        memset(points,0,sizeof *points);
        KsnProcAffineQ14 coeff;
        /* Cardputer ADV calibration: at n=8/16/40 the PIE path beat scalar
         * across 2048 runs, with exact output parity. The dispatcher still
         * falls back on hosts or an unavailable/unaligned backend. */
        const KsnProcPointsPolicy policy={true,8};
        if(!read_points(ctx,argv[1],points,&coeff)||
           !ksn_proc_plan_register_points_affine(plan,&coeff,&policy)){
            free(point_allocation);free(plan);
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid typed point batch");
        }
    }
    unsigned slot=0;
    while(slot<PROC_HANDLES&&slots[slot].plan)slot++;
    if(slot==PROC_HANDLES||next_handle==INT32_MAX){
        free(point_allocation);
        free(plan);
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"procedural plan limit");
    }
    uint32_t handle=++next_handle;
    slots[slot]=(proc_slot){handle,plan,point_allocation,points};
    return JS_NewInt32(ctx,(int32_t)handle);
}
static JSValue begin_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.beginFrame";
    uint32_t color;
    if(argc!=1||!integer(ctx,argv[0],UINT16_MAX,&color))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected RGB565 backdrop");
    if(pending)return failure(ctx,op,POCKET_ERR_BUSY,"previous frame awaits presentation");
    if(!buffers())return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"frame allocation failed");
    memset(candidate,0,sizeof *candidate);
    candidate_color=(uint16_t)color;
    building=true;
    return JS_UNDEFINED;
}
static JSValue draw_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.procedural.draw";
    uint32_t handle,length;
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    if(argc!=2||!integer(ctx,argv[0],INT32_MAX,&handle)||
       !array_length(ctx,argv[1],&length)||length!=KSN_PROC_INPUTS)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected handle and four inputs");
    proc_slot *slot=NULL;
    for(unsigned i=0;i<PROC_HANDLES;i++)if(slots[i].handle==handle){slot=&slots[i];break;}
    if(!slot)return failure(ctx,op,POCKET_ERR_CLOSED,"stale procedural handle");
    float input[KSN_PROC_INPUTS];
    for(unsigned i=0;i<KSN_PROC_INPUTS;i++){
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
        const KsnProcPointSrc src={p->x,p->y};
        const KsnProcPointDst dst={p->out_x,p->out_y};
        if(!ksn_proc_plan_run_points_affine(slot->plan,dst,src,p->count,&decision)){
            building=false;
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"typed point run failed");
        }
        typed_segments=p->count-1;
        for(unsigned i=0;i<p->count;i++){
            if(p->out_x[i]<0||p->out_x[i]>=KSN_PROC_W||
               p->out_y[i]<0||p->out_y[i]>=KSN_PROC_H){
                building=false;
                return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"typed point off panel");
            }
            if(i){
                int dx=abs(p->out_x[i]-p->out_x[i-1]);
                int dy=abs(p->out_y[i]-p->out_y[i-1]);
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
        for(unsigned i=1;i<p->count;i++)
            candidate->segments[candidate->count++]=(ksn_proc_segment){
                p->out_x[i-1],p->out_y[i-1],p->out_x[i],p->out_y[i],p->color};
        candidate->raster_steps+=(uint16_t)typed_steps;
        if(decision.backend==KSN_PROC_POINTS_PIE)pie_batches++;
        else scalar_batches++;
    }
    return JS_UNDEFINED;
}
static JSValue commit_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.procedural.commit";
    if(argc||!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    ksn_result result=pocket_kasane_proc_publish();
    if(result!=KSN_OK)return failure(ctx,op,result==KSN_OOM?POCKET_ERR_OUT_OF_MEMORY:
                                   POCKET_ERR_BUSY,"Kasane presentation unavailable");
    candidate->ready=true;
    pending=true;
    image_row_valid=false;
    building=false;
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
    return guarded(ctx,self,argc,argv,register_impl);
}
static JSValue js_begin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,begin_impl);
}
static JSValue js_draw(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,draw_impl);
}
static JSValue js_commit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,commit_impl);
}
static JSValue resource_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    if(argc)return failure(ctx,"kasane.procedural.resource",POCKET_ERR_INVALID_ARGUMENT,
                           "expected no arguments");
    return pocket_kasane_proc_resource(ctx);
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,resource_impl);
}
esp_err_t pocket_proc_install(JSContext *ctx,JSValueConst ns){
    JSValue procedural=JS_NewObject(ctx);
    if(JS_IsException(procedural))return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("register",1,js_register),
        JS_CFUNC_DEF("beginFrame",1,js_begin),
        JS_CFUNC_DEF("draw",2,js_draw),
        JS_CFUNC_DEF("commit",0,js_commit),
        JS_CFUNC_DEF("resource",0,js_resource),
    };
    if(JS_SetPropertyFunctionList(ctx,procedural,methods,5)<0){
        JS_FreeValue(ctx,procedural);return ESP_ERR_NO_MEM;
    }
    return JS_SetPropertyStr(ctx,ns,"procedural",procedural)<0?ESP_ERR_NO_MEM:ESP_OK;
}
void pocket_proc_reset(void){
    for(unsigned i=0;i<PROC_HANDLES;i++){
        free(slots[i].plan);free(slots[i].point_allocation);slots[i]=(proc_slot){0};
    }
    free(candidate);free(scratch);free(committed);free(vm);
    candidate=scratch=committed=NULL;vm=NULL;
    image_row_valid=false;image_row_frame=NULL;
    building=pending=has_committed=repair_required=image_mode=js_call_active=false;
    candidate_color=committed_color=0;
    scalar_batches=pie_batches=0;
}
void pocket_proc_batch_counts(uint32_t *scalar,uint32_t *pie){
    if(scalar)*scalar=scalar_batches;
    if(pie)*pie=pie_batches;
}
void pocket_proc_end_turn(void){
    /* A thrown JS frame must not carry an unfinished candidate into the next
     * owner turn. Previously committed or pending presentation stays intact. */
    building=false;
}
bool pocket_proc_has_frame(void){return pending||has_committed||repair_required;}
bool pocket_proc_pending(void){return pending;}
bool pocket_proc_is_image_mode(void){return image_mode;}
void pocket_proc_image_mode(void){image_mode=true;}
static ksn_result image_span(void *ctx,uint16_t variant,uint16_t frame_number,
                             uint16_t y,uint16_t x,uint16_t count,
                             uint16_t *rgb565,uint8_t *alpha){
    (void)ctx;
    if(variant||frame_number||y>=KSN_PROC_H||x>KSN_PROC_W||
       count>KSN_PROC_W-x||(count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
    const ksn_proc_frame *frame=pending?candidate:(has_committed?committed:NULL);
    uint16_t color=pending?candidate_color:(has_committed?committed_color:0);
    if(!image_row_valid||image_row_y!=y||image_row_frame!=frame||
       image_row_color!=color){
        for(unsigned i=0;i<KSN_PROC_W;i++)image_row[i]=color;
        if(frame&&!ksn_proc_render_band(frame,image_row,y,1))return KSN_INVALID;
        image_row_y=y;image_row_frame=frame;image_row_color=color;
        image_row_valid=true;
    }
    memcpy(rgb565,image_row+x,count*sizeof *rgb565);
    memset(alpha,255,count);
    return KSN_OK;
}
void pocket_proc_image_port(ksn_image_port *out){
    if(out)*out=(ksn_image_port){NULL,KSN_PROC_W,KSN_PROC_H,1,1,image_span};
}
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels){
    (void)ctx;
    if(!pixels||y>KSN_PROC_H||rows>KSN_PROC_H-y)return KSN_INVALID;
    const ksn_proc_frame *frame=pending?candidate:(has_committed?committed:NULL);
    uint16_t color=pending?candidate_color:(has_committed?committed_color:0);
    for(unsigned i=0;i<(unsigned)rows*KSN_PROC_W;i++)pixels[i]=color;
    return !frame||ksn_proc_render_band(frame,pixels,y,rows)?KSN_OK:KSN_INVALID;
}
void pocket_proc_present_result(ksn_result result){
    if(!pending&&!repair_required)return;
    if(result==KSN_OK){
        if(pending){
            ksn_proc_frame *old=committed;committed=candidate;candidate=old;
            committed_color=candidate_color;
            has_committed=true;pending=false;
            image_row_valid=false;
        }
        repair_required=false;
    }else if(result==KSN_IO){
        image_row_valid=false;
        if(image_mode){
            /* Kasane retries a submitted UI patch against the same image
             * resource. Keep its pixels paired with that ticket until ACK. */
            repair_required=true;
        }else{
            pending=false;
            /* A backdrop has no submitted image node: repaint the old frame. */
            repair_required=true;
        }
        pocket_kasane_invalidate();
    }
}
