#include "pocket_kasane.h"
#include "pocket_api.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define KASANE_REF_LIMIT 32u
#define KASANE_REF_STORAGE 64u
#define KASANE_SCREEN ((ksn_rect){0,0,240,135})

typedef enum { REF_FREE, REF_CANDIDATE, REF_ACTIVE } ref_status;
typedef struct {
    uint32_t handle;
    ksn_ref ref;
    ksn_tx ticket;
    ref_status status;
} ref_slot;
typedef struct {
    ksn_core core;
    ksn_cache cache;
    ksn_view_host host;
    /* Two generations let a 32-reference REPLACE be built while the displayed
     * generation remains valid. Retiring identities frees slots immediately;
     * old wrappers/finalizers cannot affect subsequently reused slots. */
    ref_slot refs[KASANE_REF_STORAGE];
    ksn_tx building;
    ksn_tx submitted;
    ksn_update_mode submitted_mode;
    bool active;
} kasane_state;

static kasane_state *state;
/* Never recycle identities across host reset while old JS wrappers can live. */
static uint32_t ref_serial;
static JSClassID tx_class, modal_class, ref_class, template_class;
static JSClassID instance_class, ticket_class;
static JSRuntime *tx_rt, *modal_rt, *ref_rt, *template_rt;
static JSRuntime *instance_rt, *ticket_rt;

static const char *result_code(ksn_result result) {
    switch(result) {
    case KSN_OK: return "OK";
    case KSN_INVALID: return POCKET_ERR_INVALID_ARGUMENT;
    case KSN_LIMIT: return POCKET_ERR_LIMIT_EXCEEDED;
    case KSN_OOM: return POCKET_ERR_OUT_OF_MEMORY;
    case KSN_STALE: return POCKET_ERR_CLOSED;
    case KSN_BUSY: return POCKET_ERR_BUSY;
    case KSN_UNSUPPORTED: return POCKET_ERR_UNSUPPORTED;
    case KSN_IO: return POCKET_ERR_IO_ERROR;
    case KSN_CANCELLED: return POCKET_ERR_CANCELLED;
    }
    return POCKET_ERR_CONFLICT;
}

static JSValue throw_result(JSContext *ctx, ksn_result result, const char *op) {
    const char *code=result_code(result);
    return pocket_api_throw(ctx,code,op,code,
                            result==KSN_BUSY||result==KSN_OOM||result==KSN_IO,
                            POCKET_OUTCOME_NOT_APPLIED);
}

static bool ensure_state(JSContext *ctx, const char *op) {
    if(state) return true;
    state=calloc(1,sizeof(*state));
    if(!state) {
        pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                         "Kasane native arena could not be allocated",true,
                         POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    ksn_view_host_init(&state->host,&state->core,&state->cache,0);
    return true;
}

static ksn_view *view(void) {
    return state?ksn_view_host_endpoint(&state->host,KSN_APP):NULL;
}

static uint32_t opaque_value(JSValueConst value, JSClassID class_id) {
    return (uint32_t)(uintptr_t)JS_GetOpaque(value,class_id);
}

static void release_ref_handle(uint32_t handle) {
    if(!state||!handle) return;
    unsigned index=handle&63u;
    if(state->refs[index].handle==handle)
        state->refs[index]=(ref_slot){0};
}

static void ref_finalizer(JSRuntime *rt, JSValue value) {
    (void)rt;
    release_ref_handle(opaque_value(value,ref_class));
}

static const JSClassDef tx_def={.class_name="KasaneTransaction"};
static const JSClassDef modal_def={.class_name="KasaneModalTransaction"};
static const JSClassDef ref_def={.class_name="KasaneDrawRef",.finalizer=ref_finalizer};
static const JSClassDef template_def={.class_name="KasaneTemplate"};
static const JSClassDef instance_def={.class_name="KasaneInstanceRef"};
static const JSClassDef ticket_def={.class_name="KasaneTicket"};

static ref_slot *ref_from(JSContext *ctx, JSValueConst self, const char *op) {
    uint32_t handle=opaque_value(self,ref_class);
    if(!state||!handle) {
        pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"draw reference is closed",false,NULL);
        return NULL;
    }
    ref_slot *slot=&state->refs[handle&63u];
    if(slot->handle!=handle||slot->status==REF_FREE) {
        pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"draw reference is stale",false,NULL);
        return NULL;
    }
    return slot;
}

static ref_slot *claim_ref(JSContext *ctx, ksn_ref ref, ksn_tx ticket) {
    unsigned candidates=0;
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
        if(state->refs[i].status==REF_CANDIDATE&&
           state->refs[i].ticket.value==ticket.value) candidates++;
    if(candidates==KASANE_REF_LIMIT) {
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.rect",
                         "the update exposes more than 32 draw references",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++) {
        if(state->refs[i].status!=REF_FREE) continue;
        if(ref_serial==0x03ffffffu) break;
        uint32_t handle=(++ref_serial<<6)|i;
        state->refs[i]=(ref_slot){handle,ref,ticket,REF_CANDIDATE};
        return &state->refs[i];
    }
    pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.rect",
                     "draw-reference wrapper storage is in use",false,
                     POCKET_OUTCOME_NOT_APPLIED);
    return NULL;
}

