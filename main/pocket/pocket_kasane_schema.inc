/* js_create_scene, the schema methods, the runtime descriptor compiler and the
 * presenter mount: code that mostly runs at registration time. It is an .inc,
 * not a .c, because pocket_kasane.c is one translation unit; a separate
 * (possibly -Os) object would shift code placement and break the
 * `pocket_kasane:` object name in main/vm/helper_iram8.lf, and a placement
 * change cannot be compared against the 15% instruction-cache alignment noise
 * (docs/kasane/api-reorg.md section 4.2). Function order is the original order
 * on purpose. Included only from pocket_kasane.c. */
/* Returns {invalidate, flush} as own data properties holding bound functions,
 * the shape the JS factory's object literal had, so detached calls still work.
 * The returned object is also the state's holder (its class inherits
 * Object.prototype): each bound function keeps it alive through its data slot,
 * a cycle the collector breaks through scene_mark. One object fewer than a
 * separate hidden holder. */
static JSValue js_create_scene(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    JSValueConst options=argc?argv[0]:JS_UNDEFINED;
    JSValue build=JS_UNDEFINED,patch=JS_UNDEFINED;
    bool valid=false;
    if(JS_ToBool(ctx,options)) {
        build=JS_GetPropertyStr(ctx,options,"build");
        if(JS_IsException(build)) return build;
        if(JS_IsFunction(ctx,build)) {
            patch=JS_GetPropertyStr(ctx,options,"patch");
            if(JS_IsException(patch)) { JS_FreeValue(ctx,build);return patch; }
            valid=JS_IsUndefined(patch)||JS_IsFunction(ctx,patch);
        }
    }
    if(!valid) {
        JS_FreeValue(ctx,build);JS_FreeValue(ctx,patch);
        return JS_ThrowTypeError(ctx,"createScene requires build and optional patch callbacks");
    }
    JSValue out=JS_NewObjectClass(ctx,scene_class);
    kasane_scene *scene=JS_IsException(out)?NULL:js_mallocz(ctx,sizeof(*scene));
    if(!scene) {
        JS_FreeValue(ctx,out);JS_FreeValue(ctx,build);JS_FreeValue(ctx,patch);
        return JS_EXCEPTION;
    }
    *scene=(kasane_scene){.build=build,.patch=patch,.refs=JS_NULL,.candidate=JS_NULL,
                          .model=JS_UNDEFINED,.dirty=true,.rebuild=true};
    JS_SetOpaque(out,scene);
    JSValueConst data[]={out};
    JSValue invalidate=JS_NewCFunctionData2(ctx,scene_invalidate,"invalidate",1,0,1,data);
    if(JS_IsException(invalidate)||
       JS_DefinePropertyValueStr(ctx,out,"invalidate",invalidate,JS_PROP_C_W_E)<0) goto fail;
    JSValue flush=JS_NewCFunctionData2(ctx,scene_flush,"flush",1,0,1,data);
    if(JS_IsException(flush)||
       JS_DefinePropertyValueStr(ctx,out,"flush",flush,JS_PROP_C_W_E)<0) goto fail;
    return out;
fail:
    JS_FreeValue(ctx,out);
    return JS_EXCEPTION;
}

