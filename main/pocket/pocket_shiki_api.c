// pocket.kasane.shiki (docs/kasane/shiki-spec.md sec.9): registration of
// Shiki programs, data regions and their windows, the Shiki heap's report.
// Drawing shares the procedural surface's slots, handles and frames
// (pocket_proc.c): beginFrame, commit, resource and createSurface are the
// procedural functions themselves, and draw runs the Shiki engine into the
// same frame. grid is the existing pocket.kasane.grid (sec.7).
//
// Data regions (sec.5.1, 9.2, 9.3; v1.4 field planes). data() takes the
// memory from the Shiki heap, 16-byte aligned, and hands JS a typed array over
// an ArrayBuffer of that external memory: the whole region, k_rec planes of
// S = R rounded up to 8, padding included. fields() makes per-field views of
// length R on request only (a guest object each; DERBY's heap is tight). Shiki holds the window (the typed array) and its
// ArrayBuffer strongly, so JS dropping the window frees nothing and calls no
// free_func: a region lives until release() or the end of the session.
// Identity is by the window object (a second view of the same buffer is not
// the region). The bookkeeping -- the windows, the handles bound to each --
// is this file's .bss, outside the heap. free_func does nothing: Shiki frees
// the memory itself, after detaching, and the finalizer may call free_func
// again with NULL (sec.9.2).
#include "pocket_shiki_api.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include "shiki_heap.h"
#include "ui/kasane/ksn_shiki.h"
#include <math.h>
#include <string.h>

#define REGIONS 16          // maxSessionDataRegions (sec.9.1)
#define BINDS 32            // one per slot: maxHandles

typedef struct {
    JSValue window,buffer;   // both held: the window for identity, the buffer to detach
    void *mem;
    uint32_t records;        // R; the memory is k_rec planes of S = R rounded up to 8
    uint8_t type,q,k_rec;
    uint8_t live;
    uint16_t binds;          // handles bound to it
} region_t;
typedef struct { uint32_t handle; uint8_t n; uint8_t region[KSN_SHIKI_REGIONS]; } bind_t;

static region_t regions[REGIONS];
static bind_t binds[BINDS];
static JSContext *owner;     // the guest the windows belong to (for the reset)

static void free_nothing(JSRuntime *rt,void *opaque,void *ptr){(void)rt;(void)opaque;(void)ptr;}

static JSValue failure(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst v,double *out){
    return JS_IsNumber(v)&&JS_ToFloat64(ctx,out,v)==0;
}
static bool whole(JSContext *ctx,JSValueConst v,double lo,double hi,uint32_t *out){
    double n;
    if(!number(ctx,v,&n)||!isfinite(n)||n!=floor(n)||n<lo||n>hi)return false;
    *out=(uint32_t)n;return true;
}
static bool prop_whole(JSContext *ctx,JSValueConst o,const char *name,double lo,double hi,uint32_t *out){
    JSValue v=JS_GetPropertyStr(ctx,o,name);
    const bool ok=!JS_IsException(v)&&whole(ctx,v,lo,hi,out);
    JS_FreeValue(ctx,v);
    return ok;
}
static bool length_of(JSContext *ctx,JSValueConst array,uint32_t max,uint32_t *n){
    return JS_IsArray(array)&&prop_whole(ctx,array,"length",0,max,n);
}

// The live region whose window is v, or -1. *closed: v is a typed array over
// a detached buffer -- a released window (or someone's transferred one).
static int region_of(JSContext *ctx,JSValueConst v,bool *closed){
    *closed=false;
    for(int k=0;k<REGIONS;k++)if(regions[k].live&&JS_IsStrictEqual(ctx,v,regions[k].window))return k;
    if(JS_GetTypedArrayType(v)>=0){
        size_t off=0,len=0,bpe=0,size=0;
        JSValue buf=JS_GetTypedArrayBuffer(ctx,v,&off,&len,&bpe);
        // A detached typed array is out of bounds: the call throws.
        if(JS_IsException(buf)){JS_FreeValue(ctx,JS_GetException(ctx));*closed=true;return -1;}
        if(!JS_GetArrayBuffer(ctx,&size,buf)){JS_FreeValue(ctx,JS_GetException(ctx));*closed=true;}
        JS_FreeValue(ctx,buf);
    }
    return -1;
}

