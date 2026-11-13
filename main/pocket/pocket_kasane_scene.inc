/* The native scene controller of createScene(): kasane_scene, its build/flush
 * turn, the stats result builder, and the method tables for
 * tx/modal/ref/animation/instance. It is an .inc, not a .c, because
 * pocket_kasane.c is one translation unit: the statics it shares with the tx
 * and parse code must keep inlining at -O2 and the object name
 * `pocket_kasane:` used by main/vm/helper_iram8.lf must keep matching.
 * Function order is the original order on purpose. Included only from
 * pocket_kasane.c. */
/* The scene controller of createScene(), native since the guest-memory work
 * (docs/kasane/decisions.md). As JS it cost each Kasane app
 * four closures, their bytecode and source copies, a dozen var_refs and the
 * atoms of every local name; here it is one opaque holder and two bound
 * functions. apps/kasane/create_scene.js remains as the reference model the
 * node tests drive, and the flush below is that file transcribed statement by
 * statement: keep the two in step. */
typedef struct {
    JSValue build,patch,refs,candidate,model;
    bool pending,pending_replace,dirty,rebuild,running;
} kasane_scene;
static JSClassID scene_class;
static pocket_class_owner_t scene_owner;

static void scene_set(JSContext *ctx,JSValue *slot,JSValue value) {
    JSValue old=*slot;*slot=value;JS_FreeValue(ctx,old);
}

/* build(tx, model) must yield a non-null, non-callable object (the candidate
 * refs); patch(tx, refs, model) promotes the displayed refs unchanged. */
static JSValue scene_callback(JSContext *ctx,kasane_scene *scene,JSValueConst tx,
                              ksn_update_mode mode) {
    if(mode==KSN_PATCH) {
        JSValueConst args[]={tx,scene->refs,scene->model};
        JSValue returned=JS_Call(ctx,scene->patch,JS_UNDEFINED,3,args);
        if(!JS_IsException(returned))
            scene_set(ctx,&scene->candidate,JS_DupValue(ctx,scene->refs));
        return returned;
    }
    JSValueConst args[]={tx,scene->model};
    JSValue returned=JS_Call(ctx,scene->build,JS_UNDEFINED,2,args);
    if(JS_IsException(returned)) return returned;
    scene_set(ctx,&scene->candidate,JS_DupValue(ctx,returned));
    if(JS_IsObject(returned)&&!JS_IsFunction(ctx,returned)) return returned;
    JS_FreeValue(ctx,returned);
    return JS_ThrowTypeError(ctx,"scene build must return an object containing candidate refs");
}

/* scene is NULL for view.replace/patch(build): the ticket is created and the
 * JS function called. A scene discards the ticket, so none is allocated. */
static JSValue run_build(JSContext *ctx, JSValueConst build, kasane_scene *scene,
                         ksn_update_mode mode, const char *op) {
    if(!scene&&!JS_IsFunction(ctx,build))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must be a synchronous function",false,NULL);
    if(!ensure_state(ctx,op)) return JS_EXCEPTION;
    if(state->provider||state->schema)return throw_result(ctx,KSN_BUSY,op);
    /* Keep the callback closed to reentrant builds after an inner abort. */
    if(state->building.value) return throw_result(ctx,KSN_BUSY,op);
    if(!tx_proto(ctx)||!modal_proto(ctx)) return JS_EXCEPTION;
    apply_outcome();
    frame_bd_join_point(FRAME_BD_JOIN_KASANE_TX);
    ksn_tx tx;ksn_result result=ksn_view_begin(view(),mode,&tx);
    if(result!=KSN_OK) return throw_result(ctx,result,op);
    state->building=tx;
    JSValue ticket=JS_UNDEFINED,tx_object=JS_UNDEFINED,modal_object=JS_UNDEFINED;
    /* No fallible allocation may follow successful native submission. */
    if(!scene) {
        ticket=wrap_direct(ctx,ticket_class,tx.value);
        if(JS_IsException(ticket)) goto fail;
    }
    tx_object=wrap_direct(ctx,tx_class,tx.value);
    if(JS_IsException(tx_object)) goto fail;
    modal_object=wrap_direct(ctx,modal_class,tx.value);
    if(JS_IsException(modal_object)) goto fail;
    if(JS_DefinePropertyValueStr(ctx,tx_object,"modal",JS_DupValue(ctx,modal_object),
                                 JS_PROP_C_W_E)<0) goto fail;
    JSValue arg=JS_DupValue(ctx,tx_object);
    JSValue returned=scene?scene_callback(ctx,scene,arg,mode)
                          :JS_Call(ctx,build,JS_UNDEFINED,1,&arg);
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
    if(result!=KSN_OK) {
        if(result==KSN_INVALID&&mode==KSN_REPLACE)
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "replace requires tx.background(color)",false,
                             POCKET_OUTCOME_NOT_APPLIED);
        else throw_result(ctx,result,op);
        goto fail;
    }
    state->building=(ksn_tx){0};
    state->submitted=tx;state->submitted_mode=mode;state->active=true;
    ksn_runtime_app_activate(state->lease);
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
    (void)self;
