/* Managed follow objects own no guest pointers. js_mallocz keeps all native
 * state inside the guest's ordinary memory ceiling; finalizers use its runtime
 * allocator. Shared prototypes retain no per-instance closure or typed view.
 * Include after pocket_shape.c's shade_buffer/number/error helpers. */
typedef struct {
    ayatsuri_follow_coeff coeff;
    ayatsuri_follow_state state;
    ayatsuri_follow_goal goal;
    float initial[6];
    bool busy;
} pocket_follow_actor;
typedef struct {
    uint32_t phase[3],increment[3],seed;
    float value;
    bool busy;
} pocket_follow_driver;
static JSClassID pocket_follow_class,pocket_drive_class;
static pocket_class_owner_t pocket_follow_class_owner, pocket_drive_class_owner;

static JSValue managed_follow_error(JSContext *ctx,const char *op,ayatsuri_status status){
    if(JS_HasException(ctx))return JS_EXCEPTION;
    return status==AYATSURI_RANGE?fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"float32 follow range"):
        invalid(ctx,op,"invalid follow argument");
}
static ayatsuri_status managed_follow_pair(JSContext *ctx,JSValueConst value,float out[2]){
    shade_view typed;
    if(JS_GetTypedArrayType(value)==JS_TYPED_ARRAY_FLOAT32){
        if(!shade_buffer(ctx,value,JS_TYPED_ARRAY_FLOAT32,false,&typed)||typed.bytes!=8)return AYATSURI_INVALID;
        memcpy(out,typed.data,8);
        return isfinite(out[0])&&isfinite(out[1])?AYATSURI_OK:AYATSURI_INVALID;
    }
    if(!JS_IsObject(value))return AYATSURI_INVALID;
    JSValue length=JS_GetPropertyStr(ctx,value,"length");double n;
    bool valid=!JS_IsException(length)&&number(ctx,length,&n)&&n==2;
    JS_FreeValue(ctx,length);
    if(!valid)return AYATSURI_INVALID;
    for(unsigned i=0;i<2;++i){
        JSValue component=JS_GetPropertyUint32(ctx,value,i);double x;
        valid=!JS_IsException(component)&&number(ctx,component,&x)&&isfinite(x);
        JS_FreeValue(ctx,component);
        if(!valid)return AYATSURI_INVALID;
        const ayatsuri_status status=ayatsuri_follow_narrow(x,&out[i]);
        if(status!=AYATSURI_OK)return status;
    }
    return AYATSURI_OK;
}
static pocket_follow_actor *managed_follow_enter(JSContext *ctx,JSValueConst self,const char *op){
    pocket_follow_actor *actor=JS_GetOpaque2(ctx,self,pocket_follow_class);
    if(!actor)return NULL;
    if(actor->busy){fail(ctx,op,POCKET_ERR_BUSY,"busy follower");return NULL;}
    actor->busy=true;return actor;
}
static pocket_follow_driver *managed_drive_enter(JSContext *ctx,JSValueConst self,const char *op){
    pocket_follow_driver *driver=JS_GetOpaque2(ctx,self,pocket_drive_class);
    if(!driver)return NULL;
    if(driver->busy){fail(ctx,op,POCKET_ERR_BUSY,"busy drive");return NULL;}
    driver->busy=true;return driver;
}
static JSValue managed_follow_retarget(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.follow.retarget";pocket_follow_actor *a=managed_follow_enter(ctx,self,op);
    if(!a)return JS_EXCEPTION;
    float target[2];ayatsuri_status status=argc==1?managed_follow_pair(ctx,argv[0],target):AYATSURI_INVALID;
    if(status==AYATSURI_OK)status=ayatsuri_follow_retarget(target,&a->goal);
    a->busy=false;return status==AYATSURI_OK?JS_UNDEFINED:managed_follow_error(ctx,op,status);
}
static JSValue managed_follow_impulse(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.follow.impulse";pocket_follow_actor *a=managed_follow_enter(ctx,self,op);
    if(!a)return JS_EXCEPTION;
    float impulse[2];ayatsuri_status status=argc==1?managed_follow_pair(ctx,argv[0],impulse):AYATSURI_INVALID;
    if(status==AYATSURI_OK)status=ayatsuri_follow_impulse(&a->coeff,impulse,&a->state);
    a->busy=false;return status==AYATSURI_OK?JS_UNDEFINED:managed_follow_error(ctx,op,status);
}
static JSValue managed_follow_step(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.follow.step";pocket_follow_actor *a=managed_follow_enter(ctx,self,op);
    if(!a)return JS_EXCEPTION;
    float force[2]={0,0};ayatsuri_status status=AYATSURI_OK;
    if(argc>1)status=AYATSURI_INVALID;
    else if(argc&&!JS_IsUndefined(argv[0]))status=managed_follow_pair(ctx,argv[0],force);
    if(status==AYATSURI_OK)status=ayatsuri_follow_step(&a->coeff,&a->goal,force,&a->state);
    a->busy=false;return status==AYATSURI_OK?JS_UNDEFINED:managed_follow_error(ctx,op,status);
}
static JSValue managed_follow_sample(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.follow.sample";pocket_follow_actor *a=managed_follow_enter(ctx,self,op);
    if(!a)return JS_EXCEPTION;
    JSValue fields[3]={JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};shade_view views[3];bool valid=argc==1&&JS_IsObject(argv[0]);
    const char *names[3]={"position","velocity","goal"};
    /* All accessors run under the per-instance guard, before borrowing any
     * backing memory. A getter may throw or detach another output buffer. */
    for(unsigned i=0;valid&&i<3;++i){fields[i]=JS_GetPropertyStr(ctx,argv[0],names[i]);valid=!JS_IsException(fields[i]);}
    for(unsigned i=0;valid&&i<3;++i)valid=shade_buffer(ctx,fields[i],JS_TYPED_ARRAY_FLOAT32,true,&views[i])&&views[i].bytes==8;
    if(valid)valid=!shade_overlap(&views[0],&views[1])&&!shade_overlap(&views[0],&views[2])&&!shade_overlap(&views[1],&views[2]);
    if(valid){memcpy(views[0].data,a->state.q,8);memcpy(views[1].data,a->state.v,8);memcpy(views[2].data,a->goal.q,8);}
    for(unsigned i=0;i<3;++i)JS_FreeValue(ctx,fields[i]);
    a->busy=false;
    return valid?JS_DupValue(ctx,argv[0]):managed_follow_error(ctx,op,AYATSURI_INVALID);
}
static JSValue managed_follow_reset(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.follow.reset";pocket_follow_actor *a=managed_follow_enter(ctx,self,op);
    if(!a)return JS_EXCEPTION;
    if(argc){a->busy=false;return invalid(ctx,op,"reset takes no arguments");}
    memcpy(&a->state,a->initial,16);memcpy(&a->goal,a->initial+4,8);
    a->busy=false;return JS_UNDEFINED;
}
static void managed_follow_finalizer(JSRuntime *rt,JSValue value){
    js_free_rt(rt,JS_GetOpaque(value,pocket_follow_class));
}
static void managed_drive_finalizer(JSRuntime *rt,JSValue value){
    js_free_rt(rt,JS_GetOpaque(value,pocket_drive_class));
}
static const JSClassDef managed_follow_definition={.class_name="AyatsuriFollow",.finalizer=managed_follow_finalizer};
static const JSClassDef managed_drive_definition={.class_name="AyatsuriDrive",.finalizer=managed_drive_finalizer};
static const JSCFunctionListEntry managed_follow_methods[]={
    JS_CFUNC_DEF("retarget",1,managed_follow_retarget),JS_CFUNC_DEF("impulse",1,managed_follow_impulse),
    JS_CFUNC_DEF("step",1,managed_follow_step),JS_CFUNC_DEF("sample",1,managed_follow_sample),JS_CFUNC_DEF("reset",0,managed_follow_reset)};

