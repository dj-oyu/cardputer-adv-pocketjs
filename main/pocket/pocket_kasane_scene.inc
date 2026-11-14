/* The tx and modal wrappers of replace/patch, the cache handles, stats and
 * the method tables for tx/modal/ref/animation/instance. It is an .inc, not a
 * .c, because pocket_kasane.c is one translation unit: the statics it shares
 * with the tx and parse code must keep inlining at -O2 and the object name
 * `pocket_kasane:` used by main/vm/helper_iram8.lf must keep matching.
 * Function order is the original order on purpose. Included only from
 * pocket_kasane.c. (createScene and its native scene controller were removed
 * in api-reorg P5; a scene is now the app's own loop over replace/patch.) */
/* The tx and modal wrappers of replace/patch (docs/kasane/api-reorg.md
 * sec.3.2). They used to be made for every call, with the ticket and an own
 * "modal" property: four guest allocations a call on the per-turn path. Now a
 * build reuses the pair of the build before -- unless the app kept one (a
 * reference besides ours: stored, captured, returned), in which case ours is
 * dropped and the next build makes a fresh one. So a tx that outlives its
 * callback stays CLOSED for good, in later callbacks too, exactly as when
 * every call made its own (R4); an app that keeps nothing allocates nothing.
 * The opaque holds the transaction only while the callback runs. tx.modal is
 * a getter on the prototype. They are guest values: pocket_kasane_reset() --
 * app_stop(), before the guest goes -- drops them (release_build_wrappers). */
static JSValue build_tx={0},build_modal={0};
static JSContext *build_ctx;
/* The count QuickJS keeps in front of every object (JSRefCountHeader in
 * quickjs.c, the first field of JSGCObjectHeader); not in quickjs.h. */
typedef struct { int ref_count; } build_ref_header;
static bool build_shared(JSValueConst v) {
    return JS_IsObject(v)&&((const build_ref_header *)JS_VALUE_GET_PTR(v))->ref_count>1;
}
static void release_build_wrappers(void) {
    if(!build_ctx) return;
    JS_FreeValue(build_ctx,build_tx);JS_FreeValue(build_ctx,build_modal);
    build_tx=build_modal=JS_UNDEFINED;build_ctx=NULL;
}
static bool build_wrappers(JSContext *ctx) {
    if(build_ctx!=ctx) {
        release_build_wrappers();
        build_tx=build_modal=JS_UNDEFINED;build_ctx=ctx;
    }
    if(JS_IsUndefined(build_tx)) {
        build_tx=wrap_direct(ctx,tx_class,0);
        if(JS_IsException(build_tx)) { build_tx=JS_UNDEFINED;return false; }
    }
    if(JS_IsUndefined(build_modal)) {
        build_modal=wrap_direct(ctx,modal_class,0);
        if(JS_IsException(build_modal)) { build_modal=JS_UNDEFINED;return false; }
    }
    return true;
}
/* After a callback: the opaques close, and a wrapper the app kept is left
 * to it (CLOSED) instead of being handed to the next build. */
static void settle_build_wrappers(JSContext *ctx) {
    JS_SetOpaque(build_tx,NULL);JS_SetOpaque(build_modal,NULL);
    if(build_shared(build_tx)) { JS_FreeValue(ctx,build_tx);build_tx=JS_UNDEFINED; }
    if(build_shared(build_modal)) { JS_FreeValue(ctx,build_modal);build_modal=JS_UNDEFINED; }
}
static JSValue js_tx_modal(JSContext *ctx, JSValueConst self) {
    /* Only the live tx's modal; a kept, closed tx gets a closed modal. */
    if(build_ctx==ctx&&JS_IsObject(self)&&JS_IsObject(build_tx)&&
       JS_VALUE_GET_PTR(self)==JS_VALUE_GET_PTR(build_tx))
        return JS_DupValue(ctx,build_modal);
    return wrap_direct(ctx,modal_class,0);
}

/* view.replace/patch(build): the JS function is called and nothing is
 * returned (api-reorg P5 removed tickets, poll and cancel: the app learns
 * the outcome from the thrown error and from what the screen shows). */