#ifdef KASANE_MEGADEMO_TRACE
    JSValue ticket=run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_REPLACE,"kasane.replace");
    if(!JS_IsException(ticket))pocket_proc_trace_view();
    return ticket;
#else
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_REPLACE,"kasane.replace");
#endif
}
static JSValue js_patch(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;
#ifdef KASANE_MEGADEMO_TRACE
    JSValue ticket=run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_PATCH,"kasane.patch");
    if(!JS_IsException(ticket))pocket_proc_trace_view();
    return ticket;
#else
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_PATCH,"kasane.patch");
#endif
}

static void scene_finalizer(JSRuntime *rt, JSValue value) {
    kasane_scene *scene=JS_GetOpaque(value,scene_class);
    if(!scene) return;
    JS_FreeValueRT(rt,scene->build);JS_FreeValueRT(rt,scene->patch);
    JS_FreeValueRT(rt,scene->refs);JS_FreeValueRT(rt,scene->candidate);
    JS_FreeValueRT(rt,scene->model);js_free_rt(rt,scene);
}
static void scene_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
    kasane_scene *scene=JS_GetOpaque(value,scene_class);
    if(!scene) return;
    JS_MarkValue(rt,scene->build,mark);JS_MarkValue(rt,scene->patch,mark);
    JS_MarkValue(rt,scene->refs,mark);JS_MarkValue(rt,scene->candidate,mark);
    JS_MarkValue(rt,scene->model,mark);
}
static const JSClassDef scene_def={.class_name=KASANE_CLASS,.finalizer=scene_finalizer,
                                   .gc_mark=scene_mark};

static JSValue scene_invalidate(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                                int magic,JSValueConst *data) {
    (void)self;(void)magic;
    kasane_scene *scene=JS_GetOpaque(data[0],scene_class);
    scene->dirty=true;
    if(argc&&JS_ToBool(ctx,argv[0])) scene->rebuild=true;
    return JS_UNDEFINED;
}