static schema_state *schema_owner(JSValueConst self){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,schema_class);
    return handle&&state&&state->schema&&state->schema->handle==handle?
           state->schema:NULL;
}
static void schema_free_pending_text(JSContext *ctx,const char *const *texts,
                                     const uint8_t *touched,unsigned count){
    for(unsigned n=0;n<count;n++){
        unsigned i=touched[n];
        if(texts[i])JS_FreeCString(ctx,texts[i]);
    }
}
static JSValue js_schema_set(JSContext *ctx,schema_state *s,JSValueConst model){
    const char *op="kasane.view.set";
    if(!JS_IsObject(model)||JS_IsArray(model))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "slots must be an object",false,NULL);
    ksn_schema_value candidate[KSN_SCHEMA_MAX_SLOTS];
    /* JS_ToCStringLen owns a live JSString reference until FreeCString.
     * Borrow those immutable UTF-8 bytes through preflight, then make the
     * single schema-owned copy only for changed slots. */
    const char *pending_text[KSN_SCHEMA_MAX_SLOTS]={0};
    uint8_t touched[KSN_SCHEMA_MAX_SLOTS];
    unsigned touched_count=0;
    memcpy(candidate,s->values,s->definition->slot_count*sizeof(*candidate));
    ksn_p0_probe_copy(KSN_P0_ADAPTER_TEMP,s->definition->slot_count*sizeof(*candidate));
    JSPropertyEnum *props=NULL;uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,model,
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY))return JS_EXCEPTION;
    bool ok=true;
    for(uint32_t p=0;p<count&&ok;p++){
        const char *key=JS_AtomToCString(ctx,props[p].atom);
        if(!key){ok=false;break;}
        unsigned i=0;
        for(;i<s->definition->slot_count;i++)
            if(strcmp(key,s->definition->slots[i].name)==0)break;
        JS_FreeCString(ctx,key);
        if(i==s->definition->slot_count){
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "unknown display slot",false,NULL);ok=false;break;
        }
        /* Own property names are unique; only these slots can differ from
         * the copied candidate. Compare after all getters have run, since a
         * getter may reenter view.set and change the live baseline. */
        touched[touched_count++]=(uint8_t)i;
        JSValue value=JS_GetProperty(ctx,model,props[p].atom);
        if(JS_IsException(value)){ok=false;break;}
        const ksn_schema_slot *slot=&s->definition->slots[i];
        if(slot->type==KSN_SLOT_TEXT){
            if(!JS_IsString(value))ok=false;
            else{
                int64_t units=0;
                if(JS_GetLength(ctx,value,&units)<0||units>slot->capacity)ok=false;
                else{
                    size_t length=0;const char *text=JS_ToCStringLen(ctx,&length,value);
                    if(text)ksn_p0_probe_copy(KSN_P0_UTF8_REQUEST,length);
                    if(!text)ok=false;
                    else if(length>slot->capacity)ok=false;
                    else{
                        pending_text[i]=text;
                        candidate[i].data.text=(ksn_schema_text){pending_text[i],(uint16_t)length};
                    }
                    if(text&&!pending_text[i])JS_FreeCString(ctx,text);
                }
            }
        }else if(slot->type==KSN_SLOT_RECT){
            ok=parse_rect_mode(ctx,value,&candidate[i].data.rect,op,false);
        }else if(slot->type==KSN_SLOT_BOOL){
            ok=JS_IsBool(value);
            if(ok)candidate[i].data.boolean=JS_ToBool(ctx,value)!=0;
        }else if(slot->type==KSN_SLOT_COLOR){
            uint32_t color;ok=parse_u32(ctx,value,&color);
            if(ok)candidate[i].data.color=color;
        }else if(slot->type==KSN_SLOT_U16){
            double number;ok=number_in(ctx,value,0,
                           slot->maximum?slot->maximum:UINT16_MAX,&number);
            if(ok)candidate[i].data.number=(uint16_t)number;
        }else if(slot->type==KSN_SLOT_U32){
            uint32_t number;ok=parse_u32(ctx,value,&number)&&
                (!slot->maximum||number<=slot->maximum);
            if(ok)candidate[i].data.wide_number=number;
        }else if(slot->type==KSN_SLOT_RESOURCE){
            candidate[i].data.resource.value=opaque_value(value,image_class);
            ok=candidate[i].data.resource.value!=0;
        }
        JS_FreeValue(ctx,value);
        if(!ok&&!JS_HasException(ctx))
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "display slot type or value is invalid",false,NULL);
    }
    JS_FreePropertyEnum(ctx,props,count);
    if(!ok){
        schema_free_pending_text(ctx,pending_text,touched,touched_count);
        return JS_EXCEPTION;
    }
    uint32_t changed_slots=0;
    for(unsigned n=0;n<touched_count;n++){
        unsigned i=touched[n];
        bool differs;
        if(pending_text[i]){
            ksn_schema_text a=candidate[i].data.text,b=s->values[i].data.text;
            differs=a.bytes!=b.bytes||memcmp(a.utf8,b.utf8,a.bytes)!=0;
        }else differs=memcmp(&candidate[i].data,&s->values[i].data,
                            sizeof(candidate[i].data))!=0;
        if(differs)changed_slots|=(uint32_t)1u<<i;
    }
    if(!changed_slots){
        schema_free_pending_text(ctx,pending_text,touched,touched_count);
        return JS_UNDEFINED;
    }
    if(s->revision==UINT64_MAX){
        schema_free_pending_text(ctx,pending_text,touched,touched_count);
        return throw_result(ctx,KSN_LIMIT,op);
    }
    ksn_result check=ksn_schema_preflight_view(view(),s->definition,candidate,viewport);
    if(check!=KSN_OK){
        schema_free_pending_text(ctx,pending_text,touched,touched_count);
        return throw_result(ctx,check,op);
    }
    for(unsigned n=0;n<touched_count;n++){
        unsigned i=touched[n];
        if(!(changed_slots&((uint32_t)1u<<i)))continue;
        if(pending_text[i]){
            ksn_schema_text text=candidate[i].data.text;
            char *dest=(char *)s->values[i].data.text.utf8;
            memcpy(dest,text.utf8,text.bytes+1u);
            ksn_p0_probe_copy(KSN_P0_ADAPTER_OWNED,text.bytes+1u);
            s->values[i].data.text=(ksn_schema_text){dest,text.bytes};
            ksn_p0_probe_copy(KSN_P0_ADAPTER_SLOT_COMMIT,sizeof(ksn_schema_text));
        }else{
            s->values[i]=candidate[i];
            ksn_p0_probe_copy(KSN_P0_ADAPTER_SLOT_COMMIT,sizeof(candidate[i]));
        }
    }
    schema_free_pending_text(ctx,pending_text,touched,touched_count);
    s->revision++;
    s->pending_base_slots|=changed_slots;
    ksn_result submitted=schema_refresh(NULL);