// ------------------------------------------------------------- data()
static const char *const TYPE_NAME[]={"i16","u16","i32"};
static JSValue js_data(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.data";
    if(argc<1||!JS_IsObject(argv[0]))return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected {type, q, records, record}");
    // v1.4: the v1.3 shape (length, elements in record order) is refused,
    // whatever its value, so old code does not run on the new layout.
    JSAtom len_atom=JS_NewAtom(ctx,"length");
    const int has_length=JS_HasProperty(ctx,argv[0],len_atom);
    JS_FreeAtom(ctx,len_atom);
    if(has_length<0)return JS_EXCEPTION;
    if(has_length)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"length is v1.3's shape: pass records (the record count)");
    JSValue t=JS_GetPropertyStr(ctx,argv[0],"type");
    const char *s=JS_IsString(t)?JS_ToCString(ctx,t):NULL;
    int type=-1;
    for(int k=0;s&&k<3;k++)if(!strcmp(s,TYPE_NAME[k]))type=k;
    if(s)JS_FreeCString(ctx,s);
    JS_FreeValue(ctx,t);
    uint32_t q=0,records=0,record=0;
    // sec.9.3: an unknown type, q outside 0..30, record outside 1..8, records
    // not an integer in 1..65,535 (not rounded).
    if(type<0||!prop_whole(ctx,argv[0],"q",0,30,&q)||!prop_whole(ctx,argv[0],"record",1,8,&record)||
       !prop_whole(ctx,argv[0],"records",1,KSN_SHIKI_RECORDS,&records))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected type i16/u16/i32, q 0..30, record 1..8, records 1..65,535");
    int slot=-1;
    for(int k=0;k<REGIONS&&slot<0;k++)if(!regions[k].live)slot=k;
    if(slot<0)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"16 data regions in a session");
    const size_t elements=ksn_shiki_region_elements(records,record);
    const size_t bytes=elements*(type==2?4u:2u);
    void *mem=shiki_heap_aligned(bytes);
    if(!mem)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"the Shiki heap's budget");
    memset(mem,0,bytes);
    JSValue buffer=JS_NewArrayBuffer(ctx,mem,bytes,free_nothing,NULL,false);
    if(JS_IsException(buffer)){shiki_heap_free(mem);return buffer;}
    static const JSTypedArrayEnum KIND[]={JS_TYPED_ARRAY_INT16,JS_TYPED_ARRAY_UINT16,JS_TYPED_ARRAY_INT32};
    JSValueConst targs[3]={buffer,JS_NewInt32(ctx,0),JS_NewInt32(ctx,(int32_t)elements)};
    JSValue window=JS_NewTypedArray(ctx,3,targs,KIND[type]);
    if(JS_IsException(window)){
        JS_DetachArrayBuffer(ctx,buffer);JS_FreeValue(ctx,buffer);shiki_heap_free(mem);
        return window;
    }
    owner=ctx;
    regions[slot]=(region_t){JS_DupValue(ctx,window),buffer,mem,records,(uint8_t)type,(uint8_t)q,(uint8_t)record,1,0};
    return window;
}

// fields(window): k_rec fresh views, field c = window[c*S .. c*S+R). Not
// regions themselves: register()/release() still want the window.
static JSValue js_fields(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.fields";
    bool closed=false;
    const int k=argc>=1?region_of(ctx,argv[0],&closed):-1;
    if(k<0)return failure(ctx,op,closed?POCKET_ERR_CLOSED:POCKET_ERR_INVALID_ARGUMENT,
                          closed?"released window":"not a window shiki.data() returned");
    const region_t *g=&regions[k];
    static const JSTypedArrayEnum KIND[]={JS_TYPED_ARRAY_INT16,JS_TYPED_ARRAY_UINT16,JS_TYPED_ARRAY_INT32};
    const uint32_t S=ksn_shiki_stride(g->records),size=g->type==2?4u:2u;
    JSValue out=JS_NewArray(ctx);
    if(JS_IsException(out))return out;
    for(uint32_t c=0;c<g->k_rec;c++){
        JSValueConst targs[3]={g->buffer,JS_NewInt64(ctx,(int64_t)c*S*size),JS_NewInt32(ctx,(int32_t)g->records)};
        JSValue view=JS_NewTypedArray(ctx,3,targs,KIND[g->type]);
        if(JS_IsException(view)){JS_FreeValue(ctx,out);return view;}
        if(JS_SetPropertyUint32(ctx,out,c,view)<0){JS_FreeValue(ctx,out);return JS_EXCEPTION;}
    }
    return out;
}