static JSValue scene_flush_turn(JSContext *ctx,kasane_scene *scene,JSValueConst model) {
    if(scene->pending) {
        apply_outcome();
        ksn_submission outcome=state?ksn_view_poll(view()):(ksn_submission){0};
        if(outcome.status==KSN_SUBMITTED) return JS_FALSE;
        if(outcome.status==KSN_PRESENTED)
            scene_set(ctx,&scene->refs,JS_DupValue(ctx,scene->candidate));
        else if(outcome.status==KSN_DISCARDED) {
            scene->dirty=true;
            if(scene->pending_replace) scene->rebuild=true;
        } else return JS_ThrowPlainError(ctx,"scene lost its submission");
        scene_set(ctx,&scene->candidate,JS_NULL);
        scene->pending=false;
    }
    if(!scene->dirty) return JS_TRUE;
    bool replacing=scene->rebuild||JS_IsNull(scene->refs)||JS_IsUndefined(scene->patch);
    scene->dirty=false;
    scene->rebuild=false;
    scene_set(ctx,&scene->model,JS_DupValue(ctx,model));
    JSValue submitted=replacing?run_build(ctx,JS_UNDEFINED,scene,KSN_REPLACE,"kasane.replace")
                               :run_build(ctx,JS_UNDEFINED,scene,KSN_PATCH,"kasane.patch");
    JSValue out=JS_FALSE;
    if(!JS_IsException(submitted)) {
        scene->pending_replace=replacing;
        scene->pending=true;
        goto done;
    }
    JSValue error=JS_GetException(ctx);
    /* An interrupt skips catch and finally in JS too: pass it straight on. */
    if(JS_IsUncatchableError(error)) { JS_Throw(ctx,error);return JS_EXCEPTION; }
    scene_set(ctx,&scene->candidate,JS_NULL);
    scene->dirty=true;
    if(replacing) scene->rebuild=true;
    bool busy=false;
    if(JS_ToBool(ctx,error)) {
        JSValue code=JS_GetPropertyStr(ctx,error,"code");
        if(JS_IsException(code)) { JS_FreeValue(ctx,error);out=JS_EXCEPTION;goto done; }
        if(JS_IsString(code)) {
            size_t length;const char *text=JS_ToCStringLen(ctx,&length,code);
            if(!text) { JS_FreeValue(ctx,code);JS_FreeValue(ctx,error);out=JS_EXCEPTION;goto done; }
            busy=length==4&&!memcmp(text,"BUSY",4);
            JS_FreeCString(ctx,text);
        }
        JS_FreeValue(ctx,code);
    }
    if(busy) JS_FreeValue(ctx,error);
    else { JS_Throw(ctx,error);out=JS_EXCEPTION; }
done:
    scene_set(ctx,&scene->model,JS_UNDEFINED);
    return out;
}