static void discard_candidates(ksn_tx ticket) {
    if(!state||!ticket.value) return;
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
        if(state->refs[i].status==REF_CANDIDATE &&
           state->refs[i].ticket.value==ticket.value)
            state->refs[i]=(ref_slot){0};
}

static void apply_outcome(void) {
    if(!state||!state->submitted.value) return;
    ksn_submission result=ksn_view_poll(view());
    if(result.ticket.value!=state->submitted.value||
       (result.status!=KSN_PRESENTED&&result.status!=KSN_DISCARDED)) return;
    if(result.status==KSN_PRESENTED) {
        if(state->submitted_mode==KSN_REPLACE)
            for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
                if(state->refs[i].status==REF_ACTIVE)
                    state->refs[i]=(ref_slot){0};
        for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
            if(state->refs[i].status==REF_CANDIDATE&&
               state->refs[i].ticket.value==result.ticket.value)
                state->refs[i].status=REF_ACTIVE;
    } else discard_candidates(result.ticket);
    state->submitted=(ksn_tx){0};
}

static bool number_in(JSContext *ctx, JSValueConst value, double lo, double hi,
                      double *out) {
    double n;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)||
       floor(n)!=n||n<lo||n>hi) return false;
    *out=n;
    return true;
}

static bool parse_i16(JSContext *ctx, JSValueConst value, int16_t *out) {
    double n;
    if(!number_in(ctx,value,INT16_MIN,INT16_MAX,&n)) return false;
    *out=(int16_t)n;
    return true;
}

static bool parse_u8(JSContext *ctx, JSValueConst value, uint8_t *out) {
    double n;
    if(!number_in(ctx,value,0,255,&n)) return false;
    *out=(uint8_t)n;
    return true;
}

static bool parse_u32(JSContext *ctx, JSValueConst value, uint32_t *out) {
    double n;
    if(!number_in(ctx,value,0,4294967295.0,&n)) return false;
    *out=(uint32_t)n;
    return true;
}

static bool parse_rect(JSContext *ctx, JSValueConst value, ksn_rect *out,
                       const char *op) {
    int64_t length=0;
    if(JS_IsArray(value)&&JS_GetLength(ctx,value,&length)<0) return false;
    if(!JS_IsArray(value)||length!=4) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be [x0, y0, x1, y1]",false,NULL);
        return false;
    }
    int16_t coords[4];
    for(unsigned i=0;i<4;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,value,i);
        if(JS_IsException(item)) return false;
        bool ok=parse_i16(ctx,item,&coords[i]);
        JS_FreeValue(ctx,item);
        if(!ok) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "rectangle coordinates must be int16 values",false,NULL);
            return false;
        }
    }
    *out=(ksn_rect){coords[0],coords[1],coords[2],coords[3]};
    if(out->x0>out->x1||out->y0>out->y1) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle edges are reversed",false,NULL);
        return false;
    }
    return true;
}

static bool property_rect(JSContext *ctx, JSValueConst object, const char *name,
                          ksn_rect fallback, ksn_rect *out, const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { *out=fallback; JS_FreeValue(ctx,value); return true; }
    bool ok=parse_rect(ctx,value,out,op);
    JS_FreeValue(ctx,value);
    return ok;
}

static bool property_u8(JSContext *ctx, JSValueConst object, const char *name,
                        uint8_t fallback, uint8_t *out, const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { *out=fallback; JS_FreeValue(ctx,value); return true; }
    bool ok=parse_u8(ctx,value,out);
    JS_FreeValue(ctx,value);
    if(!ok) pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "opacity must be an integer from 0 to 255",false,NULL);
    return ok;
}

static bool parse_draw(JSContext *ctx, JSValueConst value, ksn_draw *out,
                       const char *op) {
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be an object",false,NULL);
        return false;
    }
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&parse_rect(ctx,bounds,&out->bounds,op);
    JS_FreeValue(ctx,bounds);
    if(!ok) return false;
    out->kind=KSN_RECT;
    out->clip=out->bounds;
    if(!property_rect(ctx,value,"clip",out->bounds,&out->clip,op)) return false;
    if(!property_u8(ctx,value,"opacity",255,&out->opacity,op)) return false;
    JSValue color=JS_GetPropertyStr(ctx,value,"color");
    if(JS_IsException(color)) return false;
    ok=parse_u32(ctx,color,&out->data.shape.color);
    JS_FreeValue(ctx,color);
    if(!ok) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "color must be an RRGGBBAA uint32",false,NULL);
        return false;
    }
    return true;
}

