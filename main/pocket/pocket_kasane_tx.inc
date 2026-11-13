/* Transaction commands, ref handles, animation and instance/modal methods
 * (wrap_direct through the MUTATOR instantiations): the guest-visible part of
 * a Kasane turn. It is an .inc, not a .c, because pocket_kasane.c is one
 * translation unit: mutate and the static parsers it calls must stay in one
 * object compiled -O2 so inlining and the IRAM placement named in
 * main/vm/helper_iram8.lf (`pocket_kasane:`) do not change. Function order is
 * the original order on purpose. Included only from pocket_kasane.c. */
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

static JSValue expose_ref(JSContext *ctx,ksn_tx tx,ksn_ref ref) {
    if(!ref_proto(ctx)) {
        ksn_view_cancel(view(),tx);discard_candidates(tx);return JS_EXCEPTION;
    }
    ref_slot *slot=claim_ref(ctx,ref,tx);
    if(!slot) { ksn_view_cancel(view(),tx); discard_candidates(tx); return JS_EXCEPTION; }
    JSValue object=wrap_direct(ctx,ref_class,slot->handle);
    if(JS_IsException(object)) {
        *slot=(ref_slot){0};ksn_view_cancel(view(),tx);discard_candidates(tx);
    }
    return object;
}

static JSValue js_tx_primitive(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv,ksn_kind kind,const char *op) {
    ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value) return JS_EXCEPTION;
    ksn_draw draw={0};
    if(!parse_primitive(ctx,argc?argv[0]:JS_UNDEFINED,kind,&draw,op)) return JS_EXCEPTION;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    if(result==KSN_OK)return expose_ref(ctx,tx,ref);
    if(result==KSN_LIMIT)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                "scene command limit exceeded",false,POCKET_OUTCOME_NOT_APPLIED);
    if(result==KSN_INVALID&&(kind==KSN_ROUND_RECT||kind==KSN_GRADIENT))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "radius must be <= 8 and fit within half the shape width and height",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    return throw_result(ctx,result,op);
}

/* Bound conversion before QuickJS allocates UTF-8 storage (at most 384 bytes
 * plus its string header). Core rejects lone surrogates and control scalars. */
static const char *text_string(JSContext *ctx,JSValueConst value,uint16_t *bytes,const char *op){
    if(!JS_IsString(value)){throw_result(ctx,KSN_INVALID,op);return NULL;}
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0)return NULL;
    if(units>128){throw_result(ctx,KSN_LIMIT,op);return NULL;}
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,value);
    if(!text)return NULL;
    if(length>128){JS_FreeCString(ctx,text);throw_result(ctx,KSN_LIMIT,op);return NULL;}
    *bytes=(uint16_t)length;return text;
}

static bool property_u16(JSContext *ctx,JSValueConst spec,const char *key,uint16_t fallback,
                         uint16_t *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);
    if(JS_IsException(value))return false;
    double n=fallback;bool ok=JS_IsUndefined(value)||number_in(ctx,value,0,65535,&n);
    JS_FreeValue(ctx,value);
    if(!ok){throw_result(ctx,KSN_INVALID,op);return false;}
    *out=(uint16_t)n;return true;
}