static void managed_drive_seed(pocket_follow_driver *driver,uint32_t seed){
    uint32_t current=seed;driver->seed=seed;
    for(unsigned i=0;i<3;++i){current=UINT32_C(1664525)*current+UINT32_C(1013904223);driver->phase[i]=current;}
    driver->value=0;
}
static bool managed_drive_seed_argument(JSContext *ctx,JSValueConst value,uint32_t *out){
    double n;if(!number(ctx,value,&n)||!whole(n,0,UINT32_MAX))return false;
    *out=(uint32_t)n;return true;
}
static JSValue managed_drive_step(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argv;const char *op="ayatsuri.drive.step";pocket_follow_driver *d=managed_drive_enter(ctx,self,op);
    if(!d)return JS_EXCEPTION;
    if(argc){d->busy=false;return invalid(ctx,op,"step takes no arguments");}
    uint32_t phase[3];float sample=0;memcpy(phase,d->phase,sizeof phase);
    const ayatsuri_status status=ayatsuri_follow_drive(phase,d->increment,&sample);
    JSValue result=status==AYATSURI_OK?JS_NewFloat64(ctx,sample):managed_follow_error(ctx,op,status);
    if(!JS_IsException(result)){memcpy(d->phase,phase,sizeof phase);d->value=sample;}
    d->busy=false;return result;
}
static JSValue managed_drive_sample(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.drive.sample";pocket_follow_driver *d=managed_drive_enter(ctx,self,op);
    if(!d)return JS_EXCEPTION;
    shade_view output;const bool valid=argc==1&&shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,true,&output)&&output.bytes==4;
    if(valid)memcpy(output.data,&d->value,4);
    d->busy=false;return valid?JS_DupValue(ctx,argv[0]):managed_follow_error(ctx,op,AYATSURI_INVALID);
}
static JSValue managed_drive_snapshot(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.drive.snapshot";pocket_follow_driver *d=managed_drive_enter(ctx,self,op);
    if(!d)return JS_EXCEPTION;
    JSValue fields[2]={JS_UNDEFINED,JS_UNDEFINED};shade_view views[2];bool valid=argc==1&&JS_IsObject(argv[0]);
    if(valid){fields[0]=JS_GetPropertyStr(ctx,argv[0],"phases");valid=!JS_IsException(fields[0]);}
    if(valid){fields[1]=JS_GetPropertyStr(ctx,argv[0],"increments");valid=!JS_IsException(fields[1]);}
    for(unsigned i=0;valid&&i<2;++i)valid=shade_buffer(ctx,fields[i],JS_TYPED_ARRAY_UINT32,true,&views[i])&&views[i].bytes==12;
    if(valid)valid=!shade_overlap(&views[0],&views[1]);
    /* Scalar diagnostic output may invoke a setter. Do it before any typed
     * write, then reacquire views because that setter can detach a buffer. */
    if(valid)valid=JS_SetPropertyStr(ctx,argv[0],"value",JS_NewFloat64(ctx,d->value))>=0;
    for(unsigned i=0;valid&&i<2;++i)valid=shade_buffer(ctx,fields[i],JS_TYPED_ARRAY_UINT32,true,&views[i])&&views[i].bytes==12;
    if(valid){memcpy(views[0].data,d->phase,12);memcpy(views[1].data,d->increment,12);}
    JS_FreeValue(ctx,fields[0]);JS_FreeValue(ctx,fields[1]);d->busy=false;
    return valid?JS_DupValue(ctx,argv[0]):managed_follow_error(ctx,op,AYATSURI_INVALID);
}
static JSValue managed_drive_reset(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="ayatsuri.drive.reset";pocket_follow_driver *d=managed_drive_enter(ctx,self,op);
    if(!d)return JS_EXCEPTION;
    uint32_t seed=d->seed;const bool valid=argc<=1&&(!argc||JS_IsUndefined(argv[0])||managed_drive_seed_argument(ctx,argv[0],&seed));
    if(valid)managed_drive_seed(d,seed);
    d->busy=false;return valid?JS_UNDEFINED:invalid(ctx,op,"uint32 seed required");
}
static const JSCFunctionListEntry managed_drive_methods[]={
    JS_CFUNC_DEF("step",0,managed_drive_step),JS_CFUNC_DEF("sample",1,managed_drive_sample),
    JS_CFUNC_DEF("snapshot",1,managed_drive_snapshot),JS_CFUNC_DEF("reset",1,managed_drive_reset)};