static bool parse_placement(JSContext *ctx, JSValueConst value, ksn_placement *out,
                            const char *op) {
    *out=(ksn_placement){.clip=KASANE_SCREEN,.opacity=255,.visible=true};
    if(JS_IsUndefined(value)) return true;
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "placement must be an object",false,NULL);
        return false;
    }
    JSValue offset=JS_GetPropertyStr(ctx,value,"offset");
    if(JS_IsException(offset)) return false;
    if(!JS_IsUndefined(offset)) {
        int64_t length=0;
        if(JS_IsArray(offset)&&JS_GetLength(ctx,offset,&length)<0) {
            JS_FreeValue(ctx,offset);return false;
        }
        if(!JS_IsArray(offset)||length!=2) {
            JS_FreeValue(ctx,offset);
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "offset must be [x, y]",false,NULL);
            return false;
        }
        JSValue x=JS_GetPropertyUint32(ctx,offset,0);
        if(JS_IsException(x)) { JS_FreeValue(ctx,offset);return false; }
        JSValue y=JS_GetPropertyUint32(ctx,offset,1);
        if(JS_IsException(y)) {
            JS_FreeValue(ctx,x);JS_FreeValue(ctx,offset);return false;
        }
        bool ok=parse_i16(ctx,x,&out->x)&&parse_i16(ctx,y,&out->y);
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,offset);
        if(!ok) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "offset coordinates must be int16 values",false,NULL);
            return false;
        }
    } else JS_FreeValue(ctx,offset);
    if(!property_rect(ctx,value,"clip",KASANE_SCREEN,&out->clip,op)) return false;
    if(!property_u8(ctx,value,"opacity",255,&out->opacity,op)) return false;
    JSValue visible=JS_GetPropertyStr(ctx,value,"visible");
    if(JS_IsException(visible)) return false;
    if(!JS_IsUndefined(visible)) {
        if(!JS_IsBool(visible)) {
            JS_FreeValue(ctx,visible);
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "visible must be boolean",false,NULL);
            return false;
        }
        out->visible=JS_ToBool(ctx,visible);
    }
    JS_FreeValue(ctx,visible);
    return true;
}

static ksn_tx tx_from(JSContext *ctx, JSValueConst value, const char *op) {
    uint32_t raw=opaque_value(value,tx_class);
    if(!raw) pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,
                              "transaction is outside its build callback",false,NULL);
    return (ksn_tx){raw};
}

static JSValue wrap_direct(JSContext *ctx, JSClassID class_id, uint32_t handle) {
    JSValue object=JS_NewObjectClass(ctx,class_id);
    if(JS_IsException(object)) return object;
    JS_SetOpaque(object,(void *)(uintptr_t)handle);
    return object;
}

static JSValue js_tx_background(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.background");
    if(!tx.value) return JS_EXCEPTION;
    uint32_t color;
    if(argc<1||!parse_u32(ctx,argv[0],&color))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.background",
                                "color must be an RRGGBBAA uint32",false,NULL);
    ksn_result result=ksn_view_background(view(),tx,color);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.background");
}

static JSValue js_tx_rect(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.rect");
    if(!tx.value) return JS_EXCEPTION;
    ksn_draw draw={0};
    if(!parse_draw(ctx,argc?argv[0]:JS_UNDEFINED,&draw,"kasane.rect")) return JS_EXCEPTION;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    if(result!=KSN_OK) return throw_result(ctx,result,"kasane.rect");
    ref_slot *slot=claim_ref(ctx,ref,tx);
    if(!slot) { ksn_view_cancel(view(),tx); discard_candidates(tx); return JS_EXCEPTION; }
    JSValue object=wrap_direct(ctx,ref_class,slot->handle);
    if(JS_IsException(object)) {
        *slot=(ref_slot){0};ksn_view_cancel(view(),tx);discard_candidates(tx);
    }
    return object;
}

static JSValue js_tx_group(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.group");
    if(!tx.value) return JS_EXCEPTION;
    if(argc<3)
        return throw_result(ctx,KSN_INVALID,"kasane.group");
    ref_slot *first=ref_from(ctx,argv[0],"kasane.group");
    double count_number;uint8_t opacity;
    if(!first) return JS_EXCEPTION;
    if(!number_in(ctx,argv[1],1,UINT16_MAX,&count_number)||
       !parse_u8(ctx,argv[2],&opacity))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.group",
                                "group(firstRef, count, opacity) requires integer values",
                                false,NULL);
    ksn_result result=ksn_view_group(view(),tx,first->ref,(uint16_t)count_number,opacity);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.group");
}

static JSValue js_tx_instantiate(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.instantiate");
    if(!tx.value) return JS_EXCEPTION;
    uint32_t raw=argc?opaque_value(argv[0],template_class):0;
    ksn_placement placement;
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.instantiate",
                                "template is closed",false,NULL);
    if(!parse_placement(ctx,argc>1?argv[1]:JS_UNDEFINED,&placement,
                        "kasane.instantiate")) return JS_EXCEPTION;
    JSValue object=wrap_direct(ctx,instance_class,0);
    if(JS_IsException(object)) return object;
    ksn_instance instance;ksn_result result=ksn_view_instantiate(
        view(),tx,(ksn_template){raw},&placement,&instance);
    if(result!=KSN_OK) {
        JS_FreeValue(ctx,object);return throw_result(ctx,result,"kasane.instantiate");
    }
    JS_SetOpaque(object,(void *)(uintptr_t)instance.value);return object;
}