static bool parse_angle(JSContext *ctx,JSValueConst value,int32_t *out,const char *op){
    double degrees;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&degrees,value)<0||!isfinite(degrees)||degrees<-32768||degrees>32767){
        throw_result(ctx,KSN_INVALID,op);return false;
    }
    *out=(int32_t)lround(degrees*(1024.0/360.0));return true;
}
static bool parse_rotation(JSContext *ctx,JSValueConst value,uint16_t *out,const char *op){
    int32_t turns;if(!parse_angle(ctx,value,&turns,op))return false;
    *out=(uint16_t)((turns%1024+1024)%1024);return true;
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="kasane.resource";
    const char *name=argc?bounded_cstring(ctx,argv[0],31):NULL;
    if(!name)return throw_result(ctx,KSN_INVALID,op);
    const pocket_app_image_asset *asset=pocket_app_image_lookup(name);
    JS_FreeCString(ctx,name);
    if(!asset)return throw_result(ctx,KSN_UNSUPPORTED,op);
    if(state&&state->building.value)return throw_result(ctx,KSN_BUSY,op);
    /* Finish every fallible JS allocation before reserving a native resource. */
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",asset->width},{"height",asset->height},
        {"variants",asset->variants},{"frames",asset->frames}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
    }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    unsigned index=0;
    for(;index<4;index++)if(state->images[index].asset==asset)break;
    if(index==4){
        for(index=0;index<4&&state->images[index].asset;index++);
        if(index==4){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_LIMIT,op);}
        ksn_image_port port;ksn_result opened=asset->open(&port);
        if(opened!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,opened,op);}
        if(port.width!=asset->width||port.height!=asset->height||
           port.variants!=asset->variants||port.frames!=asset->frames){
            JS_FreeValue(ctx,object);return throw_result(ctx,KSN_INVALID,op);
        }
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),&port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->images[index].asset=asset;state->images[index].resource=resource;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->images[index].resource.value);return object;
}

JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){
    const char *op="kasane.shiki.resource";
    if(surface>=2||(state&&state->building.value))return throw_result(ctx,KSN_BUSY,op);
    /* Complete all fallible JS work before changing native display mode. */
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",KSN_PROC_W},{"height",KSN_PROC_H},{"variants",1},{"frames",1}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
        }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    if(!state->proc_resource[surface].value){
        ksn_image_port port;pocket_proc_image_port_at(&port,surface);
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),&port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->proc_resource[surface]=resource;
        pocket_proc_image_mode_at(surface);
        if(surface==0){
            /* Existing backdrop content is removed when image mode is selected. */
            ksn_runtime_invalidate();
        }
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->proc_resource[surface].value);
    return object;
}
JSValue pocket_kasane_proc_resource(JSContext *ctx){
    return pocket_kasane_proc_resource_at(ctx,0);
}

JSValue pocket_kasane_grid_resource(JSContext *ctx,unsigned slot,
                                    const ksn_image_port *port){
    const char *op="kasane.grid.resource";
    if(slot>=POCKET_GRID_MAX_SLOTS||!port||!port->width||!port->height||
       (state&&state->building.value))return throw_result(ctx,KSN_INVALID,op);
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",port->width},{"height",port->height},
        {"variants",1},{"frames",1}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
        }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    if(!state->grid_resources[slot].value){
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->grid_resources[slot]=resource;
        state->grid_width[slot]=port->width;
        state->grid_height[slot]=port->height;
    }else if(state->grid_width[slot]!=port->width||
             state->grid_height[slot]!=port->height){
        JS_FreeValue(ctx,object);return throw_result(ctx,KSN_INVALID,op);
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->grid_resources[slot].value);
    return object;
}
JSValue pocket_kasane_video_resource(JSContext *ctx,const ksn_image_port *port){
    const char *op="kasane.video.open";
    if(!port||!port->width||!port->height||
       (state&&state->building.value))return throw_result(ctx,KSN_INVALID,op);
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",port->width},{"height",port->height},
        {"variants",1},{"frames",1}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
        }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    if(!state->video_resource.value){
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->video_resource=resource;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->video_resource.value);
    return object;
}
JSValue pocket_kasane_pixel_resource(JSContext *ctx,const ksn_image_port *port){
    const char *op="kasane.pixel.open";
    if(!port||!port->width||!port->height||
       (state&&state->building.value))return throw_result(ctx,KSN_INVALID,op);
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",port->width},{"height",port->height},
        {"variants",1},{"frames",1}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
        }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    if(!state->pixel_resource.value){
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->pixel_resource=resource;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->pixel_resource.value);
    return object;
}
void pocket_kasane_pixel_invalidate(void){
    if(state&&state->pixel_resource.value){
        ksn_runtime_invalidate_image(state->pixel_resource);
        pocket_grid_source_invalidated(state->pixel_resource.value);
    }
}
bool pocket_kasane_pixel_can_stage(void){
    return state&&state->pixel_resource.value&&!state->building.value&&
           !state->submitted.value&&!ksn_runtime_has_submission();
}
bool pocket_kasane_video_can_select(void){
    return state&&state->video_resource.value&&!state->building.value&&
           !state->submitted.value&&!ksn_runtime_has_submission();
}
void pocket_kasane_video_invalidate(void){
    if(state&&state->video_resource.value){
        ksn_runtime_invalidate_image(state->video_resource);
        pocket_grid_source_invalidated(state->video_resource.value);
    }
}
void pocket_kasane_grid_invalidate(unsigned slot){
    if(state&&slot<POCKET_GRID_MAX_SLOTS&&state->grid_resources[slot].value){
        ksn_runtime_invalidate_image(state->grid_resources[slot]);
        pocket_grid_source_invalidated(state->grid_resources[slot].value);
    }
}
bool pocket_kasane_grid_source_port(JSContext *ctx,JSValueConst object,
                                    ksn_image_port *out,uint32_t *id){
    (void)ctx;
    if(!out||!id||!state||!JS_IsObject(object))return false;
    uint32_t value=opaque_value(object,image_class);
    if(!value)return false;
    if(ksn_view_host_image_port(view(),(ksn_resource){value},out)!=KSN_OK)
        return false;
    *id=value;
    return true;
}