static bool managed_follow_class_ready(JSContext *ctx,bool drive){
    JSRuntime *rt=JS_GetRuntime(ctx);JSClassID *id=drive?&pocket_drive_class:&pocket_follow_class;
    pocket_class_owner_t *owner=drive?&pocket_drive_class_owner:&pocket_follow_class_owner;
    if(!pocket_api_class_ready(rt,owner,id))JS_NewClassID(rt,id);
    /* Registration can fail after the session owner/id were recorded. Retry
     * that allocated id instead of treating an owner match as registration. */
    if(!JS_IsRegisteredClass(rt,*id)&&JS_NewClass(rt,*id,drive?&managed_drive_definition:&managed_follow_definition)<0){
        if(!JS_HasException(ctx))JS_ThrowOutOfMemory(ctx);
        return false;
    }
    JSValue proto=JS_GetClassProto(ctx,*id);const bool ready=!JS_IsNull(proto);JS_FreeValue(ctx,proto);
    if(ready)return true;
    proto=JS_NewObject(ctx);if(JS_IsException(proto))return false;
    const JSCFunctionListEntry *methods=drive?managed_drive_methods:managed_follow_methods;
    const int count=drive?(int)(sizeof managed_drive_methods/sizeof managed_drive_methods[0]):
        (int)(sizeof managed_follow_methods/sizeof managed_follow_methods[0]);
    if(JS_SetPropertyFunctionList(ctx,proto,methods,count)<0||JS_HasException(ctx)){JS_FreeValue(ctx,proto);return false;}
    JS_SetClassProto(ctx,*id,proto);return true;
}
static ayatsuri_status managed_follow_description_pair(JSContext *ctx,JSValueConst description,
        const char *name,bool optional,float out[2]){
    JSValue value=JS_GetPropertyStr(ctx,description,name);
    ayatsuri_status status=AYATSURI_INVALID;
    if(!JS_IsException(value)){
        if(optional&&JS_IsUndefined(value)){out[0]=out[1]=0;status=AYATSURI_OK;}
        else status=managed_follow_pair(ctx,value,out);
    }
    JS_FreeValue(ctx,value);return status;
}
static ayatsuri_status managed_follow_description_scalar(JSContext *ctx,JSValueConst description,
        const char *name,float *out){
    JSValue value=JS_GetPropertyStr(ctx,description,name);double x;
    ayatsuri_status status=AYATSURI_INVALID;
    if(!JS_IsException(value)){
        if(JS_IsUndefined(value)){*out=0;status=AYATSURI_OK;}
        else if(number(ctx,value,&x)&&isfinite(x))status=ayatsuri_follow_narrow(x,out);
    }
    JS_FreeValue(ctx,value);return status;
}
static JSValue js_ayatsuri_follow_create(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.followCreate";
    pocket_follow_actor candidate={0};ayatsuri_follow_config raw;
    ayatsuri_status status=AYATSURI_OK;
    if(argc==1&&JS_IsObject(argv[0])){
        /* Resolve the artist description completely on the stack. No class,
         * payload, or pointer to this caller-owned object is retained while
         * its accessors can still throw or call another independent factory. */
        status=managed_follow_description_pair(ctx,argv[0],"inertia",false,raw.inertia);
        if(status==AYATSURI_OK)status=managed_follow_description_pair(ctx,argv[0],"stiffness",false,raw.stiffness);
        if(status==AYATSURI_OK)status=managed_follow_description_pair(ctx,argv[0],"damping",false,raw.damping);
        if(status==AYATSURI_OK)status=managed_follow_description_scalar(ctx,argv[0],"couplingStiffness",&raw.coupling_stiffness);
        if(status==AYATSURI_OK)status=managed_follow_description_scalar(ctx,argv[0],"couplingDamping",&raw.coupling_damping);
        if(status==AYATSURI_OK)status=managed_follow_description_pair(ctx,argv[0],"position",true,candidate.initial);
        if(status==AYATSURI_OK)status=managed_follow_description_pair(ctx,argv[0],"velocity",true,candidate.initial+2);
        if(status==AYATSURI_OK)status=managed_follow_description_pair(ctx,argv[0],"goal",true,candidate.initial+4);
        if(status!=AYATSURI_OK)return managed_follow_error(ctx,op,status);
    }else if(argc==2){
        shade_view config,initial;
        if(!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,false,&config)||config.bytes!=32||
           !shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,false,&initial)||initial.bytes!=24)
            return invalid(ctx,op,"expected Float32 config[8], initial q/v/goal[6]");
        memcpy(&raw,config.data,sizeof raw);memcpy(candidate.initial,initial.data,sizeof candidate.initial);
        for(unsigned i=0;i<6;++i)if(!isfinite(candidate.initial[i]))return invalid(ctx,op,"finite initial q/v/goal required");
    }else return invalid(ctx,op,"expected follow description or config/initial Float32 packets");
    status=ayatsuri_follow_prepare(&raw,&candidate.coeff);
    if(status==AYATSURI_RANGE)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"unsupported follow coefficients: condition must be <= 1000000");
    if(status!=AYATSURI_OK)return managed_follow_error(ctx,op,status);
    memcpy(&candidate.state,candidate.initial,16);memcpy(&candidate.goal,candidate.initial+4,8);
    if(!managed_follow_class_ready(ctx,false))return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowOutOfMemory(ctx);
    pocket_follow_actor *actor=js_mallocz(ctx,sizeof *actor);if(!actor)return JS_ThrowOutOfMemory(ctx);
    *actor=candidate;
    JSValue object=JS_NewObjectClass(ctx,pocket_follow_class);
    if(JS_IsException(object)){js_free(ctx,actor);return object;}
    JS_SetOpaque(object,actor);return object;
}
static JSValue js_ayatsuri_drive_create(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.driveCreate";uint32_t seed;
    if(argc!=1||!managed_drive_seed_argument(ctx,argv[0],&seed))return invalid(ctx,op,"uint32 seed required");
    if(!managed_follow_class_ready(ctx,true))return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowOutOfMemory(ctx);
    pocket_follow_driver *driver=js_mallocz(ctx,sizeof *driver);if(!driver)return JS_ThrowOutOfMemory(ctx);
    driver->increment[0]=22190664;driver->increment[1]=52255435;driver->increment[2]=80888551;
    managed_drive_seed(driver,seed);
    JSValue object=JS_NewObjectClass(ctx,pocket_drive_class);
    if(JS_IsException(object)){js_free(ctx,driver);return object;}
    JS_SetOpaque(object,driver);return object;
}