static JSValue change_ref(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv, ksn_property property,
                          const char *op) {
    ref_slot *slot=ref_from(ctx,self,op);
    if(!slot) return JS_EXCEPTION;
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    if(slot->status==REF_CANDIDATE&&slot->ticket.value!=tx.value)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,
                                "candidate reference belongs to another transaction",
                                false,NULL);
    ksn_change change={.property=property};
    if(property==KSN_SET_RECT||property==KSN_SET_CLIP) {
        if(!parse_rect(ctx,argc>1?argv[1]:JS_UNDEFINED,&change.value.rect,op)) return JS_EXCEPTION;
    } else if(property==KSN_SET_COLOR) {
        if(argc<2||!parse_u32(ctx,argv[1],&change.value.color))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "color must be an RRGGBBAA uint32",false,NULL);
    } else {
        if(argc<2||!JS_IsBool(argv[1]))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "visible must be boolean",false,NULL);
        change.value.visible=JS_ToBool(ctx,argv[1]);
    }
    ksn_result result=ksn_view_change(view(),tx,slot->ref,&change);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
}

#define REF_SETTER(name,property,op) \
static JSValue name(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return change_ref(ctx,self,argc,argv,property,op); \
}
REF_SETTER(js_ref_rect,KSN_SET_RECT,"kasane.ref.setRect")
REF_SETTER(js_ref_clip,KSN_SET_CLIP,"kasane.ref.setClip")
REF_SETTER(js_ref_color,KSN_SET_COLOR,"kasane.ref.setColor")
REF_SETTER(js_ref_visible,KSN_SET_VISIBLE,"kasane.ref.setVisible")

static JSValue js_instance_place(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv) {
    uint32_t raw=opaque_value(self,instance_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.instance.place",
                                "instance is closed",false,NULL);
    ksn_tx tx=argc?tx_from(ctx,argv[0],"kasane.instance.place"):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    ksn_placement placement;
    if(!parse_placement(ctx,argc>1?argv[1]:JS_UNDEFINED,&placement,
                        "kasane.instance.place")) return JS_EXCEPTION;
    ksn_result result=ksn_view_place(view(),tx,(ksn_instance){raw},&placement);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.instance.place");
}

static JSValue js_instance_visible(JSContext *ctx, JSValueConst self, int argc,
                                   JSValueConst *argv) {
    uint32_t raw=opaque_value(self,instance_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,
                                "kasane.instance.setVisible","instance is closed",
                                false,NULL);
    ksn_tx tx=argc?tx_from(ctx,argv[0],"kasane.instance.setVisible"):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    if(argc<2||!JS_IsBool(argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                "kasane.instance.setVisible","visible must be boolean",
                                false,NULL);
    ksn_result result=ksn_view_visible(view(),tx,(ksn_instance){raw},JS_ToBool(ctx,argv[1]));
    return result==KSN_OK?JS_UNDEFINED
                        :throw_result(ctx,result,"kasane.instance.setVisible");
}

static JSValue js_modal_open(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv) {
    uint32_t raw=opaque_value(self,modal_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.modal.open",
                                "transaction is outside its build callback",false,NULL);
    if(argc<1||!JS_IsObject(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "modal spec must be an object",false,NULL);
    ksn_modal_backdrop backdrop=KSN_MODAL_SOLID;
    JSValue mode=JS_GetPropertyStr(ctx,argv[0],"backdrop");
    if(JS_IsException(mode)) return mode;
    if(!JS_IsUndefined(mode)) {
        const char *text=JS_IsString(mode)?JS_ToCString(ctx,mode):NULL;
        if(JS_IsString(mode)&&!text) { JS_FreeValue(ctx,mode);return JS_EXCEPTION; }
        if(text&&strcmp(text,"dim-live")==0) backdrop=KSN_MODAL_DIM_LIVE;
        else if(!text||strcmp(text,"solid")!=0) {
            if(text)JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,mode);
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                    "kasane.modal.open",
                                    "backdrop must be 'solid' or 'dim-live'",false,NULL);
        }
        if(text)JS_FreeCString(ctx,text);
    }
    JS_FreeValue(ctx,mode);
    uint32_t color=0x00000080u,focus=0;
    JSValue color_value=JS_GetPropertyStr(ctx,argv[0],"color");
    if(JS_IsException(color_value)) return color_value;
    if(!JS_IsUndefined(color_value)&&!parse_u32(ctx,color_value,&color)) {
        JS_FreeValue(ctx,color_value);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "color must be an RRGGBBAA uint32",false,NULL);
    }
    JS_FreeValue(ctx,color_value);
    JSValue focus_value=JS_GetPropertyStr(ctx,argv[0],"focus");
    if(JS_IsException(focus_value)) return focus_value;
    if(!JS_IsUndefined(focus_value)&&!parse_u32(ctx,focus_value,&focus)) {
        JS_FreeValue(ctx,focus_value);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "focus must be a uint32",false,NULL);
    }
    JS_FreeValue(ctx,focus_value);
    ksn_result result=ksn_view_modal_open(view(),(ksn_tx){raw},backdrop,color,focus);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.modal.open");
}

