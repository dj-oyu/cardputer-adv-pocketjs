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
// maxDrawsPerCall (sec.9.4, v1.5): drawMany runs in native code, where the
// guest's interrupt check does not run; the watchdog is not asked between
// draws either (user decision: its cost per draw is not worth it). So a call
// cannot be stopped for at most 256 draws of up to KSN_SHIKI_STEPS steps,
// about 1 s at worst (estimate).
#define DRAWS_PER_CALL 256

typedef struct {
    JSValue window,buffer;   // both held: the window for identity, the buffer to detach
    void *mem;
    uint32_t records;        // R; the memory is k_rec planes of S = R rounded up to 8
    uint8_t type,q,k_rec;
    uint8_t live;
    uint16_t binds;          // handles bound to it
} region_t;
// lists: entries of native lists that name the handle (unregister is BUSY
// while any do, shiki-v2.md sec.5.1).
typedef struct { uint32_t handle; uint16_t lists; uint8_t n; uint8_t region[KSN_SHIKI_REGIONS]; } bind_t;

static region_t regions[REGIONS];
static bind_t binds[BINDS];

// Native lists (Shiki v2.0 draft, docs/kasane/shiki-v2.md sec.5.1): a copy of
// the handles in the Shiki heap, drawn by drawList in an order table's order.
// The id is slot + 1 + LISTS * generation, so a released list's id stays
// stale after its slot is reused.
#define LISTS 8              // maxLists
typedef struct { uint32_t *h; uint32_t id; uint16_t n; } list_t;
static list_t lists[LISTS];
static uint32_t list_generation;
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
// Built-in programs by name (sec.9.4): the apps' @shiki functions, compiled
// at build time into a const table in flash (tools/kasane_ir/emit_shiki_rom.mjs,
// the build's generated ksn_shiki_rom.c, or make_app_chunks.py's empty one).
// No weak default here: this file and the table share libmain.a, and the
// linker never pulls an archive member to replace a symbol already defined,
// so a weak empty table won on the device while the host (plain objects) passed.

