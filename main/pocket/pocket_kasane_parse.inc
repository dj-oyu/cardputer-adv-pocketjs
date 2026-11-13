/* Argument validation and integer/rect/colour parsing shared by every
 * pocket.kasane entry point (parse_i16, parse_rect_mode, tx_from, ...), up to
 * the proto forward declarations. It is an .inc, not a .c, because
 * pocket_kasane.c is one translation unit: its static helpers must inline into
 * the per-turn mutators, the file is compiled -O2 (main/CMakeLists.txt), and
 * main/vm/helper_iram8.lf places mutate, parse_i16 and parse_rect_mode by the
 * object name `pocket_kasane:`. Separate objects would change inlining,
 * placement and that name. Function order is the original order on purpose
 * (order decides placement). Included only from pocket_kasane.c. */

static bool parse_i16(JSContext *ctx, JSValueConst value, int16_t *out) {
    double n;
    if(JS_VALUE_GET_TAG(value)==JS_TAG_INT){
        const int32_t i=JS_VALUE_GET_INT(value);
        if(i<INT16_MIN||i>INT16_MAX) return false;
        *out=(int16_t)i;
        return true;
    }
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)) return false;
    n=round(n);
    if(n<INT16_MIN||n>INT16_MAX)return false;
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

static bool parse_rect_mode(JSContext *ctx, JSValueConst value, ksn_rect *out,
                            const char *op,bool translate) {
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
                             "rounded coordinates must fit int16",false,NULL);
            return false;
        }
    }
    *out=(ksn_rect){coords[0],coords[1],coords[2],coords[3]};
    if(out->x0>out->x1||out->y0>out->y1) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle edges are reversed",false,NULL);
        return false;
    }
    if(translate)viewport_rect(out);
    return true;
}

static bool parse_rect(JSContext *ctx, JSValueConst value, ksn_rect *out,
                       const char *op) {
    return parse_rect_mode(ctx,value,out,op,true);
}

/* The same rectangle as four arguments (ref.setRect/setClip(tx, x0, y0, x1,
 * y1)): the same rounding, int16 range and edge rule as the array form. */
static bool parse_rect_ints(JSContext *ctx, JSValueConst *argv, ksn_rect *out,
                            const char *op) {
    int16_t c[4];
    for(unsigned i=0;i<4;i++)
        if(!parse_i16(ctx,argv[i],&c[i])) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "rounded coordinates must fit int16",false,NULL);
            return false;
        }
    *out=(ksn_rect){c[0],c[1],c[2],c[3]};
    if(out->x0>out->x1||out->y0>out->y1) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle edges are reversed",false,NULL);
        return false;
    }
    viewport_rect(out);
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

static bool property_local_rect(JSContext *ctx,JSValueConst object,const char *name,
                                ksn_rect fallback,ksn_rect *out,const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){*out=fallback;JS_FreeValue(ctx,value);return true;}
    bool ok=parse_rect_mode(ctx,value,out,op,false);
    JS_FreeValue(ctx,value);return ok;
}

static bool property_u8(JSContext *ctx, JSValueConst object, const char *name,
                        uint8_t fallback, uint8_t *out, const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { *out=fallback; JS_FreeValue(ctx,value); return true; }
    bool ok=parse_u8(ctx,value,out);
    JS_FreeValue(ctx,value);
    if(!ok) pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "value must be an integer from 0 to 255",false,NULL);
    return ok;
}

static bool parse_draw_base(JSContext *ctx, JSValueConst value, ksn_draw *out,
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
    viewport_clip(&out->clip);
    if(!property_u8(ctx,value,"opacity",255,&out->opacity,op)) return false;
    return true;
}

static bool property_color(JSContext *ctx,JSValueConst value,const char *name,
                           ksn_rgba *out,const char *op) {
    JSValue color=JS_GetPropertyStr(ctx,value,name);
    if(JS_IsException(color)) return false;
    bool ok=parse_u32(ctx,color,out);
    JS_FreeValue(ctx,color);
    if(!ok) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "color must be an RRGGBBAA uint32",false,NULL);
        return false;
    }
    return true;
}

static bool parse_draw(JSContext *ctx, JSValueConst value, ksn_draw *out,
                       const char *op) {
    return parse_draw_base(ctx,value,out,op)&&
           property_color(ctx,value,"color",&out->data.shape.color,op);
}