static JSValue js_modal_close(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
    (void)argc;(void)argv;
    uint32_t raw=opaque_value(self,modal_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.modal.close",
                                "transaction is outside its build callback",false,NULL);
    ksn_result result=ksn_view_modal_close(view(),(ksn_tx){raw});
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.modal.close");
}

/* Validate ownership before entering any parser (which can invoke JS getters).
 * Every exception in an owning mutation aborts even when the callback catches
 * it. A foreign/expired transaction must never cancel the current builder. */
static JSValue mutate(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv, JSClassID class_id, bool tx_argument,
                       JSCFunction *function, const char *op) {
    JSValueConst token=tx_argument?(argc?argv[0]:JS_UNDEFINED):self;
    ksn_tx tx={opaque_value(token,class_id)};
    if(!state||!tx.value||state->building.value!=tx.value||
       state->host.builder.value!=tx.value)
        return throw_result(ctx,KSN_STALE,op);
    JSValue result=function(ctx,self,argc,argv);
    if(JS_IsException(result)) {
        ksn_view_cancel(view(),tx);discard_candidates(tx);
    }
    return result;
}

#define MUTATOR(name,class_id,tx_argument,op) \
static JSValue name##_checked(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return mutate(ctx,self,argc,argv,class_id,tx_argument,name,op); \
}
MUTATOR(js_tx_background,tx_class,false,"kasane.background")
MUTATOR(js_tx_rect,tx_class,false,"kasane.rect")
MUTATOR(js_tx_group,tx_class,false,"kasane.group")
MUTATOR(js_tx_instantiate,tx_class,false,"kasane.instantiate")
MUTATOR(js_ref_rect,tx_class,true,"kasane.ref.setRect")
MUTATOR(js_ref_clip,tx_class,true,"kasane.ref.setClip")
MUTATOR(js_ref_color,tx_class,true,"kasane.ref.setColor")
MUTATOR(js_ref_visible,tx_class,true,"kasane.ref.setVisible")
MUTATOR(js_instance_place,tx_class,true,"kasane.instance.place")
MUTATOR(js_instance_visible,tx_class,true,"kasane.instance.setVisible")
MUTATOR(js_modal_open,modal_class,false,"kasane.modal.open")
MUTATOR(js_modal_close,modal_class,false,"kasane.modal.close")
#undef MUTATOR

static JSValue run_build(JSContext *ctx, int argc, JSValueConst *argv,
                         ksn_update_mode mode, const char *op) {
    if(argc<1||!JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must be a synchronous function",false,NULL);
    if(!ensure_state(ctx,op)) return JS_EXCEPTION;
    /* Keep the callback closed to reentrant builds after an inner abort. */
    if(state->building.value) return throw_result(ctx,KSN_BUSY,op);
    apply_outcome();
    ksn_tx tx;ksn_result result=ksn_view_begin(view(),mode,&tx);
    if(result!=KSN_OK) return throw_result(ctx,result,op);
    state->building=tx;
    JSValue ticket=JS_UNDEFINED,tx_object=JS_UNDEFINED,modal_object=JS_UNDEFINED;
    /* No fallible allocation may follow successful native submission. */
    ticket=wrap_direct(ctx,ticket_class,tx.value);
    if(JS_IsException(ticket)) goto fail;
    tx_object=wrap_direct(ctx,tx_class,tx.value);
    if(JS_IsException(tx_object)) goto fail;
    modal_object=wrap_direct(ctx,modal_class,tx.value);
    if(JS_IsException(modal_object)) goto fail;
    if(JS_DefinePropertyValueStr(ctx,tx_object,"modal",JS_DupValue(ctx,modal_object),
                                 JS_PROP_C_W_E)<0) goto fail;
    JSValue arg=JS_DupValue(ctx,tx_object);
    JSValue returned=JS_Call(ctx,argv[0],JS_UNDEFINED,1,&arg);
    JS_FreeValue(ctx,arg);
    JS_SetOpaque(tx_object,NULL);JS_SetOpaque(modal_object,NULL);
    if(JS_IsException(returned)) goto fail;
    bool thenable=false;
    if(JS_IsObject(returned)) {
        JSValue then=JS_GetPropertyStr(ctx,returned,"then");
        if(JS_IsException(then)) {
            JS_FreeValue(ctx,returned);goto fail;
        }
        thenable=JS_IsFunction(ctx,then);JS_FreeValue(ctx,then);
    }
    JS_FreeValue(ctx,returned);
    if(thenable) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must not return a Promise or thenable",false,
                                POCKET_OUTCOME_NOT_APPLIED);
        goto fail;
    }
    result=ksn_view_submit(view(),tx);
    if(result!=KSN_OK) { throw_result(ctx,result,op);goto fail; }
    state->building=(ksn_tx){0};
    state->submitted=tx;state->submitted_mode=mode;state->active=true;
    JS_FreeValue(ctx,tx_object);JS_FreeValue(ctx,modal_object);
    return ticket;