static JSValue js_register(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.register";
    if(argc<1)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a program (Uint8Array or a built-in name)");
    const uint8_t *code=NULL;
    size_t len=0;
    if(JS_IsString(argv[0])){
        const char *name=JS_ToCString(ctx,argv[0]);
        unsigned rom_count=0;
        const ksn_shiki_rom_entry *rom=ksn_shiki_rom_table(&rom_count);
        for(unsigned k=0;name&&k<rom_count;k++)
            if(!strcmp(rom[k].name,name)){code=rom[k].code;len=rom[k].len;}
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
        // Reading data[k] can run a getter, which can release a window read
        // before it: the plan must not keep memory the heap has taken back.
        for(uint32_t k=0;k<nr;k++)
            if(!regions[which[k]].live||regions[which[k]].mem!=bound[k].data)
                return failure(ctx,op,POCKET_ERR_CLOSED,"a released window");
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
    binds[b]=(bind_t){.handle=handle};
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
    if(b<0)return failure(ctx,op,POCKET_ERR_CLOSED,"stale Shiki handle");
    if(binds[b].lists)return failure(ctx,op,POCKET_ERR_BUSY,"a list still draws the handle");
    if(!pocket_proc_shiki_detach(handle))return failure(ctx,op,POCKET_ERR_CLOSED,"stale Shiki handle");
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

// ----------------------------------------------------------- drawMany()
// sec.9.4 (v1.5 draft; docs/kasane/shiki-bundle.md sec.5-6): many draws in
// one call. handles is a Uint32Array; inputs holds stride inputs a
// draw, in handle order, as a Float64Array (JS numbers, converted by sec.2.4)
// or an Int32Array (already each input's Q format: no conversion). Typed
// arrays so that one pointer is taken once and every input is a load, not a
// property read (shiki-bundle.md sec.3: a call is ~1,200 cycles and an array
// element ~400). Draws run in order; the first that fails throws as draw()
// does, with the draw's position as the error's index, and abandons the
// frame, as draw() does.
static const void *typed_data(JSContext *ctx,JSValueConst v,int kind,size_t *count){
    if(JS_GetTypedArrayType(v)!=kind)return NULL;
    size_t off=0,len=0,bpe=0,size=0;
    JSValue buf=JS_GetTypedArrayBuffer(ctx,v,&off,&len,&bpe);
    if(JS_IsException(buf)){JS_FreeValue(ctx,JS_GetException(ctx));return NULL;}
    uint8_t *base=JS_GetArrayBuffer(ctx,&size,buf);
    JS_FreeValue(ctx,buf);
    if(!base){JS_FreeValue(ctx,JS_GetException(ctx));return NULL;}   /* detached */
    *count=bpe?len/bpe:0;
    return base+off;
}
static JSValue js_draw_many(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.drawMany";
    size_t count=0,ninputs=0;
    uint32_t stride=0;
    const uint32_t *handles=argc>=1?typed_data(ctx,argv[0],JS_TYPED_ARRAY_UINT32,&count):NULL;
    if(!handles||argc<3||!whole(ctx,argv[2],0,KSN_SHIKI_INPUTS,&stride))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected (Uint32Array handles, Float64Array|Int32Array inputs, stride 0..8)");
    if(count>DRAWS_PER_CALL)
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"more than 256 draws in one call");
    const double *real=NULL;
    const int32_t *fixed=NULL;
    if(stride){
        real=typed_data(ctx,argv[1],JS_TYPED_ARRAY_FLOAT64,&ninputs);
        if(!real)fixed=typed_data(ctx,argv[1],JS_TYPED_ARRAY_INT32,&ninputs);
        if((!real&&!fixed)||ninputs!=count*stride)
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"inputs: a Float64Array or Int32Array of handles.length * stride");
    }
    for(size_t i=0;i<count;i++){
        if(pocket_shiki_prof.on)pocket_shiki_prof.mark=pocket_cycles();
        JSValue r=pocket_proc_shiki_draw_inputs(ctx,handles[i],real?real+i*stride:NULL,
                                                fixed?fixed+i*stride:NULL,stride,(unsigned)i);
        if(JS_IsException(r)){
            JSValue e=JS_GetException(ctx);
            if(JS_IsObject(e))JS_SetPropertyStr(ctx,e,"index",JS_NewUint32(ctx,(uint32_t)i));
            return JS_Throw(ctx,e);
        }
    }
    return JS_UNDEFINED;
}