/* Cache templates live in their own local coordinate space. Translating or
 * clipping them here destroys pixels that a later placement moves into the
 * viewport. The placement supplies the viewport origin and final clip. */
static bool parse_cache_draw(JSContext *ctx,JSValueConst value,ksn_draw *out,
                             const char *op) {
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be an object",false,NULL);
        return false;
    }
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&
        parse_rect_mode(ctx,bounds,&out->bounds,op,false);
    JS_FreeValue(ctx,bounds);
    if(!ok)return false;
    out->kind=KSN_RECT;out->clip=out->bounds;
    if(!property_local_rect(ctx,value,"clip",out->bounds,&out->clip,op)||
       !property_u8(ctx,value,"opacity",255,&out->opacity,op))return false;
    return property_color(ctx,value,"color",&out->data.shape.color,op);
}

static bool parse_primitive(JSContext *ctx,JSValueConst value,ksn_kind kind,
                            ksn_draw *out,const char *op) {
    if(kind!=KSN_GRADIENT) {
        if(!parse_draw(ctx,value,out,op))return false;
        out->kind=kind;
        if(kind==KSN_ROUND_RECT)
            return property_u8(ctx,value,"radius",0,&out->data.shape.radius,op);
        if(kind==KSN_STROKE)
            return property_u8(ctx,value,"width",1,&out->data.shape.width,op);
        return true;
    }
    if(!parse_draw_base(ctx,value,out,op))return false;
    out->kind=kind;
    if(!property_color(ctx,value,"from",&out->data.gradient.from,op)||
       !property_color(ctx,value,"to",&out->data.gradient.to,op)||
       !property_u8(ctx,value,"radius",0,&out->data.gradient.radius,op))return false;
    JSValue axis=JS_GetPropertyStr(ctx,value,"axis");
    if(JS_IsException(axis))return false;
    bool valid=JS_IsUndefined(axis);
    out->data.gradient.axis=1;
    if(JS_IsString(axis)) {
        const char *name=JS_ToCString(ctx,axis);
        if(!name){JS_FreeValue(ctx,axis);return false;}
        valid=!strcmp(name,"x")||!strcmp(name,"y");
        out->data.gradient.axis=!strcmp(name,"y");
        JS_FreeCString(ctx,name);
    }
    JS_FreeValue(ctx,axis);
    if(!valid){throw_result(ctx,KSN_INVALID,op);return false;}
    JSValue dither=JS_GetPropertyStr(ctx,value,"dither");
    if(JS_IsException(dither))return false;
    valid=JS_IsUndefined(dither)||JS_IsBool(dither);
    out->data.gradient.dither=JS_IsBool(dither)&&JS_ToBool(ctx,dither);
    JS_FreeValue(ctx,dither);
    if(!valid)throw_result(ctx,KSN_INVALID,op);
    return valid;
}

static bool parse_placement(JSContext *ctx, JSValueConst value, ksn_placement *out,
                            const char *op) {
    *out=(ksn_placement){.x=viewport.x0,.y=viewport.y0,.clip=viewport,
                         .opacity=255,.visible=true};
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
        int16_t local_x=0,local_y=0;
        bool ok=parse_i16(ctx,x,&local_x)&&parse_i16(ctx,y,&local_y);
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,offset);
        int32_t absolute_x=(int32_t)local_x+viewport.x0;
        int32_t absolute_y=(int32_t)local_y+viewport.y0;
        if(!ok||absolute_x<INT16_MIN||absolute_x>INT16_MAX||
           absolute_y<INT16_MIN||absolute_y>INT16_MAX) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "offset coordinates must be int16 values",false,NULL);
            return false;
        }
        out->x=(int16_t)absolute_x;out->y=(int16_t)absolute_y;
    } else JS_FreeValue(ctx,offset);
    if(!property_rect(ctx,value,"clip",viewport,&out->clip,op)) return false;
    viewport_clip(&out->clip);
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

/* Built on first use: native mount apps need none of the low-level drawing
 * wrapper methods. Defined after the method tables. */
static bool tx_proto(JSContext *ctx);
static bool modal_proto(JSContext *ctx);
static bool ref_proto(JSContext *ctx);
static bool instance_proto(JSContext *ctx);
static bool animation_proto(JSContext *ctx);