fail:
    if(JS_IsObject(tx_object)) JS_SetOpaque(tx_object,NULL);
    if(JS_IsObject(modal_object)) JS_SetOpaque(modal_object,NULL);
    JS_FreeValue(ctx,tx_object);JS_FreeValue(ctx,modal_object);JS_FreeValue(ctx,ticket);
    ksn_view_cancel(view(),tx);discard_candidates(tx);state->building=(ksn_tx){0};
    return JS_EXCEPTION;
}

static JSValue js_replace(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv) {
    (void)self;return run_build(ctx,argc,argv,KSN_REPLACE,"kasane.replace");
}
static JSValue js_patch(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;return run_build(ctx,argc,argv,KSN_PATCH,"kasane.patch");
}

static JSValue js_cache_create(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv) {
    (void)self;
    if(argc<1||!JS_IsArray(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.cache.create",
                                "definition must be an array of rectangles",false,NULL);
    if(!ensure_state(ctx,"kasane.cache.create")) return JS_EXCEPTION;
    if(state->building.value) return throw_result(ctx,KSN_BUSY,"kasane.cache.create");
    apply_outcome();
    int64_t length=0;
    if(JS_GetLength(ctx,argv[0],&length)<0) return JS_EXCEPTION;
    if(length<1||length>KSN_CACHE_COMMANDS)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.cache.create",
                                "cache definition has an invalid command count",false,NULL);
    ksn_draw draws[KSN_CACHE_COMMANDS];
    memset(draws,0,sizeof(draws));
    for(int64_t i=0;i<length;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,argv[0],(uint32_t)i);
        bool ok=!JS_IsException(item)&&parse_draw(ctx,item,&draws[i],"kasane.cache.create");
        JS_FreeValue(ctx,item);
        if(!ok) return JS_EXCEPTION;
    }
    JSValue object=wrap_direct(ctx,template_class,0);
    if(JS_IsException(object)) return object;
    ksn_template result_handle;
    ksn_result result=ksn_view_cache_create(view(),draws,(uint16_t)length,&result_handle);
    if(result!=KSN_OK) {
        JS_FreeValue(ctx,object);return throw_result(ctx,result,"kasane.cache.create");
    }
    JS_SetOpaque(object,(void *)(uintptr_t)result_handle.value);return object;
}

static JSValue js_cache_release(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv) {
    (void)self;
    uint32_t raw=argc?opaque_value(argv[0],template_class):0;
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.cache.release",
                                "template is closed",false,NULL);
    if(state&&state->building.value) return throw_result(ctx,KSN_BUSY,"kasane.cache.release");
    apply_outcome();
    ksn_result result=ksn_view_cache_release(view(),(ksn_template){raw});
    if(result!=KSN_OK) return throw_result(ctx,result,"kasane.cache.release");
    JS_SetOpaque(argv[0],NULL);return JS_UNDEFINED;
}

static const char *status_name(ksn_submission_status status) {
    switch(status) {
    case KSN_SUBMITTED:return "SUBMITTED";
    case KSN_PRESENTED:return "PRESENTED";
    case KSN_DISCARDED:return "DISCARDED";
    default:return "NONE";
    }
}

/* Defining own properties avoids invoking user-installed prototype setters.
 * JS_DefinePropertyValueStr consumes value on both success and failure. */
static bool put(JSContext *ctx, JSValueConst object, const char *name, JSValue value) {
    if(JS_IsException(value)) return false;
    return JS_DefinePropertyValueStr(ctx,object,name,value,JS_PROP_C_W_E)>=0;
}
#define PUT(object,name,value) do { if(!put(ctx,object,name,value)) goto fail; } while(0)

static JSValue js_poll(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    apply_outcome();
    ksn_submission submission=state?ksn_view_poll(view()):(ksn_submission){0};
    JSValue out=JS_NewObject(ctx);
    if(JS_IsException(out)) return out;
    PUT(out,"ticket",submission.ticket.value
        ?wrap_direct(ctx,ticket_class,submission.ticket.value):JS_NULL);
    PUT(out,"status",JS_NewString(ctx,status_name(submission.status)));
    PUT(out,"reason",JS_NewString(ctx,result_code(submission.reason)));
    PUT(out,"layer",JS_NewString(ctx,"app"));
    return out;
fail:
    JS_FreeValue(ctx,out);return JS_EXCEPTION;
}