// ------------------------------------------------------------- lists
// list(handles), setList(list, handles, offset?), drawList(list, order?,
// count?), releaseList(list): shiki-v2.md sec.5.1 (v2.0 draft).
static int bind_of(uint32_t handle){
    for(int k=0;handle&&k<BINDS;k++)if(binds[k].handle==handle)return k;
    return -1;
}
// The live list named by v; *code says why not.
static list_t *list_of(JSContext *ctx,JSValueConst v,const char **code){
    uint32_t id=0;
    *code=POCKET_ERR_INVALID_ARGUMENT;
    if(!whole(ctx,v,1,UINT32_MAX,&id))return NULL;
    list_t *l=&lists[(id-1u)%LISTS];
    if(!l->h||l->id!=id){*code=POCKET_ERR_CLOSED;return NULL;}
    return l;
}
// handles (a Uint32Array of 1..max) whose entries are all live Shiki handles:
// 0, or 1 for a wrong shape, 2 for a stale handle.
enum { HANDLES_OK, HANDLES_BAD, HANDLES_STALE };
static int check_handles(JSContext *ctx,JSValueConst v,size_t max,const uint32_t **h,size_t *n){
    *h=typed_data(ctx,v,JS_TYPED_ARRAY_UINT32,n);
    if(!*h||*n<1||*n>max)return HANDLES_BAD;
    for(size_t k=0;k<*n;k++)if(bind_of((*h)[k])<0)return HANDLES_STALE;
    return HANDLES_OK;
}
static JSValue js_list(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.list";
    const uint32_t *h=NULL;
    size_t n=0;
    const int bad=argc>=1?check_handles(ctx,argv[0],DRAWS_PER_CALL,&h,&n):HANDLES_BAD;
    if(bad==HANDLES_STALE)return failure(ctx,op,POCKET_ERR_CLOSED,"a stale Shiki handle");
    if(bad)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a Uint32Array of 1..256 handles");
    int slot=-1;
    for(int k=0;k<LISTS&&slot<0;k++)if(!lists[k].h)slot=k;
    if(slot<0)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"8 lists");
    uint32_t *mem=shiki_heap_malloc(n*sizeof *mem);
    if(!mem)return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"the Shiki heap's budget");
    memcpy(mem,h,n*sizeof *mem);
    for(size_t k=0;k<n;k++)binds[bind_of(mem[k])].lists++;
    const uint32_t id=(uint32_t)slot+1u+LISTS*(++list_generation%(UINT32_MAX/LISTS-1u));
    lists[slot]=(list_t){mem,id,(uint16_t)n};
    return JS_NewUint32(ctx,id);
}
static JSValue js_set_list(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.setList",*code;
    list_t *l=argc>=1?list_of(ctx,argv[0],&code):NULL;
    if(!l)return failure(ctx,op,argc>=1?code:POCKET_ERR_INVALID_ARGUMENT,"expected a list");
    uint32_t off=0;
    if(argc>=3&&!JS_IsUndefined(argv[2])&&!whole(ctx,argv[2],0,l->n,&off))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"offset outside the list");
    const uint32_t *h=NULL;
    size_t n=0;
    int bad=argc>=2?check_handles(ctx,argv[1],l->n,&h,&n):HANDLES_BAD;
    if(!bad&&off+n>l->n)bad=HANDLES_BAD;
    if(bad==HANDLES_STALE)return failure(ctx,op,POCKET_ERR_CLOSED,"a stale Shiki handle");
    if(bad)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected a Uint32Array inside the list");
    for(size_t k=0;k<n;k++){
        binds[bind_of(l->h[off+k])].lists--;
        l->h[off+k]=h[k];
        binds[bind_of(h[k])].lists++;
    }
    return JS_UNDEFINED;
}
static JSValue js_release_list(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.releaseList",*code;
    list_t *l=argc>=1?list_of(ctx,argv[0],&code):NULL;
    if(!l)return failure(ctx,op,argc>=1?code:POCKET_ERR_INVALID_ARGUMENT,"expected a list");
    for(unsigned k=0;k<l->n;k++)binds[bind_of(l->h[k])].lists--;
    shiki_heap_free(l->h);
    *l=(list_t){0};
    return JS_UNDEFINED;
}
// drawList: no inputs (stride 0). Without order, entries 0..n-1; with order
// (a u16, record-1 window), entries order[0..count-1], each read at its
// draw's start (an earlier draw's STORE shows), all checked once before the
// first draw. DRAW_INDEX is the entry's number; an error's index is the
// position in order.
static JSValue js_draw_list(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.drawList",*code;
    list_t *l=argc>=1?list_of(ctx,argv[0],&code):NULL;
    if(!l)return failure(ctx,op,argc>=1?code:POCKET_ERR_INVALID_ARGUMENT,"expected a list");
    const uint16_t *order=NULL;
    uint32_t count=l->n;
    if(argc>=2&&!JS_IsUndefined(argv[1])&&!JS_IsNull(argv[1])){
        bool closed=false;
        const int g=region_of(ctx,argv[1],&closed);
        if(g<0)return failure(ctx,op,closed?POCKET_ERR_CLOSED:POCKET_ERR_INVALID_ARGUMENT,
                              closed?"a released window":"order: a u16 window of record 1 from shiki.data()");
        if(regions[g].type!=1||regions[g].k_rec!=1)
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"order: a u16 window of record 1 from shiki.data()");
        order=regions[g].mem;
        count=regions[g].records;
        if(argc>=3&&!JS_IsUndefined(argv[2])&&!whole(ctx,argv[2],0,regions[g].records,&count))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"count outside the order table");
        for(uint32_t k=0;k<count;k++)if(order[k]>=l->n)
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"order names an entry past the list");
    }
    for(uint32_t i=0;i<count;i++){
        const uint32_t e=order?order[i]:i;   /* read now: an earlier draw may have written it */
        if(pocket_shiki_prof.on)pocket_shiki_prof.mark=pocket_cycles();
        JSValue r=e<l->n?pocket_proc_shiki_draw_inputs(ctx,l->h[e],NULL,NULL,0,e)
                        :failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"order names an entry past the list");
        if(JS_IsException(r)){
            JSValue x=JS_GetException(ctx);
            if(JS_IsObject(x))JS_SetPropertyStr(ctx,x,"index",JS_NewUint32(ctx,i));
            return JS_Throw(ctx,x);
        }
    }
    return JS_UNDEFINED;
}