static JSValue scene_flush(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                           int magic,JSValueConst *data) {
    (void)self;(void)magic;
    kasane_scene *scene=JS_GetOpaque(data[0],scene_class);
    if(scene->running) return JS_ThrowPlainError(ctx,"scene flush is not reentrant");
    scene->running=true;
    JSValue out=scene_flush_turn(ctx,scene,argc?argv[0]:JS_UNDEFINED);
    scene->running=false;
    return out;
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
        bool ok=!JS_IsException(item)&&parse_cache_draw(ctx,item,&draws[i],"kasane.cache.create");
        JS_FreeValue(ctx,item);
        if(!ok) return JS_EXCEPTION;
    }
    JSValue object=wrap_direct(ctx,template_class,0);
    if(JS_IsException(object)) return object;
    ksn_template result_handle;
    ksn_result result=create_template(draws,(uint16_t)length,&result_handle);
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
    PUT(out,"roundRect",JS_NewBool(ctx,true));
    PUT(out,"strokeRect",JS_NewBool(ctx,true));
    PUT(out,"gradient",JS_NewBool(ctx,true));
    PUT(out,"text",JS_NewBool(ctx,true));
    PUT(out,"image",JS_NewBool(ctx,true));
    PUT(out,"imageStretch",JS_NewBool(ctx,true));
    PUT(out,"imageRotation",JS_NewBool(ctx,true));
    PUT(out,"groupOpacity",JS_NewBool(ctx,true));
    PUT(out,"modal",JS_NewBool(ctx,!overlay_profile));
    PUT(out,"animation",JS_NewBool(ctx,true));
    PUT(out,"frosted",JS_NewBool(ctx,false));
    PUT(capacity,"commands",JS_NewInt32(ctx,KSN_APP_COMMANDS));
    PUT(capacity,"textBytes",JS_NewInt32(ctx,KSN_APP_TEXT_BYTES));
    PUT(capacity,"refs",JS_NewInt32(ctx,KASANE_REF_LIMIT));
    PUT(capacity,"animations",JS_NewInt32(ctx,KSN_APP_TRACKS));
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
    ksn_view_stats stats=ksn_runtime_stats(KSN_APP);
    JSValue out=JS_UNDEFINED,displayed=JS_UNDEFINED,cache=JS_UNDEFINED;
    out=JS_NewObject(ctx);if(JS_IsException(out)) goto fail;
    displayed=JS_NewObject(ctx);if(JS_IsException(displayed)) goto fail;
    cache=JS_NewObject(ctx);if(JS_IsException(cache)) goto fail;
    PUT(out,"active",JS_NewBool(ctx,state&&state->active));
    PUT(out,"nativeBytes",JS_NewUint32(ctx,ksn_runtime_reserved_bytes()+
        (state?sizeof(*state)+(source_caps?sizeof(*source_caps):0)+
          (state->provider?
          state->provider->native_bytes(state->provider_state):0)+
          (state->schema?state->schema->allocation_bytes+
                         state->schema->owned_asset_bytes+
                         (schema_external_state(state->schema)?
                          sizeof(schema_externals):0):0):0)));
    PUT(displayed,"commands",JS_NewInt32(ctx,stats.displayed.commands));
    PUT(displayed,"textBytes",JS_NewInt32(ctx,stats.displayed.text_bytes));
    PUT(cache,"commands",JS_NewInt32(ctx,stats.shared_cache.commands));
    PUT(cache,"templates",JS_NewInt32(ctx,stats.shared_cache.templates));
    PUT(cache,"instances",JS_NewInt32(ctx,stats.shared_cache.instances));
    PUT(cache,"reservedBytes",JS_NewUint32(ctx,ksn_runtime_cache_bytes()));
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
    JS_CFUNC_DEF("roundRect",1,js_tx_round_rect_checked),
    JS_CFUNC_DEF("strokeRect",1,js_tx_stroke_rect_checked),
    JS_CFUNC_DEF("gradient",1,js_tx_gradient_checked),
    JS_CFUNC_DEF("text",1,js_tx_text_checked),
    JS_CFUNC_DEF("image",1,js_tx_image_checked),
    JS_CFUNC_DEF("group",3,js_tx_group_checked),
    JS_CFUNC_DEF("instantiate",2,js_tx_instantiate_checked),
};
static const JSCFunctionListEntry modal_methods[]={
    JS_CFUNC_DEF("open",1,js_modal_open_checked),JS_CFUNC_DEF("close",0,js_modal_close_checked),
};
static const JSCFunctionListEntry ref_methods[]={
    JS_CFUNC_DEF("setRect",2,js_ref_rect_checked),JS_CFUNC_DEF("setClip",2,js_ref_clip_checked),
    JS_CFUNC_DEF("setColor",2,js_ref_color_checked),JS_CFUNC_DEF("setVisible",2,js_ref_visible_checked),
    JS_CFUNC_DEF("setText",2,js_ref_text_checked),JS_CFUNC_DEF("setReveal",2,js_ref_reveal_checked),
    JS_CFUNC_DEF("setImageFrame",3,js_ref_image_checked),
    JS_CFUNC_DEF("setRotation",2,js_ref_rotation_checked),
    JS_CFUNC_DEF("animate",2,js_ref_animate_checked),
};
static const JSCFunctionListEntry animation_methods[]={
    JS_CFUNC_DEF("stop",1,js_animation_stop_checked),JS_CFUNC_DEF("finish",1,js_animation_finish_checked),
    JS_CFUNC_DEF("poll",0,js_animation_poll),
};
static const JSCFunctionListEntry instance_methods[]={
    JS_CFUNC_DEF("place",2,js_instance_place_checked),
    JS_CFUNC_DEF("setVisible",2,js_instance_visible_checked),
};