// Detach, drop Shiki's references, give the memory back (sec.9.2's order).
static void drop_region(JSContext *ctx,region_t *g){
    JS_DetachArrayBuffer(ctx,g->buffer);
    JS_FreeValue(ctx,g->window);JS_FreeValue(ctx,g->buffer);
    shiki_heap_free(g->mem);
    *g=(region_t){0};
}
static JSValue js_release(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.release";
    bool closed=false;
    const int k=argc>=1?region_of(ctx,argv[0],&closed):-1;
    if(k<0)return failure(ctx,op,closed?POCKET_ERR_CLOSED:POCKET_ERR_INVALID_ARGUMENT,
                          closed?"released window":"not a window shiki.data() returned");
    if(regions[k].binds)return failure(ctx,op,POCKET_ERR_BUSY,"a registered program still uses the region");
    drop_region(ctx,&regions[k]);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------ register()
// Built-in programs by name (sec.9.4): none yet. DERBY's move to Shiki
// (sec.11 step 3) fills this table.
typedef struct { const char *name; const uint8_t *code; uint16_t len; } builtin_t;
static const builtin_t BUILTIN[1]={{NULL,NULL,0}};

static JSValue js_register(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.register";
    if(argc<1)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a program (Uint8Array or a built-in name)");
    const uint8_t *code=NULL;
    size_t len=0;
    if(JS_IsString(argv[0])){
        const char *name=JS_ToCString(ctx,argv[0]);
        for(unsigned k=0;name&&k<sizeof BUILTIN/sizeof BUILTIN[0];k++)
            if(BUILTIN[k].name&&!strcmp(BUILTIN[k].name,name)){code=BUILTIN[k].code;len=BUILTIN[k].len;}
        if(name)JS_FreeCString(ctx,name);
        if(!code)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"no built-in program of that name");
    }else{
        code=JS_GetUint8Array(ctx,&len,argv[0]);
        if(!code){JS_FreeValue(ctx,JS_GetException(ctx));
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a program (Uint8Array or a built-in name)");}
    }
    // Arguments: up to 8 numbers. Finiteness is the engine's check (sec.2.4).
    double args[KSN_SHIKI_ARGS];
    uint32_t na=0;
    if(argc>=2&&!JS_IsUndefined(argv[1])){
        if(!length_of(ctx,argv[1],KSN_SHIKI_ARGS,&na))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected up to 8 numeric arguments");
        for(uint32_t k=0;k<na;k++){
            JSValue v=JS_GetPropertyUint32(ctx,argv[1],k);
            const bool ok=number(ctx,v,&args[k]);
            JS_FreeValue(ctx,v);
            if(!ok)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"arguments must be numbers");
        }
    }
    // options.data: the windows, in region-number order (sec.5.1).
    ksn_shiki_region bound[KSN_SHIKI_REGIONS];
    uint8_t which[KSN_SHIKI_REGIONS];
    uint32_t nr=0;
    if(argc>=3&&!JS_IsUndefined(argv[2])){
        if(!JS_IsObject(argv[2]))return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected options {data}");
        JSValue data=JS_GetPropertyStr(ctx,argv[2],"data");
        bool ok=!JS_IsException(data);
        if(ok&&!JS_IsUndefined(data)){
            ok=length_of(ctx,data,KSN_SHIKI_REGIONS,&nr);
            for(uint32_t k=0;ok&&k<nr;k++){
                JSValue w=JS_GetPropertyUint32(ctx,data,k);
                bool closed=false;
                const int g=region_of(ctx,w,&closed);
                JS_FreeValue(ctx,w);
                if(g<0){JS_FreeValue(ctx,data);
                    return failure(ctx,op,closed?POCKET_ERR_CLOSED:POCKET_ERR_INVALID_ARGUMENT,
                                   closed?"a released window":"options.data: not a window shiki.data() returned");}
                const region_t *r=&regions[g];
                bound[k]=(ksn_shiki_region){r->mem,r->records,r->type,r->q,r->k_rec};
                which[k]=(uint8_t)g;
            }
        }
        JS_FreeValue(ctx,data);
        if(!ok)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"options.data: an array of up to 4 windows");
    }
    int b=-1;
    for(int k=0;k<BINDS&&b<0;k++)if(!binds[k].handle)b=k;
    const size_t bytes=ksn_shiki_plan_bytes_for(code,len);
    if(!bytes)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"not a Shiki program");
    if(b<0)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"32 handles");
    // From the Shiki heap: the budget decides here, at once (sec.9.4).
    ksn_shiki_plan *plan=shiki_or_sys_malloc(bytes);
    if(!plan)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"the Shiki heap's budget");
    if(!ksn_shiki_register(code,len,args,na,bound,nr,plan)){
        shiki_or_sys_free(plan);
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"program, arguments or regions refused (sec.6.1)");
    }
    const uint32_t handle=pocket_proc_shiki_attach(plan);
    if(!handle){shiki_or_sys_free(plan);return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"32 handles");}
    // A window passed twice counts once for this handle (sec.9.3).
    binds[b]=(bind_t){handle,0,{0}};
    for(uint32_t k=0;k<nr;k++){
        bool seen=false;
        for(unsigned j=0;j<binds[b].n;j++)seen|=binds[b].region[j]==which[k];
        if(seen)continue;
        binds[b].region[binds[b].n++]=which[k];
        regions[which[k]].binds++;
    }
    return JS_NewInt32(ctx,(int32_t)handle);
}
static JSValue js_unregister(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.unregister";
    uint32_t handle=0;
    if(argc!=1||!whole(ctx,argv[0],0,INT32_MAX,&handle))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected one Shiki handle");
    int b=-1;
    for(int k=0;k<BINDS&&b<0;k++)if(handle&&binds[k].handle==handle)b=k;
    if(b<0||!pocket_proc_shiki_detach(handle))return failure(ctx,op,POCKET_ERR_CLOSED,"stale Shiki handle");
    for(unsigned j=0;j<binds[b].n;j++)regions[binds[b].region[j]].binds--;
    binds[b]=(bind_t){0};
    return JS_UNDEFINED;
}