// ------------------------------------------------- sin() / cos() / sinMany()
// sec.9.4 (v1.5): the SIN instruction for JS. An angle is an integer in Q16
// turns (65536 a turn; only the low 16 bits matter, so any int32 wraps) and
// the result an integer in Q16 (-65536..65536), the instruction's value bit
// for bit (ksn_shiki_sin). Integers only, and not rounded: the point is that
// no double is made or read (the S3 has no double unit). A JS int is read off
// its tag; a double with an integer value is accepted, as JS may hold one.
static bool int32_arg(JSValueConst v,int32_t *out){
    const int tag=JS_VALUE_GET_TAG(v);
    if(tag==JS_TAG_INT){*out=JS_VALUE_GET_INT(v);return true;}
    if(!JS_TAG_IS_FLOAT64(tag))return false;
    const double d=JS_VALUE_GET_FLOAT64(v);
    if(!(d>=-2147483648.0&&d<=2147483647.0)||d!=floor(d))return false;
    *out=(int32_t)d;
    return true;
}
static JSValue js_sin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic){
    (void)self;
    int32_t t;
    if(argc<1||!int32_arg(argv[0],&t))
        return failure(ctx,magic?"kasane.shiki.cos":"kasane.shiki.sin",POCKET_ERR_INVALID_ARGUMENT,
                       "expected an int32 angle in Q16 turns");
    /* cos: a quarter turn on; unsigned so that the int32 ends wrap */
    return JS_NewInt32(ctx,ksn_shiki_sin(magic?(int32_t)((uint32_t)t+16384u):t));
}
// sinMany(t, out[, offset]): out[i] = sin(t[i] + offset) for Int32Arrays of
// one length (they may be the same array); offset an int32, default 0.
static JSValue js_sin_many(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.sinMany";
    size_t n=0,m=0;
    int32_t offset=0;
    const int32_t *t=argc>=2?typed_data(ctx,argv[0],JS_TYPED_ARRAY_INT32,&n):NULL;
    int32_t *out=t?(int32_t *)typed_data(ctx,argv[1],JS_TYPED_ARRAY_INT32,&m):NULL;
    if(!t||!out||n!=m||(argc>=3&&!JS_IsUndefined(argv[2])&&!int32_arg(argv[2],&offset)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected (Int32Array angles, Int32Array out of the same length, int32 offset)");
    for(size_t i=0;i<n;i++)out[i]=ksn_shiki_sin((int32_t)((uint32_t)t[i]+(uint32_t)offset));
    return JS_UNDEFINED;
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
// grid is kasane's own (pocket_grid_install), shared: copied from there.
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
        JS_CFUNC_DEF("drawMany",3,js_draw_many),
        JS_CFUNC_MAGIC_DEF("sin",1,js_sin,0),
        JS_CFUNC_MAGIC_DEF("cos",1,js_sin,1),
        JS_CFUNC_DEF("sinMany",3,js_sin_many),
        JS_CFUNC_DEF("list",1,js_list),
        JS_CFUNC_DEF("setList",3,js_set_list),
        JS_CFUNC_DEF("drawList",3,js_draw_list),
        JS_CFUNC_DEF("releaseList",1,js_release_list),
    };
    bool ok=JS_SetPropertyFunctionList(ctx,shiki,methods,(int)(sizeof methods/sizeof methods[0]))>=0;
    ok=ok&&pocket_proc_frame_install(ctx,shiki)==ESP_OK;
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
    {"maxDrawsPerCall",POCKET_LIMIT_INT,DRAWS_PER_CALL,NULL},
    {"maxLists",POCKET_LIMIT_INT,LISTS,NULL},
    {"vectorRegisters",POCKET_LIMIT_INT,8,NULL},
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
    for(int k=0;k<LISTS;k++)if(lists[k].h)shiki_heap_free(lists[k].h);
    memset(lists,0,sizeof lists);
    memset(binds,0,sizeof binds);
    owner=NULL;
}