#ifdef KASANE_P2_REPAIR_PROBE
    printf("KSN_P2_SET revision=%llu ticket=%u inflight=%08x pending=%08x\n",
           (unsigned long long)s->revision,(unsigned)s->session.ticket.value,
           (unsigned)s->session.inflight_dirty,(unsigned)s->session.pending_dirty);
#endif
    return submitted==KSN_OK||submitted==KSN_BUSY?JS_UNDEFINED:
           throw_result(ctx,submitted,op);
}
static JSValue js_schema_set_method(JSContext *ctx,JSValueConst self,
                                    int argc,JSValueConst *argv){
    schema_state *generic=schema_owner(self);
    if(!generic)return throw_result(ctx,KSN_STALE,"kasane.view.set");
    return js_schema_set(ctx,generic,argc?argv[0]:JS_UNDEFINED);
}
/* A numeric source index is local to the mount; a C-issued capability can
 * refer to a separate service registry. Neither path searches source names. */
static JSValue js_schema_bind_method(JSContext *ctx,JSValueConst self,
                                     int argc,JSValueConst *argv){
    const char *op="kasane.view.bind";
    schema_state *s=schema_owner(self);
    if(!s)return throw_result(ctx,KSN_STALE,op);
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    if(argc<2||!JS_IsObject(argv[1])||JS_IsArray(argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "source and slot bindings are required",false,NULL);
    bool local=JS_IsNumber(argv[0]);
    unsigned source_index=0;
    source_cap_record *offer=NULL;
    if(local){
        double number;
        if(!s->asset->source_count||!number_in(ctx,argv[0],0,
                  s->asset->source_count-1u,&number))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "source index is out of range",false,NULL);
        source_index=(unsigned)number;
    }else{
        uint32_t serial=(uint32_t)(uintptr_t)JS_GetOpaque(argv[0],source_cap_class);
        if(!serial||!source_caps)return throw_result(ctx,KSN_STALE,op);
        for(unsigned i=0;i<source_caps->count;i++)
            if(source_caps->records[i].serial==serial){
                offer=&source_caps->records[i];break;
            }
        if(!offer)return throw_result(ctx,KSN_STALE,op);
    }
    JSPropertyEnum *props=NULL;uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,argv[1],
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY))return JS_EXCEPTION;
    if(!count||count>KSN_SOURCE_MAX_FIELDS){
        JS_FreePropertyEnum(ctx,props,count);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "binding count is out of range",false,NULL);
    }
    ksn_source_binding bindings[KSN_SOURCE_MAX_FIELDS];
    bool valid=true;
    for(uint32_t p=0;p<count&&valid;p++){
        const char *name=JS_AtomToCString(ctx,props[p].atom);
        if(!name){valid=false;break;}
        unsigned slot=0;
        for(;slot<s->definition->slot_count;slot++)
            if(strcmp(name,s->definition->slots[slot].name)==0)break;
        JS_FreeCString(ctx,name);
        if(slot==s->definition->slot_count){valid=false;break;}
        JSValue value=JS_GetProperty(ctx,argv[1],props[p].atom);
        if(JS_IsException(value)){valid=false;break;}
        double field;
        valid=number_in(ctx,value,0,KSN_SOURCE_MAX_FIELDS-1u,&field);
        JS_FreeValue(ctx,value);
        if(valid)bindings[p]=(ksn_source_binding){(uint8_t)slot,(uint8_t)field};
    }
    JS_FreePropertyEnum(ctx,props,count);
    if(!valid)return JS_HasException(ctx)?JS_EXCEPTION:
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "slot name or field index is invalid",false,NULL);
    /* A binding getter can call view.set() and submit while we parse it. */
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    schema_sources *sources=schema_native(s);
    schema_externals *external=schema_external_state(s);
    schema_source *entry=local?&sources->entries[source_index]:NULL;
    int external_index=-1;
    if(!local&&external)for(unsigned i=0;i<external->count;i++)
        if(external->entries[i].registry==offer->registry&&
           external->entries[i].handle.index==offer->handle.index&&
           external->entries[i].handle.generation==offer->handle.generation){
            external_index=(int)i;break;
        }
    ksn_source_subscription *old=local?&entry->subscription:
        external_index>=0?&external->entries[external_index].subscription:NULL;
    ksn_source_subscription candidate;
    ksn_result r=ksn_source_subscribe(local?&sources->registry:offer->registry,
        local?entry->handle:offer->handle,s->handle,
        s->definition,bindings,(uint8_t)count,&candidate);
    if(r!=KSN_OK)return throw_result(ctx,r,op);
    if(old&&old->binding_count==count&&
       memcmp(old->bindings,bindings,count*sizeof(*bindings))==0)return JS_UNDEFINED;
    for(unsigned i=0;i<s->asset->source_count;i++)
        if((!local||i!=source_index)&&
           (sources->entries[i].subscription.bound_slots&candidate.bound_slots))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "another source owns a bound slot",false,NULL);
    if(external)for(unsigned i=0;i<external->count;i++)
        if((local||(int)i!=external_index)&&
           (external->entries[i].subscription.bound_slots&candidate.bound_slots))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "another source owns a bound slot",false,NULL);
    unsigned active_sources=external?external->count:0u;
    if(sources)for(unsigned i=0;i<s->asset->source_count;i++)
        if(sources->entries[i].subscription.binding_count)active_sources++;
    if((!old||!old->binding_count)&&
       active_sources>=KSN_SOURCE_MAX_REGISTERED)
        return throw_result(ctx,KSN_LIMIT,op);
    if(!local&&!external){
        external=calloc(1,sizeof(*external));
        if(!external)return throw_result(ctx,KSN_OOM,op);
        if(sources)sources->external=external;
        else s->source_state=external;
    }
    s->pending_base_slots|=(old?old->bound_slots:0)|candidate.bound_slots;
    if(local){
        entry->subscription=candidate;
        entry->pending_revision=0;
    }else{
        schema_external *target=&external->entries[external_index>=0?
                                                   (unsigned)external_index:
                                                   external->count++];
        *target=(schema_external){.registry=offer->registry,.handle=offer->handle,
                                  .subscription=candidate};
    }
    /* The next owner step reacquires the complete current snapshot. Only the
     * first external bind allocates its fixed-capacity subscription block;
     * a temporary producer failure leaves the current frame intact. */
    return JS_UNDEFINED;
}
static JSValue js_schema_unbind_method(JSContext *ctx,JSValueConst self,
                                       int argc,JSValueConst *argv){
    const char *op="kasane.view.unbind";
    schema_state *s=schema_owner(self);
    if(!s)return throw_result(ctx,KSN_STALE,op);
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    if(!argc)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "source is required",false,NULL);
    if(JS_IsNumber(argv[0])){
        double number;
        if(!s->asset->source_count||!number_in(ctx,argv[0],0,
                  s->asset->source_count-1u,&number))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "source index is out of range",false,NULL);
        schema_sources *sources=schema_native(s);
        schema_source *entry=&sources->entries[(unsigned)number];
        s->pending_base_slots|=entry->subscription.bound_slots;
        entry->subscription=(ksn_source_subscription){0};
        entry->pending_revision=0;
        sources->expiry_us=0;
        return JS_UNDEFINED;
    }
    uint32_t serial=(uint32_t)(uintptr_t)JS_GetOpaque(argv[0],source_cap_class);
    if(!serial||!source_caps)return throw_result(ctx,KSN_STALE,op);
    source_cap_record *offer=NULL;
    for(unsigned i=0;i<source_caps->count;i++)
        if(source_caps->records[i].serial==serial){
            offer=&source_caps->records[i];break;
        }
    if(!offer)return throw_result(ctx,KSN_STALE,op);
    schema_externals *external=schema_external_state(s);
    if(external)for(unsigned i=0;i<external->count;i++)
        if(external->entries[i].registry==offer->registry&&
           external->entries[i].handle.index==offer->handle.index&&
           external->entries[i].handle.generation==offer->handle.generation){
            schema_external_remove(s,i);
            break;
        }
    return JS_UNDEFINED;
}
static const JSCFunctionListEntry schema_methods[]={
    JS_CFUNC_DEF("set",1,js_schema_set_method),
    JS_CFUNC_DEF("bind",2,js_schema_bind_method),
    JS_CFUNC_DEF("unbind",1,js_schema_unbind_method),
};
static bool lazy_proto(JSContext *ctx,JSClassID id,const JSCFunctionListEntry *methods,int count);
static JSValue js_schema_mount(JSContext *ctx,const pocket_app_view_asset *asset,
                               void *owned_asset,size_t owned_asset_bytes){
    if(!ensure_state(ctx,"kasane.mount"))return JS_EXCEPTION;
    if(state->schema||state->provider||state->building.value||
       state->submitted.value||state->active)
        return throw_result(ctx,KSN_BUSY,"kasane.mount");
    if(schema_serial==UINT32_MAX)return throw_result(ctx,KSN_LIMIT,"kasane.mount");
    if(!lazy_proto(ctx,schema_class,schema_methods,
                   (int)(sizeof(schema_methods)/sizeof(schema_methods[0]))))
        return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,schema_class);
    if(JS_IsException(object))return object;
    schema_state *s=schema_alloc(asset,schema_serial+1u);
    if(!s){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_OOM,"kasane.mount");}
    s->owned_asset=owned_asset;
    s->owned_asset_bytes=owned_asset_bytes;
    schema_serial=s->handle;
    JS_SetOpaque(object,(void *)(uintptr_t)s->handle);
    state->schema=s;
    return object;
}
/* Runtime declarations compile once into the same immutable descriptor that
 * flash assets use. The pools are one mount-time allocation, never visited by
 * the per-frame source/key fast path. */