static JSValue js_cancel(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv) {
    (void)self;
    uint32_t raw=argc?opaque_value(argv[0],ticket_class):0;
    if(!state||!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.cancel",
                                "ticket is closed",false,NULL);
    ksn_result result=ksn_view_cancel(view(),(ksn_tx){raw});
    if(result!=KSN_OK) return throw_result(ctx,result,"kasane.cancel");
    apply_outcome();return JS_UNDEFINED;
}

static JSValue js_features(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    JSValue out=JS_UNDEFINED,capacity=JS_UNDEFINED,cache=JS_UNDEFINED;
    out=JS_NewObject(ctx);if(JS_IsException(out)) goto fail;
    capacity=JS_NewObject(ctx);if(JS_IsException(capacity)) goto fail;
    cache=JS_NewObject(ctx);if(JS_IsException(cache)) goto fail;
    PUT(out,"rect",JS_NewBool(ctx,true));
    PUT(out,"groupOpacity",JS_NewBool(ctx,true));
    PUT(out,"modal",JS_NewBool(ctx,true));
    PUT(out,"animation",JS_NewBool(ctx,false));
    PUT(out,"frosted",JS_NewBool(ctx,false));
    PUT(capacity,"commands",JS_NewInt32(ctx,KSN_APP_COMMANDS));
    PUT(capacity,"textBytes",JS_NewInt32(ctx,KSN_APP_TEXT_BYTES));
    PUT(capacity,"refs",JS_NewInt32(ctx,KASANE_REF_LIMIT));
    PUT(cache,"commands",JS_NewInt32(ctx,KSN_CACHE_COMMANDS));
    PUT(cache,"templates",JS_NewInt32(ctx,KSN_CACHE_TEMPLATES));
    PUT(cache,"instances",JS_NewInt32(ctx,KSN_CACHE_INSTANCES));
    PUT(out,"capacity",JS_DupValue(ctx,capacity));
    PUT(out,"cache",JS_DupValue(ctx,cache));
    JS_FreeValue(ctx,capacity);JS_FreeValue(ctx,cache);return out;
fail:
    JS_FreeValue(ctx,out);JS_FreeValue(ctx,capacity);JS_FreeValue(ctx,cache);
    return JS_EXCEPTION;
}

static JSValue js_stats(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    apply_outcome();
    ksn_view_stats stats=state?ksn_view_get_stats(view()):(ksn_view_stats){0};
    JSValue out=JS_UNDEFINED,displayed=JS_UNDEFINED,cache=JS_UNDEFINED;
    out=JS_NewObject(ctx);if(JS_IsException(out)) goto fail;
    displayed=JS_NewObject(ctx);if(JS_IsException(displayed)) goto fail;
    cache=JS_NewObject(ctx);if(JS_IsException(cache)) goto fail;
    PUT(out,"active",JS_NewBool(ctx,state&&state->active));
    PUT(out,"nativeBytes",JS_NewUint32(ctx,state?(uint32_t)sizeof(*state):0));
    PUT(displayed,"commands",JS_NewInt32(ctx,stats.displayed.commands));
    PUT(displayed,"textBytes",JS_NewInt32(ctx,stats.displayed.text_bytes));
    PUT(cache,"commands",JS_NewInt32(ctx,stats.shared_cache.commands));
    PUT(cache,"templates",JS_NewInt32(ctx,stats.shared_cache.templates));
    PUT(cache,"instances",JS_NewInt32(ctx,stats.shared_cache.instances));
    PUT(out,"displayed",JS_DupValue(ctx,displayed));PUT(out,"cache",JS_DupValue(ctx,cache));
    JS_FreeValue(ctx,displayed);JS_FreeValue(ctx,cache);return out;
fail:
    JS_FreeValue(ctx,out);JS_FreeValue(ctx,displayed);JS_FreeValue(ctx,cache);
    return JS_EXCEPTION;
}
#undef PUT

static JSValue js_input_scope(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    ksn_input_scope scope=pocket_kasane_input_scope(false);
    const char *name=scope==KSN_INPUT_MODAL?"modal":scope==KSN_INPUT_BLOCKED?"blocked":"app";
    return JS_NewString(ctx,name);
}