static JSValue js_tx_image(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.image";ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value)return JS_EXCEPTION;
    JSValueConst spec=argc?argv[0]:JS_UNDEFINED;ksn_draw draw={0};
    if(!parse_draw_base(ctx,spec,&draw,op))return JS_EXCEPTION;
    draw.kind=KSN_IMAGE;
    JSValue resource=JS_GetPropertyStr(ctx,spec,"resource");
    if(JS_IsException(resource))return resource;
    draw.data.image.resource.value=opaque_value(resource,image_class);JS_FreeValue(ctx,resource);
    if(!draw.data.image.resource.value)return throw_result(ctx,KSN_INVALID,op);
    if(!property_u16(ctx,spec,"variant",0,&draw.data.image.variant,op)||
       !property_u16(ctx,spec,"frame",0,&draw.data.image.frame,op)||
       !property_u16(ctx,spec,"sourceX",0,&draw.data.image.source_x,op)||
       !property_u16(ctx,spec,"sourceY",0,&draw.data.image.source_y,op))return JS_EXCEPTION;
    JSValue scale=JS_GetPropertyStr(ctx,spec,"scale");
    if(JS_IsException(scale))return scale;
    bool stretch=JS_IsUndefined(scale);
    double n=1;bool ok=stretch||(JS_IsNumber(scale)&&JS_ToFloat64(ctx,&n,scale)==0);
    JS_FreeValue(ctx,scale);
    if(!ok||(n!=0.5&&n!=1&&n!=2))return throw_result(ctx,KSN_INVALID,op);
    draw.data.image.scale=stretch?KSN_IMAGE_STRETCH:n==0.5?KSN_IMAGE_HALF:n==2?KSN_IMAGE_2X:KSN_IMAGE_1X;
    /* Source extents stay fixed when setRect changes the destination. */
    ksn_image_port source_port;
    ksn_result port_result=ksn_view_host_image_port(view(),
        draw.data.image.resource,&source_port);
    if(port_result!=KSN_OK)return throw_result(ctx,port_result,op);
    uint16_t source_w=source_port.width,source_h=source_port.height;
    uint16_t width=draw.data.image.source_x<source_w?
                   source_w-draw.data.image.source_x:0;
    uint16_t height=draw.data.image.source_y<source_h?
                    source_h-draw.data.image.source_y:0;
    if(!property_u16(ctx,spec,"sourceWidth",width,&draw.data.image.source_width,op)||
       !property_u16(ctx,spec,"sourceHeight",height,&draw.data.image.source_height,op))return JS_EXCEPTION;
    JSValue rotation=JS_GetPropertyStr(ctx,spec,"rotation");
    if(JS_IsException(rotation))return rotation;
    ok=JS_IsUndefined(rotation)||parse_rotation(ctx,rotation,&draw.data.image.rotation,op);
    JS_FreeValue(ctx,rotation);if(!ok)return JS_EXCEPTION;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    return result==KSN_OK?expose_ref(ctx,tx,ref):throw_result(ctx,result,op);
}