#define RUNTIME_NAME_BYTES 32u
typedef struct {
    pocket_app_view_asset asset;
    ksn_schema schema;
    size_t bytes;
} runtime_descriptor;
static bool runtime_u16(JSContext *ctx,JSValueConst object,const char *key,
                        uint16_t maximum,uint16_t *out,bool required){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)&&!required){JS_FreeValue(ctx,value);return true;}
    double number;bool ok=number_in(ctx,value,0,maximum,&number);
    JS_FreeValue(ctx,value);
    if(ok)*out=(uint16_t)number;
    return ok;
}
static bool runtime_u32(JSContext *ctx,JSValueConst object,const char *key,
                        uint32_t *out){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){JS_FreeValue(ctx,value);return true;}
    bool ok=parse_u32(ctx,value,out);
    JS_FreeValue(ctx,value);
    return ok;
}
static bool runtime_binding(JSContext *ctx,JSValueConst object,const char *key,
                            ksn_slot_type type,const ksn_schema *schema,
                            char literal[KSN_SCHEMA_TEXT_MAX+1u],
                            ksn_schema_binding *out){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    JSValue slot=JS_IsObject(value)&&!JS_IsArray(value)?
                 JS_GetPropertyStr(ctx,value,"slot"):JS_UNDEFINED;
    if(JS_IsException(slot)){JS_FreeValue(ctx,value);return false;}
    bool ok=false;
    if(JS_IsString(slot)){
        const char *name=bounded_cstring(ctx,slot,RUNTIME_NAME_BYTES-1u);
        if(name){
            for(unsigned i=0;i<schema->slot_count;i++)
                if(strcmp(name,schema->slots[i].name)==0&&schema->slots[i].type==type){
                    *out=(ksn_schema_binding){.slot=(uint8_t)i};ok=true;break;
                }
            JS_FreeCString(ctx,name);
        }
    }else if(JS_IsUndefined(slot)){
        *out=(ksn_schema_binding){.slot=KSN_SCHEMA_LITERAL};
        if(type==KSN_SLOT_RECT)ok=parse_rect_mode(ctx,value,&out->literal.rect,
                                                  "kasane.mount",false);
        else if(type==KSN_SLOT_COLOR){
            uint32_t color;ok=parse_u32(ctx,value,&color);
            if(ok)out->literal.color=color;
        }else if(type==KSN_SLOT_BOOL){
            ok=JS_IsBool(value);if(ok)out->literal.boolean=JS_ToBool(ctx,value)!=0;
        }else if(type==KSN_SLOT_U16){
            double n;ok=number_in(ctx,value,0,UINT16_MAX,&n);
            if(ok)out->literal.number=(uint16_t)n;
        }else if(type==KSN_SLOT_RESOURCE){
            out->literal.resource.value=opaque_value(value,image_class);
            ok=out->literal.resource.value!=0;
        }else if(type==KSN_SLOT_TEXT&&JS_IsString(value)){
            int64_t units=0;
            if(JS_GetLength(ctx,value,&units)==0&&units<=KSN_SCHEMA_TEXT_MAX){
                size_t length=0;const char *src=JS_ToCStringLen(ctx,&length,value);
                if(src){
                    ksn_p0_probe_copy(KSN_P0_UTF8_REQUEST,length);
                    if(length<=KSN_SCHEMA_TEXT_MAX){
                        memcpy(literal,src,length);literal[length]=0;
                        ksn_p0_probe_copy(KSN_P0_ADAPTER_TEMP,length);
                        out->literal.text=(ksn_schema_text){literal,(uint16_t)length};ok=true;
                    }
                    JS_FreeCString(ctx,src);
                }
            }
        }
    }
    JS_FreeValue(ctx,slot);JS_FreeValue(ctx,value);
    return ok;
}
static bool runtime_optional_binding(JSContext *ctx,JSValueConst object,const char *key,
                                     ksn_slot_type type,const ksn_schema *schema,
                                     char literal[KSN_SCHEMA_TEXT_MAX+1u],
                                     ksn_schema_binding *out,bool *present){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    *present=!JS_IsUndefined(value);JS_FreeValue(ctx,value);
    return !*present||runtime_binding(ctx,object,key,type,schema,literal,out);
}
static runtime_descriptor *runtime_compile(JSContext *ctx,JSValueConst definition){
    JSValue slots=JS_GetPropertyStr(ctx,definition,"slots");
    JSValue nodes=JS_GetPropertyStr(ctx,definition,"nodes");
    JSPropertyEnum *properties=NULL;uint32_t slot_count=0;
    int64_t node_count=0;
    bool valid=JS_IsObject(slots)&&!JS_IsArray(slots)&&JS_IsArray(nodes)&&
        JS_GetOwnPropertyNames(ctx,&properties,&slot_count,slots,
                               JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)==0&&
        JS_GetLength(ctx,nodes,&node_count)==0&&
        slot_count<=KSN_SCHEMA_MAX_SLOTS&&node_count>=0&&
        node_count<=KSN_SCHEMA_MAX_NODES;
    runtime_descriptor *r=NULL;
    if(valid){
        size_t bytes=sizeof(*r)+(size_t)slot_count*sizeof(ksn_schema_slot)+
          (size_t)node_count*sizeof(ksn_schema_node)+
          (size_t)slot_count*RUNTIME_NAME_BYTES+
          (size_t)node_count*(KSN_SCHEMA_TEXT_MAX+1u);
        r=calloc(1,bytes);valid=r!=NULL;
        if(valid){
            r->bytes=bytes;
            char *pool=(char *)(r+1);
            ksn_schema_slot *slot_defs=(ksn_schema_slot *)pool;
            pool+=(size_t)slot_count*sizeof(*slot_defs);
            ksn_schema_node *node_defs=(ksn_schema_node *)pool;
            pool+=(size_t)node_count*sizeof(*node_defs);
            char *names=pool;pool+=(size_t)slot_count*RUNTIME_NAME_BYTES;
            char *literal_text=pool;
            r->asset.schema=&r->schema;
            r->schema=(ksn_schema){.version=KSN_SCHEMA_ABI_VERSION,.slot_count=(uint8_t)slot_count,
                .node_count=(uint8_t)node_count,.background=0x000000ffu,
                .slots=slot_defs,.nodes=node_defs};
            uint16_t version=0;
            valid=runtime_u16(ctx,definition,"version",1,&version,true)&&version==1;
            JSValue bg=JS_GetPropertyStr(ctx,definition,"background");
            if(JS_IsException(bg))valid=false;
            else if(!JS_IsUndefined(bg)){
                uint32_t color=0;
                if(!parse_u32(ctx,bg,&color))valid=false;
                else r->schema.background=color;
            }
            JS_FreeValue(ctx,bg);
            for(unsigned i=0;valid&&i<slot_count;i++){
                JSValue name_value=JS_AtomToValue(ctx,properties[i].atom);
                const char *name=bounded_cstring(ctx,name_value,RUNTIME_NAME_BYTES-1u);
                JS_FreeValue(ctx,name_value);
                if(!name){valid=false;break;}
                size_t len=strlen(name);
                if(!len||len>=RUNTIME_NAME_BYTES)valid=false;
                else{memcpy(names+i*RUNTIME_NAME_BYTES,name,len+1u);
                     slot_defs[i].name=names+i*RUNTIME_NAME_BYTES;}
                JS_FreeCString(ctx,name);
                if(!valid)break;
                JSValue spec=JS_GetProperty(ctx,slots,properties[i].atom);
                JSValue type=JS_IsObject(spec)?JS_GetPropertyStr(ctx,spec,"type"):JS_UNDEFINED;
                const char *label=bounded_cstring(ctx,type,16);
                if(!label)valid=false;
                else{
                    static const char *const types[]={"text","rect","color","bool","u16","resource","u32"};
                    unsigned t=0;for(;t<7;t++)if(strcmp(label,types[t])==0)break;
                    if(t==7)valid=false;else slot_defs[i].type=(ksn_slot_type)t;
                    JS_FreeCString(ctx,label);
                }
                if(valid&&slot_defs[i].type==KSN_SLOT_TEXT){
                    uint16_t cap=0;valid=runtime_u16(ctx,spec,"capacity",KSN_SCHEMA_TEXT_MAX,&cap,true)&&cap>0;
                    slot_defs[i].capacity=(uint8_t)cap;
                }
                if(valid&&slot_defs[i].type==KSN_SLOT_U16){
                    uint16_t initial=0,maximum=0;
                    valid=runtime_u16(ctx,spec,"initial",UINT16_MAX,&initial,false)&&
                          runtime_u16(ctx,spec,"maximum",UINT16_MAX,&maximum,false);
                    if(valid){slot_defs[i].initial_number=initial;slot_defs[i].maximum=maximum;}
                }
                if(valid&&slot_defs[i].type==KSN_SLOT_U32){
                    valid=runtime_u32(ctx,spec,"initial",&slot_defs[i].initial_number)&&
                          runtime_u32(ctx,spec,"maximum",&slot_defs[i].maximum);
                }
                JS_FreeValue(ctx,type);JS_FreeValue(ctx,spec);
            }
            JSValue background_slot=JS_GetPropertyStr(ctx,definition,"backgroundSlot");
            if(JS_IsException(background_slot))valid=false;
            else if(valid&&!JS_IsUndefined(background_slot)){
                const char *name=bounded_cstring(ctx,background_slot,RUNTIME_NAME_BYTES-1u);
                if(!name)valid=false;
                else{
                    bool found=false;
                    for(unsigned i=0;i<slot_count;i++)
                        if(strcmp(name,slot_defs[i].name)==0&&slot_defs[i].type==KSN_SLOT_COLOR){
                            r->schema.dynamic_background=true;
                            r->schema.background_slot=(uint8_t)i;found=true;break;
                        }
                    if(!found)valid=false;
                    JS_FreeCString(ctx,name);
                }
            }
            JS_FreeValue(ctx,background_slot);
            for(unsigned i=0;valid&&i<(unsigned)node_count;i++){
                JSValue spec=JS_GetPropertyUint32(ctx,nodes,i);
                JSValue type=JS_IsObject(spec)?JS_GetPropertyStr(ctx,spec,"type"):JS_UNDEFINED;
                const char *label=bounded_cstring(ctx,type,16);
                if(!label)valid=false;
                else{
                    static const char *const types[]={"rect","roundRect","text","image","plateText"};
                    unsigned t=0;for(;t<5;t++)if(strcmp(label,types[t])==0)break;
                    if(t==5)valid=false;else node_defs[i].kind=(ksn_schema_node_kind)t;
                    JS_FreeCString(ctx,label);
                }
                ksn_schema_node *n=&node_defs[i];
                char *text=literal_text+i*(KSN_SCHEMA_TEXT_MAX+1u);
                if(valid)valid=runtime_binding(ctx,spec,"bounds",KSN_SLOT_RECT,&r->schema,text,&n->bounds);
                JSValue add=JS_GetPropertyStr(ctx,spec,"rectAdd");
                if(JS_IsException(add))valid=false;
                else if(!JS_IsUndefined(add)){
                    int64_t length=0;
                    if(!JS_IsArray(add)||JS_GetLength(ctx,add,&length)<0||length!=4)valid=false;
                    for(unsigned edge=0;valid&&edge<4;edge++){
                        JSValue name_value=JS_GetPropertyUint32(ctx,add,edge);
                        if(JS_IsException(name_value)){valid=false;break;}
                        if(!JS_IsNull(name_value)&&!JS_IsUndefined(name_value)){
                            const char *name=bounded_cstring(ctx,name_value,RUNTIME_NAME_BYTES-1u);
                            if(!name)valid=false;
                            else{
                                bool found=false;
                                for(unsigned slot=0;slot<slot_count;slot++)
                                    if(strcmp(name,slot_defs[slot].name)==0&&
                                       slot_defs[slot].type==KSN_SLOT_U16){
                                        n->rect_add_mask|=(uint8_t)(1u<<edge);
                                        n->rect_add_slot[edge]=(uint8_t)slot;found=true;break;
                                    }
                                if(!found)valid=false;
                                JS_FreeCString(ctx,name);
                            }
                        }
                        JS_FreeValue(ctx,name_value);
                    }
                }
                JS_FreeValue(ctx,add);
                if(valid&&n->kind!=KSN_NODE_IMAGE)
                    valid=runtime_binding(ctx,spec,"color",KSN_SLOT_COLOR,&r->schema,text,&n->color);
                if(valid&&(n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT))
                    valid=runtime_binding(ctx,spec,"text",KSN_SLOT_TEXT,&r->schema,text,&n->text);
                if(valid&&n->kind==KSN_NODE_PLATE_TEXT)
                    valid=runtime_binding(ctx,spec,"plateColor",KSN_SLOT_COLOR,&r->schema,text,&n->plate_color);
                if(valid&&n->kind==KSN_NODE_IMAGE){
                    valid=runtime_binding(ctx,spec,"resource",KSN_SLOT_RESOURCE,&r->schema,text,&n->resource)&&
                          runtime_binding(ctx,spec,"variant",KSN_SLOT_U16,&r->schema,text,&n->variant)&&
                          runtime_binding(ctx,spec,"frame",KSN_SLOT_U16,&r->schema,text,&n->frame)&&
                          runtime_u16(ctx,spec,"sourceWidth",256,&n->source_width,true)&&
                          runtime_u16(ctx,spec,"sourceHeight",256,&n->source_height,true);
                }
                bool present=false;
                if(valid)valid=runtime_optional_binding(ctx,spec,"visible",KSN_SLOT_BOOL,&r->schema,
                                                         text,&n->visible,&present);
                if(present)n->flags|=KSN_SCHEMA_HAS_VISIBLE;
                if(valid)valid=runtime_optional_binding(ctx,spec,"page",KSN_SLOT_U16,&r->schema,
                                                         text,&n->page,&present);
                if(present){n->flags|=KSN_SCHEMA_HAS_PAGE;
                    valid=valid&&runtime_u16(ctx,spec,"pageEquals",UINT16_MAX,&n->page_equals,true);}
                if(valid)valid=runtime_optional_binding(ctx,spec,"reveal",KSN_SLOT_U16,&r->schema,
                                                         text,&n->reveal,&present);
                if(present)n->flags|=KSN_SCHEMA_HAS_REVEAL;
                uint16_t radius=0,font=0;
                if(valid)valid=runtime_u16(ctx,spec,"radius",8,&radius,false)&&
                               runtime_u16(ctx,spec,"font",KSN_DISPLAY,&font,false);
                n->radius=(uint8_t)radius;n->font=(ksn_font)font;
                JS_FreeValue(ctx,type);JS_FreeValue(ctx,spec);
            }
            if(valid)valid=ksn_schema_validate(&r->schema)==KSN_OK;
        }
    }
    JS_FreePropertyEnum(ctx,properties,slot_count);
    JS_FreeValue(ctx,slots);JS_FreeValue(ctx,nodes);
    if(!valid){free(r);return NULL;}
    return r;
}
static JSValue js_presenter_mount(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    if(argc&&JS_IsObject(argv[0])&&!JS_IsArray(argv[0])){
        runtime_descriptor *compiled=runtime_compile(ctx,argv[0]);
        if(!compiled)return JS_HasException(ctx)?JS_EXCEPTION:
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.mount",
                             "invalid view definition",false,NULL);
        JSValue object=js_schema_mount(ctx,&compiled->asset,compiled,compiled->bytes);
        if(JS_IsException(object)){free(compiled);return object;}
        if(argc>1&&!JS_IsUndefined(argv[1])){
            JSValue result=js_schema_set(ctx,state->schema,argv[1]);
            if(JS_IsException(result)){
                if(!state->submitted.value){
                    schema_state *failed=state->schema;state->schema=NULL;
                    free(failed);free(compiled);
                }
                JS_FreeValue(ctx,object);return result;
            }
            JS_FreeValue(ctx,result);
        }
        return object;
    }
    const char *id=argc?bounded_cstring(ctx,argv[0],31):NULL;
    if(!id)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.mount",
                                  "view id must be a registered presenter",false,NULL);
    const pocket_app_view_asset *asset=pocket_app_view_lookup(id);
    if(asset){JS_FreeCString(ctx,id);return js_schema_mount(ctx,asset,NULL,0);}
    const pocket_app_view_provider *provider=pocket_app_view_provider_lookup(id);
    JS_FreeCString(ctx,id);
    if(!provider)return throw_result(ctx,KSN_UNSUPPORTED,"kasane.mount");
    if(!ensure_state(ctx,"kasane.mount"))return JS_EXCEPTION;
    if(state->schema||state->provider||state->building.value||state->submitted.value||state->active)
        return throw_result(ctx,KSN_BUSY,"kasane.mount");
    pocket_app_view_host host={.owner=state,.view=view(),.viewport=viewport,
        .now_us=&owner_now_us,.busy=provider_busy,.submitted=provider_submitted};
    void *instance=NULL;
    JSValue object=provider->mount(ctx,&host,&instance);
    if(JS_IsException(object))return object;
    if(!instance){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_INVALID,"kasane.mount");}
    state->provider=provider;state->provider_state=instance;
    return object;
}