static const JSCFunctionListEntry tx_methods[]={
    JS_CFUNC_DEF("background",1,js_tx_background_checked),
    JS_CFUNC_DEF("rect",1,js_tx_rect_checked),
    JS_CFUNC_DEF("group",3,js_tx_group_checked),
    JS_CFUNC_DEF("instantiate",2,js_tx_instantiate_checked),
};
static const JSCFunctionListEntry modal_methods[]={
    JS_CFUNC_DEF("open",1,js_modal_open_checked),JS_CFUNC_DEF("close",0,js_modal_close_checked),
};
static const JSCFunctionListEntry ref_methods[]={
    JS_CFUNC_DEF("setRect",2,js_ref_rect_checked),JS_CFUNC_DEF("setClip",2,js_ref_clip_checked),
    JS_CFUNC_DEF("setColor",2,js_ref_color_checked),JS_CFUNC_DEF("setVisible",2,js_ref_visible_checked),
};
static const JSCFunctionListEntry instance_methods[]={
    JS_CFUNC_DEF("place",2,js_instance_place_checked),
    JS_CFUNC_DEF("setVisible",2,js_instance_visible_checked),
};
static const JSCFunctionListEntry functions[]={
    JS_CFUNC_DEF("replace",1,js_replace),JS_CFUNC_DEF("patch",1,js_patch),
    JS_CFUNC_DEF("poll",0,js_poll),JS_CFUNC_DEF("cancel",1,js_cancel),
    JS_CFUNC_DEF("features",0,js_features),JS_CFUNC_DEF("stats",0,js_stats),
    JS_CFUNC_DEF("inputScope",0,js_input_scope),
};

static bool make_class(JSContext *ctx, JSClassID *id, JSRuntime **owner,
                       const JSClassDef *def, const JSCFunctionListEntry *methods,
                       int count) {
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(!pocket_api_class_ready(rt,owner,id)) {
        JS_NewClassID(rt,id);
        if(JS_NewClass(rt,*id,def)<0) return false;
    }
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return false;
    if(count&&JS_SetPropertyFunctionList(ctx,proto,methods,count)<0) {
        JS_FreeValue(ctx,proto);return false;
    }
    JS_SetClassProto(ctx,*id,proto);return true;
}

static esp_err_t build_kasane(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
#define MAKE(id,owner,def,methods) make_class(ctx,&id,&owner,&def,methods,(int)(sizeof(methods)/sizeof(methods[0])))
    if(!MAKE(tx_class,tx_rt,tx_def,tx_methods)||
       !MAKE(modal_class,modal_rt,modal_def,modal_methods)||
       !MAKE(ref_class,ref_rt,ref_def,ref_methods)||
       !MAKE(instance_class,instance_rt,instance_def,instance_methods)||
       !make_class(ctx,&template_class,&template_rt,&template_def,NULL,0)||
       !make_class(ctx,&ticket_class,&ticket_rt,&ticket_def,NULL,0)) return ESP_ERR_NO_MEM;
#undef MAKE
    if(JS_SetPropertyFunctionList(ctx,ns,functions,
       (int)(sizeof(functions)/sizeof(functions[0])))<0) return ESP_ERR_NO_MEM;
    JSValue cache=JS_NewObject(ctx);
    if(JS_IsException(cache)) return ESP_ERR_NO_MEM;
    /* QuickJS keeps these entries for lazy function materialization. */
    static const JSCFunctionListEntry cache_functions[]={
        JS_CFUNC_DEF("create",1,js_cache_create),JS_CFUNC_DEF("release",1,js_cache_release),
    };
    if(JS_SetPropertyFunctionList(ctx,cache,cache_functions,2)<0) {
        JS_FreeValue(ctx,cache);return ESP_ERR_NO_MEM;
    }
    return JS_SetPropertyStr(ctx,ns,"cache",cache)<0?ESP_ERR_NO_MEM:ESP_OK;
}

static const pocket_limit_t kasane_limits[]={
    {.name="commands",.kind=POCKET_LIMIT_INT,.number=KSN_APP_COMMANDS},
    {.name="references",.kind=POCKET_LIMIT_INT,.number=KASANE_REF_LIMIT},
    {.name="cacheTemplates",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_TEMPLATES},
    {.name="cacheInstances",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_INSTANCES},
    {0},
};
static const pocket_capability_t capability={
    .name="display.kasane",.supported=true,.available=true,.limits=kasane_limits,
};

esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    esp_err_t result=pocket_api_register(&capability);
    return result==ESP_OK?pocket_api_lazy(ctx,"kasane",build_kasane,NULL):result;
}

void pocket_kasane_reset(void) {
    free(state);state=NULL;
}
bool pocket_kasane_active(void) { return state&&state->active; }
bool pocket_kasane_has_submission(void) {
    return state&&ksn_core_has_submission(&state->core);
}
bool pocket_kasane_needs_present(void) {
    return state&&state->active&&ksn_view_host_needs_present(&state->host);
}
void pocket_kasane_invalidate(void) {
    if(state)ksn_view_host_invalidate(&state->host);
}
ksn_result pocket_kasane_present(const ksn_display_port *display,ksn_render_stats *stats) {
    if(!stats) return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    if(!pocket_kasane_needs_present()) return KSN_OK;
    ksn_result result=ksn_view_host_present(&state->host,display,stats);
    apply_outcome();return result;
}
void pocket_kasane_end_turn(void) {
    if(state) { ksn_view_host_end_turn(&state->host); apply_outcome(); }
}
ksn_input_scope pocket_kasane_input_scope(bool host_priority) {
    return state?ksn_view_host_route(&state->host,host_priority)
                :(host_priority?KSN_INPUT_HOST:KSN_INPUT_APP);
}