static JSValue run_build(JSContext *ctx, JSValueConst build,
                         ksn_update_mode mode, const char *op) {
    if(!JS_IsFunction(ctx,build))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must be a synchronous function",false,NULL);
    if(!ensure_state(ctx,op)) return JS_EXCEPTION;
    if(state->provider||state->schema)return throw_result(ctx,KSN_BUSY,op);
    /* Keep the callback closed to reentrant builds after an inner abort. */
    if(state->building.value) return throw_result(ctx,KSN_BUSY,op);
    if(!tx_proto(ctx)||!modal_proto(ctx)) return JS_EXCEPTION;
    /* Before the transaction: a failure here has nothing to cancel. */
    if(!build_wrappers(ctx)) return JS_EXCEPTION;
    apply_outcome();
    frame_bd_join_point(FRAME_BD_JOIN_KASANE_TX);
    ksn_tx tx;ksn_result result=ksn_view_begin(view(),mode,&tx);
    if(result!=KSN_OK) return throw_result(ctx,result,op);
    state->building=tx;
    JS_SetOpaque(build_tx,(void *)(uintptr_t)tx.value);
    JS_SetOpaque(build_modal,(void *)(uintptr_t)tx.value);
    /* No fallible allocation may follow successful native submission. */
    JSValue arg=JS_DupValue(ctx,build_tx);
    JSValue returned=JS_Call(ctx,build,JS_UNDEFINED,1,&arg);
    JS_FreeValue(ctx,arg);
    JS_SetOpaque(build_tx,NULL);JS_SetOpaque(build_modal,NULL);
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
    settle_build_wrappers(ctx);
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
    return JS_UNDEFINED;
fail:
    settle_build_wrappers(ctx);
    ksn_view_cancel(view(),tx);discard_candidates(tx);state->building=(ksn_tx){0};
    return JS_EXCEPTION;
}

static JSValue js_replace(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv) {
    (void)self;
#ifdef KASANE_MEGADEMO_TRACE
    JSValue done=run_build(ctx,argc?argv[0]:JS_UNDEFINED,KSN_REPLACE,"kasane.replace");
    if(!JS_IsException(done))pocket_proc_trace_view();
    return done;
#else
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,KSN_REPLACE,"kasane.replace");
#endif
}
static JSValue js_patch(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;
#ifdef KASANE_MEGADEMO_TRACE
    JSValue done=run_build(ctx,argc?argv[0]:JS_UNDEFINED,KSN_PATCH,"kasane.patch");
    if(!JS_IsException(done))pocket_proc_trace_view();
    return done;
#else
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,KSN_PATCH,"kasane.patch");
#endif
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

/* Defining own properties avoids invoking user-installed prototype setters.
 * JS_DefinePropertyValueStr consumes value on both success and failure. */
static bool put(JSContext *ctx, JSValueConst object, const char *name, JSValue value) {
    if(JS_IsException(value)) return false;
    return JS_DefinePropertyValueStr(ctx,object,name,value,JS_PROP_C_W_E)>=0;
}
#define PUT(object,name,value) do { if(!put(ctx,object,name,value)) goto fail; } while(0)

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
    JS_CGETSET_DEF("modal",js_tx_modal,NULL),
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

#ifdef POCKET_KASANE_TEST_HOOKS
/* Host tests only (tools/build_kasane_test.sh defines the macro; the firmware
 * never does, so this costs no flash). api-reorg P5 removed kasane.poll and
 * kasane.cancel from JS, but the repair path after a cancelled or failed
 * presentation is still real C behaviour -- the shell discards an APP
 * submission when it must -- and these two functions let a test drive it. */
ksn_submission pocket_kasane_test_poll(void){
    apply_outcome();
    return state?ksn_view_poll(view()):(ksn_submission){0};
}
bool pocket_kasane_test_cancel(uint32_t ticket){
    if(!state||!ticket||ksn_view_cancel(view(),(ksn_tx){ticket})!=KSN_OK)return false;
    apply_outcome();return true;
}
#endif