static JSValue js_tx_text(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.text";ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value)return JS_EXCEPTION;
    JSValueConst spec=argc?argv[0]:JS_UNDEFINED;ksn_draw draw={0};
    if(!parse_draw_base(ctx,spec,&draw,op)||
       !property_color(ctx,spec,"color",&draw.data.text.color,op))return JS_EXCEPTION;
    draw.kind=KSN_TEXT;draw.data.text.font=KSN_BODY;
    JSValue font=JS_GetPropertyStr(ctx,spec,"font");
    if(JS_IsException(font))return font;
    if(!JS_IsUndefined(font)){
        uint16_t bytes=0;const char *name=text_string(ctx,font,&bytes,op);
        if(!name){JS_FreeValue(ctx,font);return JS_EXCEPTION;}
        bool valid=true;
        if(bytes==7&&!memcmp(name,"caption",7))draw.data.text.font=KSN_CAPTION;
        else if(bytes==4&&!memcmp(name,"body",4))draw.data.text.font=KSN_BODY;
        else if(bytes==7&&!memcmp(name,"display",7))draw.data.text.font=KSN_DISPLAY;
        else valid=false;
        JS_FreeCString(ctx,name);JS_FreeValue(ctx,font);
        if(!valid)return throw_result(ctx,KSN_INVALID,op);
    }else JS_FreeValue(ctx,font);
    JSValue value=JS_GetPropertyStr(ctx,spec,"text");
    if(JS_IsException(value))return value;
    const char *text=text_string(ctx,value,&draw.data.text.bytes,op);JS_FreeValue(ctx,value);
    if(!text)return JS_EXCEPTION;
    draw.data.text.utf8=text;
    uint8_t capacity=draw.data.text.bytes?draw.data.text.bytes:1;
    if(!property_u8(ctx,spec,"capacity",capacity,&capacity,op)){JS_FreeCString(ctx,text);return JS_EXCEPTION;}
    draw.data.text.capacity=capacity;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    JS_FreeCString(ctx,text);
    return result==KSN_OK?expose_ref(ctx,tx,ref):throw_result(ctx,result,op);
}