// --------------------------------------------------------------- draw()
// draw(handle, inputs[]) or draw(handle, a0, ..., a7), as the procedural
// draw. The numbers go to the engine as doubles: sec.2.4 converts them.
static JSValue js_draw(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.draw";
    const uint32_t t0=pocket_shiki_prof.on?pocket_cycles():0;
    uint32_t handle=0,n=0;
    while(argc>2&&JS_IsUndefined(argv[argc-1]))argc--;
    const bool numeric=argc>=2&&JS_IsNumber(argv[1]);
    if(argc<1||!whole(ctx,argv[0],0,INT32_MAX,&handle)||
       (numeric?(n=(uint32_t)argc-1u)>KSN_SHIKI_INPUTS:
                (argc==2&&!length_of(ctx,argv[1],KSN_SHIKI_INPUTS,&n))||argc>2))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected handle and 0..8 inputs");
    double in[KSN_SHIKI_INPUTS]={0};
    for(uint32_t k=0;k<n;k++){
        /* Inputs passed as arguments are read off their tag: an int or a
         * double is the number itself, with no reference to take and drop
         * (about 180 cycles an input otherwise, shiki-bundle.md sec.3). */
        if(numeric){
            const int tag=JS_VALUE_GET_TAG(argv[1+k]);
            if(tag==JS_TAG_INT){in[k]=JS_VALUE_GET_INT(argv[1+k]);continue;}
            if(JS_TAG_IS_FLOAT64(tag)){in[k]=JS_VALUE_GET_FLOAT64(argv[1+k]);continue;}
        }
        JSValue v=numeric?JS_DupValue(ctx,argv[1+k]):JS_GetPropertyUint32(ctx,argv[1],k);
        const bool ok=number(ctx,v,&in[k]);
        JS_FreeValue(ctx,v);
        if(!ok)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"inputs must be numbers");
    }
    if(pocket_shiki_prof.on){const uint32_t t=pocket_cycles();pocket_shiki_prof.parse+=t-t0;pocket_shiki_prof.mark=t;}
    return pocket_proc_shiki_draw(ctx,handle,in,n);
}

// --------------------------------------------------------------- heap()
static JSValue js_heap(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    uint32_t bytes,used,largest;
    shiki_heap_info(&bytes,&used,&largest);
    JSValue o=JS_NewObject(ctx);
    if(JS_IsException(o))return o;
    JS_SetPropertyStr(ctx,o,"bytes",JS_NewUint32(ctx,bytes));
    JS_SetPropertyStr(ctx,o,"used",JS_NewUint32(ctx,used));
    JS_SetPropertyStr(ctx,o,"largest",JS_NewUint32(ctx,largest));
    return o;
}