#define PRIMITIVE(name,kind,op) \
static JSValue name(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return js_tx_primitive(ctx,self,argc,argv,kind,op); \
}
PRIMITIVE(js_tx_rect,KSN_RECT,"kasane.rect")
PRIMITIVE(js_tx_round_rect,KSN_ROUND_RECT,"kasane.roundRect")
PRIMITIVE(js_tx_stroke_rect,KSN_STROKE,"kasane.strokeRect")
PRIMITIVE(js_tx_gradient,KSN_GRADIENT,"kasane.gradient")
#undef PRIMITIVE

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
    if(!instance_proto(ctx)) return JS_EXCEPTION;
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
        if(property==KSN_SET_CLIP)viewport_clip(&change.value.rect);
    } else if(property==KSN_SET_COLOR) {
        if(argc<2||!parse_u32(ctx,argv[1],&change.value.color))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "color must be an RRGGBBAA uint32",false,NULL);
    } else if(property==KSN_SET_TEXT) {
        const char *text=text_string(ctx,argc>1?argv[1]:JS_UNDEFINED,&change.value.text.bytes,op);
        if(!text)return JS_EXCEPTION;
        change.value.text.utf8=text;
        ksn_result result=ksn_view_change(view(),tx,slot->ref,&change);
        JS_FreeCString(ctx,text);
        return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
    } else if(property==KSN_SET_ROTATION) {
        if(argc<2)return throw_result(ctx,KSN_INVALID,op);
        if(!parse_rotation(ctx,argv[1],&change.value.rotation,op))return JS_EXCEPTION;
    } else if(property==KSN_SET_IMAGE_FRAME) {
        double variant,frame;
        if(argc<3||!number_in(ctx,argv[1],0,65535,&variant)||!number_in(ctx,argv[2],0,65535,&frame))
            return throw_result(ctx,KSN_INVALID,op);
        change.value.image.variant=(uint16_t)variant;change.value.image.frame=(uint16_t)frame;
    } else if(property==KSN_SET_REVEAL) {
        double n;
        if(argc<2||!number_in(ctx,argv[1],0,128,&n))return throw_result(ctx,KSN_INVALID,op);
        change.value.reveal=(uint16_t)n;
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
REF_SETTER(js_ref_text,KSN_SET_TEXT,"kasane.ref.setText")
REF_SETTER(js_ref_reveal,KSN_SET_REVEAL,"kasane.ref.setReveal")
REF_SETTER(js_ref_image,KSN_SET_IMAGE_FRAME,"kasane.ref.setImageFrame")
REF_SETTER(js_ref_rotation,KSN_SET_ROTATION,"kasane.ref.setRotation")

static bool parse_pose(JSContext *ctx,JSValueConst spec,const char *key,ksn_pose *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);
    if(JS_IsException(value))return false;
    if(!JS_IsObject(value)){JS_FreeValue(ctx,value);throw_result(ctx,KSN_INVALID,op);return false;}
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&parse_rect(ctx,bounds,&out->bounds,op);JS_FreeValue(ctx,bounds);
    if(ok){JSValue angle=JS_GetPropertyStr(ctx,value,"rotation");
        ok=!JS_IsException(angle)&&(JS_IsUndefined(angle)||parse_angle(ctx,angle,&out->rotation,op));
        JS_FreeValue(ctx,angle);}
    JS_FreeValue(ctx,value);return ok;
}
static bool parse_choice(JSContext *ctx,JSValueConst spec,const char *key,const char *const *names,unsigned count,
                          unsigned *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){*out=0;JS_FreeValue(ctx,value);return true;}
    uint16_t bytes;const char *s=text_string(ctx,value,&bytes,op);JS_FreeValue(ctx,value);if(!s)return false;
    unsigned i=0;while(i<count&&(strlen(names[i])!=bytes||memcmp(names[i],s,bytes)))i++;
    JS_FreeCString(ctx,s);if(i==count){throw_result(ctx,KSN_INVALID,op);return false;}*out=i;return true;
}
static JSValue js_ref_animate(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.ref.animate";ref_slot *slot=ref_from(ctx,self,op);if(!slot)return JS_EXCEPTION;
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};if(!tx.value)return JS_EXCEPTION;
    if(argc<2||!JS_IsObject(argv[1]))return throw_result(ctx,KSN_INVALID,op);
    if(slot->status==REF_CANDIDATE&&slot->ticket.value!=tx.value)return throw_result(ctx,KSN_STALE,op);
    ksn_motion motion={.first=slot->ref,.count=1,.property=KSN_TRANSFORM};
    if(!parse_pose(ctx,argv[1],"from",&motion.from.pose,op)||!parse_pose(ctx,argv[1],"to",&motion.to.pose,op))return JS_EXCEPTION;
    JSValue duration=JS_GetPropertyStr(ctx,argv[1],"durationMs");if(JS_IsException(duration))return duration;
    double n;bool ok=number_in(ctx,duration,1,86400000,&n);JS_FreeValue(ctx,duration);
    if(!ok)return throw_result(ctx,KSN_INVALID,op);
    motion.duration_ms=(uint32_t)n;
    static const char *const easings[]={"linear","ease-out","ease-in-out","step"};
    static const char *const repeats[]={"once","loop","ping-pong"};unsigned easing,repeat;
    if(!parse_choice(ctx,argv[1],"easing",easings,4,&easing,op)||!parse_choice(ctx,argv[1],"repeat",repeats,3,&repeat,op))return JS_EXCEPTION;
    motion.easing=(ksn_easing)easing;motion.repeat=(ksn_repeat)repeat;
    if(!animation_proto(ctx))return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,animation_class);if(JS_IsException(object))return object;
    ksn_animation id;ksn_result result=ksn_runtime_animate(view(),tx,&motion,&id);
    if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
    JS_SetOpaque(object,(void *)(uintptr_t)id.value);return object;
}
static JSValue animation_stop(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,bool finish){
    const char *op=finish?"kasane.animation.finish":"kasane.animation.stop";
    ksn_animation id={opaque_value(self,animation_class)};if(!id.value)return throw_result(ctx,KSN_STALE,op);
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};if(!tx.value)return JS_EXCEPTION;
    ksn_result result=ksn_view_stop_animation(view(),tx,id,finish);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
}
static JSValue js_animation_stop(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){return animation_stop(ctx,self,argc,argv,false);}
static JSValue js_animation_finish(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){return animation_stop(ctx,self,argc,argv,true);}
static JSValue js_animation_poll(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;static const char *const names[]={"discarded","pending","running","finished","stopped"};
    ksn_animation_status status=ksn_view_poll_animation(view(),(ksn_animation){opaque_value(self,animation_class)});
    return JS_NewString(ctx,names[status]);
}
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
    if(overlay_profile)return throw_result(ctx,KSN_UNSUPPORTED,"kasane.modal.open");
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
       !view()||view()->host->builder.value!=tx.value)
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
MUTATOR(js_tx_round_rect,tx_class,false,"kasane.roundRect")
MUTATOR(js_tx_stroke_rect,tx_class,false,"kasane.strokeRect")
MUTATOR(js_tx_gradient,tx_class,false,"kasane.gradient")
MUTATOR(js_tx_text,tx_class,false,"kasane.text")
MUTATOR(js_tx_image,tx_class,false,"kasane.image")
MUTATOR(js_tx_group,tx_class,false,"kasane.group")
MUTATOR(js_tx_instantiate,tx_class,false,"kasane.instantiate")
MUTATOR(js_ref_rect,tx_class,true,"kasane.ref.setRect")
MUTATOR(js_ref_clip,tx_class,true,"kasane.ref.setClip")
MUTATOR(js_ref_color,tx_class,true,"kasane.ref.setColor")
MUTATOR(js_ref_visible,tx_class,true,"kasane.ref.setVisible")
MUTATOR(js_ref_text,tx_class,true,"kasane.ref.setText")
MUTATOR(js_ref_reveal,tx_class,true,"kasane.ref.setReveal")
MUTATOR(js_ref_image,tx_class,true,"kasane.ref.setImageFrame")
MUTATOR(js_ref_rotation,tx_class,true,"kasane.ref.setRotation")
MUTATOR(js_ref_animate,tx_class,true,"kasane.ref.animate")
MUTATOR(js_animation_stop,tx_class,true,"kasane.animation.stop")
MUTATOR(js_animation_finish,tx_class,true,"kasane.animation.finish")
MUTATOR(js_instance_place,tx_class,true,"kasane.instance.place")
MUTATOR(js_instance_visible,tx_class,true,"kasane.instance.setVisible")
MUTATOR(js_modal_open,modal_class,false,"kasane.modal.open")
MUTATOR(js_modal_close,modal_class,false,"kasane.modal.close")
#undef MUTATOR