// ------------------------------------------------------------- install
// The procedural namespace's own functions, shared (they act on the same
// frames): copied from kasane.procedural, which pocket_proc_install made.
static bool share(JSContext *ctx,JSValueConst from,JSValueConst to,const char *name){
    JSValue f=JS_GetPropertyStr(ctx,from,name);
    if(JS_IsException(f))return false;
    return JS_SetPropertyStr(ctx,to,name,f)>=0;
}
esp_err_t pocket_shiki_api_install(JSContext *ctx,JSValueConst kasane){
    JSValue shiki=JS_NewObject(ctx);
    if(JS_IsException(shiki))return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("register",3,js_register),
        JS_CFUNC_DEF("unregister",1,js_unregister),
        JS_CFUNC_DEF("draw",2,js_draw),
        JS_CFUNC_DEF("heap",0,js_heap),
        JS_CFUNC_DEF("data",1,js_data),
        JS_CFUNC_DEF("release",1,js_release),
        JS_CFUNC_DEF("fields",1,js_fields),
    };
    bool ok=JS_SetPropertyFunctionList(ctx,shiki,methods,(int)(sizeof methods/sizeof methods[0]))>=0;
    JSValue procedural=JS_GetPropertyStr(ctx,kasane,"procedural");
    ok=ok&&!JS_IsException(procedural)&&share(ctx,procedural,shiki,"beginFrame")&&share(ctx,procedural,shiki,"commit")&&
       share(ctx,procedural,shiki,"resource")&&share(ctx,procedural,shiki,"createSurface");
    JS_FreeValue(ctx,procedural);
    ok=ok&&share(ctx,kasane,shiki,"grid");
    if(!ok){JS_FreeValue(ctx,shiki);return ESP_ERR_NO_MEM;}
    owner=ctx;
    return JS_SetPropertyStr(ctx,kasane,"shiki",shiki)<0?ESP_ERR_NO_MEM:ESP_OK;
}

// The capability (sec.9.1). supported: this firmware has it; available: the
// running session took a Shiki heap. limits: what the engine and this file
// enforce.
static const pocket_limit_t limits[]={
    {"maxInstructions",POCKET_LIMIT_INT,KSN_SHIKI_CODE,NULL},
    {"registers",POCKET_LIMIT_INT,KSN_SHIKI_REGS,NULL},
    {"maxInputs",POCKET_LIMIT_INT,KSN_SHIKI_INPUTS,NULL},
    {"maxArguments",POCKET_LIMIT_INT,KSN_SHIKI_ARGS,NULL},
    {"maxDataRegions",POCKET_LIMIT_INT,KSN_SHIKI_REGIONS,NULL},
    {"maxSessionDataRegions",POCKET_LIMIT_INT,REGIONS,NULL},
    {"maxHandles",POCKET_LIMIT_INT,BINDS,NULL},
    {"maxSteps",POCKET_LIMIT_INT,KSN_SHIKI_STEPS,NULL},
    {"maxSegments",POCKET_LIMIT_INT,KSN_SHIKI_SEGMENTS,NULL},
    {"maxRasterSteps",POCKET_LIMIT_INT,KSN_SHIKI_RASTER,NULL},
    {"maxSurfaces",POCKET_LIMIT_INT,2,NULL},
    {"loopDepth",POCKET_LIMIT_INT,KSN_SHIKI_LOOP_DEPTH,NULL},
    {"maxRecords",POCKET_LIMIT_INT,KSN_SHIKI_RECORDS,NULL},
    {0},
};
static void probe(const pocket_capability_t *cap,bool *available,const char **reason){
    (void)cap;
    *available=shiki_heap_active();
    if(!*available)*reason=POCKET_REASON_DISABLED;
}
static const pocket_capability_t capability={.name="kasane.shiki",.supported=true,.available=false,
    .reason=POCKET_REASON_DISABLED,.limits=limits,.probe=probe};
esp_err_t pocket_shiki_api_register(void){return pocket_api_register(&capability);}

void pocket_shiki_api_reset(void){
    for(int k=0;k<REGIONS;k++)if(regions[k].live)drop_region(owner,&regions[k]);
    memset(binds,0,sizeof binds);
    owner=NULL;
}
