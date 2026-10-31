/* Live SourceA uses real QuickJS and the product transform kernel. The backend
 * copies typed input into aligned native packets, then uses the real C shader.
 * Recorder quota/publish semantics remain the separate app-host contract. */
#define main legacy_model_main
#include "test_model_qjs.c"
#undef main
#include "ayatsuri/ayatsuri_transform.h"

static JSValue source_compose(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;shade_view node,parent;const bool has_parent=argc==2&&!JS_IsNull(argv[1])&&!JS_IsUndefined(argv[1]);
    if(argc!=2||!managed_model_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,true,&node)||node.bytes!=84||
       (has_parent&&(!managed_model_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,false,&parent)||(parent.bytes!=48&&parent.bytes!=84))))
        return invalid(ctx,"source.composeNode","node/parent buffers");
    ayatsuri_pose pose;ayatsuri_transform p,result;memcpy(&pose,node.data+48,sizeof pose);
    if(has_parent)memcpy(&p,parent.data,sizeof p);
    ayatsuri_status status=ayatsuri_transform_evaluate(&pose,has_parent?&p:NULL,&result);
    if(status!=AYATSURI_OK)return invalid(ctx,"source.composeNode","invalid transform");
    memcpy(node.data,&result,sizeof result);return JS_UNDEFINED;
}
static JSValue source_blend(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;shade_view v[7];const JSTypedArrayEnum types[7]={JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,
        JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16};
    if(argc!=5&&argc!=7)return invalid(ctx,"source.blend","arity");
    for(int k=0;k<argc;k++)if(!managed_model_buffer(ctx,argv[k],types[k],false,&v[k]))return invalid(ctx,"source.blend","typed view");
    if(!v[0].bytes||v[0].bytes%84||v[0].bytes>672||v[1].bytes!=36||v[2].bytes!=44||v[3].bytes!=6||v[4].bytes!=8||
        (argc==7&&(v[5].bytes!=20||v[6].bytes!=2)))return invalid(ctx,"source.blend","length");
    ksn_proc_blend_ellipsoids p={0};p.header.count=(uint8_t)(v[0].bytes/84);
    memcpy(p.lobes,v[0].data,v[0].bytes);memcpy(p.header.light,v[1].data,36);memcpy(p.header.material,v[2].data,44);
    memcpy(p.header.surface,v[3].data,6);memcpy(p.header.controls,v[4].data,8);
    if(argc==7){ksn_proc_blend_region_mask mask={0};memcpy(&mask,v[5].data,20);memcpy(&mask.color,v[6].data,2);
        return pocket_proc_native_blend_region(ctx,"source.blend",&p,&mask);}
    int l,t,r,b;if(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b))return invalid(ctx,"source.blend","bounds");
    return pocket_proc_native_blend_ellipsoids(ctx,"source.blend",&p);
}
static void source_load(JSContext *ctx,const char *path){size_t n;char *source=read_file(path,&n);eval_ok(ctx,source,n);free(source);}
static JSValue function(JSContext *ctx,const char *name){JSValue global=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,global,name);JS_FreeValue(ctx,global);CHECK(JS_IsFunction(ctx,fn));return fn;}
static JSValue call(JSContext *ctx,JSValueConst fn,int argc,JSValueConst *argv){
    JSValue r=JS_Call(ctx,fn,JS_UNDEFINED,argc,argv);
    if(JS_IsException(r)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);fprintf(stderr,"SourceA QuickJS: %s\n",s?s:"exception");
        JSValue stack=JS_GetPropertyStr(ctx,e,"stack");const char *st=JS_ToCString(ctx,stack);fprintf(stderr,"%s\n",st?st:"");exit(1);}
    return r;
}
static void named_call(JSContext *ctx,const char *name){JSValue fn=function(ctx,name),r=call(ctx,fn,0,NULL);JS_FreeValue(ctx,r);JS_FreeValue(ctx,fn);}
static uint16_t image_a[KSN_PROC_W*KSN_PROC_H],image_b[KSN_PROC_W*KSN_PROC_H];
static void raster(unsigned start,unsigned count,bool plain,uint16_t *pixels){
    static ksn_proc_frame frame;memset(&frame,0,sizeof frame);frame.ext=1;frame.ready=true;
    for(unsigned j=0;j<count;j++){
        const ksn_proc_blend_ellipsoids *p=&packets[start+j].region.patch;
        const ksn_proc_blend_region_mask *mask=&packets[start+j].region.mask;
        int l,t,r,b;CHECK(ksn_proc_blend_region_bounds(p,mask,&l,&t,&r,&b));
        unsigned words=(unsigned)(plain?KSN_PROC_BLEND_WORDS(p->header.count):KSN_PROC_BLEND_REGION_WORDS(p->header.count));
        CHECK(frame.count+1+words<KSN_PROC_SEGMENTS);
        frame.segments[frame.count++]=(ksn_proc_segment){(int16_t)(l+(plain?KSN_PROC_EXT_BLEND_ELLIPSOIDS:KSN_PROC_EXT_BLEND_REGION)),(int16_t)t,(int16_t)r,(int16_t)b,0};
        uint8_t *dst=(uint8_t *)(frame.segments+frame.count);size_t raw=KSN_PROC_BLEND_BYTES(p->header.count);
        memcpy(dst,p,raw);if(!plain)memcpy(dst+raw,mask,sizeof *mask);frame.count+=(uint16_t)words;
    }
    for(unsigned n=0;n<KSN_PROC_W*KSN_PROC_H;n++)pixels[n]=0x2137;
    CHECK(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
}
int main(int argc,char **argv){
    CHECK(argc==2);fixture=(uint8_t *)read_file(argv[1],&fixture_bytes);reset_classes();
    JSRuntime *rt=JS_NewRuntime2(&allocator,NULL);CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);install(ctx);
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"nativeCompose",JS_NewCFunction(ctx,source_compose,"nativeCompose",2));
    JS_SetPropertyStr(ctx,global,"nativeBlend",JS_NewCFunction(ctx,source_blend,"nativeBlend",7));JS_FreeValue(ctx,global);
    source_load(ctx,"apps/character/character.js");source_load(ctx,"apps/character/character_blend.js");
    source_load(ctx,"apps/lib/ayatsuri_local_plane.js");source_load(ctx,"tools/ayatsuri/model/test_source_plane_qjs.js");
    named_call(ctx,"setupParity");JSValue sweep=function(ctx,"sweep");double plane_error=0;
    for(unsigned step=0;step<16;step++){
        JSValue arg=JS_NewUint32(ctx,step),r=call(ctx,sweep,1,&arg);JS_FreeValue(ctx,r);JS_FreeValue(ctx,arg);CHECK(packet_count==4);
        for(unsigned j=0;j<2;j++){
            CHECK(!memcmp(&packets[j].region.patch,&packets[2+j].region.patch,sizeof(ksn_proc_blend_ellipsoids)));
            const ksn_proc_blend_region_mask *a=&packets[j].region.mask,*b=&packets[2+j].region.mask;
            CHECK(a->color==b->color&&a->feather==b->feather&&a->reserved==0&&b->reserved==0);
            for(unsigned k=0;k<4;k++){double error=fabs((double)a->plane[k]-b->plane[k]);if(error>plane_error)plane_error=error;CHECK(error<=2e-5*(1+fabs(b->plane[k])));}
        }
        raster(0,2,false,image_a);raster(2,2,false,image_b);CHECK(!memcmp(image_a,image_b,sizeof image_a));
        unsigned drawn=0;for(unsigned p=0;p<KSN_PROC_W*KSN_PROC_H;p++)if(image_a[p]!=0x2137)++drawn;CHECK(drawn>50);
        CHECK(packets[0].region.mask.color==packets[0].region.patch.header.surface[0]);
        raster(0,1,false,image_a);raster(0,1,true,image_b);CHECK(!memcmp(image_a,image_b,sizeof image_a));
    }
    JS_FreeValue(ctx,sweep);JSValue warm=function(ctx,"warmSource");size_t before=allocation_calls;
    for(unsigned n=0;n<1000;n++){JSValue a=JS_NewUint32(ctx,0),r=call(ctx,warm,1,&a);JS_FreeValue(ctx,r);JS_FreeValue(ctx,a);}
    const size_t unchanged_calls=allocation_calls-before;before=allocation_calls;
    for(unsigned n=0;n<1000;n++){JSValue a=JS_NewUint32(ctx,n+1),r=call(ctx,warm,1,&a);JS_FreeValue(ctx,r);JS_FreeValue(ctx,a);}
    const size_t changed_calls=allocation_calls-before;JS_FreeValue(ctx,warm);named_call(ctx,"closeParity");named_call(ctx,"invalidContracts");
    JSValue attempt=function(ctx,"attemptAdd"),recover=function(ctx,"recoverOOM");
    named_call(ctx,"setupOOM");before=allocation_calls;JSValue result=call(ctx,attempt,0,NULL);const size_t sites=allocation_calls-before;JS_FreeValue(ctx,result);
    JSValue no=JS_FALSE;result=call(ctx,recover,1,&no);JS_FreeValue(ctx,result);unsigned failures=0,successful_faults=0,pending_successes=0;
    for(size_t site=1;site<=sites;site++){
        named_call(ctx,"setupOOM");CHECK(packet_count==1);typeof(packets[0]) snapshot=packets[0];
        const size_t old_injected=injected_ooms;fail_at=allocation_calls+site;result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL);fail_at=0;
        const bool failed=JS_IsException(result);if(failed){++failures;JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}else if(injected_ooms!=old_injected)++successful_faults;
        /* Match the existing model fault harness: this QuickJS can return a
         * successful JS result after a recoverable allocator failure while
         * retaining an OOM exception. Record it explicitly and clear only at
         * the host call boundary; this is not a claim of same-turn recovery. */
        if(JS_HasException(ctx)){CHECK(!failed);++pending_successes;JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
        JS_FreeValue(ctx,result);CHECK(injected_ooms==old_injected+1);CHECK(packet_count==1&&!memcmp(&snapshot,&packets[0],sizeof snapshot));
        JSValue arg=JS_NewBool(ctx,failed);result=call(ctx,recover,1,&arg);JS_FreeValue(ctx,result);JS_FreeValue(ctx,arg);JS_RunGC(rt);
        if(packet_count!=2||memcmp(&packets[0],&packets[1],sizeof packets[0])){
            fprintf(stderr,"facade recovery packet mismatch site=%zu failed=%d count=%u\n",site,failed,packet_count);
            for(unsigned k=0;k<sizeof packets[0]/4;k++){float a,b;memcpy(&a,(const uint8_t *)&packets[0]+4*k,4);memcpy(&b,(const uint8_t *)&packets[1]+4*k,4);if(memcmp((const uint8_t *)&packets[0]+4*k,(const uint8_t *)&packets[1]+4*k,4))fprintf(stderr,"packet offset=%u intended=%g actual=%g\n",4*k,a,b);}
        }
        CHECK(packet_count==2&&!memcmp(&packets[0],&packets[1],sizeof packets[0]));
    }
    CHECK(failures>0);
    /* Keep the engine's success-with-pending-OOM behavior visible. No host
     * cleanup occurs between add and evaluation in this whole JS call. */
    named_call(ctx,"setupOOM");typeof(packets[0]) old_snapshot=packets[0];
    JSValue same_turn=function(ctx,"attemptSameTurn");fail_at=allocation_calls+15;
    result=JS_Call(ctx,same_turn,JS_UNDEFINED,0,NULL);fail_at=0;
    const bool same_turn_failed=JS_IsException(result),same_turn_pending=JS_HasException(ctx);
    global=JS_GetGlobalObject(ctx);JSValue phase_value=JS_GetPropertyStr(ctx,global,"sameTurnPhase");uint32_t phase=0;
    CHECK(JS_ToUint32(ctx,&phase,phase_value)==0);JS_FreeValue(ctx,phase_value);JS_FreeValue(ctx,global);
    if(same_turn_pending){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
    if(same_turn_failed)CHECK(packet_count==1&&!memcmp(&old_snapshot,&packets[0],sizeof old_snapshot));
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,same_turn);
    JSValue failed_add=JS_NewBool(ctx,same_turn_failed&&phase==1);result=call(ctx,recover,1,&failed_add);JS_FreeValue(ctx,result);JS_FreeValue(ctx,failed_add);
    printf("SOURCE_PLANE_PENDING_OOM site=15 sameTurnThrow=%d phase=%u pending=%d oldSnapshotPreserved=%d boundaryRecovery=true\n",same_turn_failed,phase,same_turn_pending,same_turn_failed);
    /* The base utility control excludes the optional facade and its pattern.
     * The same authoring volume, allocator, backend and boundary policy apply. */
    named_call(ctx,"setupPlainOOM");before=allocation_calls;result=call(ctx,attempt,0,NULL);const size_t plain_sites=allocation_calls-before;JS_FreeValue(ctx,result);
    result=call(ctx,recover,1,&no);JS_FreeValue(ctx,result);unsigned plain_failures=0,plain_pending=0,plain_successful_faults=0;
    for(size_t site=1;site<=plain_sites;site++){
        named_call(ctx,"setupPlainOOM");CHECK(packet_count==1);typeof(packets[0]) snapshot=packets[0];const size_t old_injected=injected_ooms;
        fail_at=allocation_calls+site;result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL);fail_at=0;
        const bool failed=JS_IsException(result);if(failed)++plain_failures;else if(injected_ooms!=old_injected)++plain_successful_faults;
        if(JS_HasException(ctx)){if(!failed)++plain_pending;JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
        JS_FreeValue(ctx,result);CHECK(injected_ooms==old_injected+1);CHECK(packet_count==1&&!memcmp(&snapshot,&packets[0],sizeof snapshot));
        JSValue arg=JS_NewBool(ctx,failed);result=call(ctx,recover,1,&arg);JS_FreeValue(ctx,result);JS_FreeValue(ctx,arg);JS_RunGC(rt);
        CHECK(packet_count==2&&!memcmp(&packets[0],&packets[1],sizeof packets[0]));
    }
    printf("SOURCE_PLANE_BASE_CONTROL oomSites=%zu rejected=%u recoveredFaults=%u pendingSuccesses=%u pattern=false facade=false boundaryRecovery=true\n",plain_sites,plain_failures,plain_successful_faults,plain_pending);
    JS_FreeValue(ctx,attempt);JS_FreeValue(ctx,recover);JS_RunGC(rt);const size_t no_handles=retained;
    named_call(ctx,"holdDisposed");JS_RunGC(rt);const size_t held_handles=retained;
    named_call(ctx,"releaseHeld");JS_RunGC(rt);const size_t released_handles=retained;
    CHECK(held_handles>=released_handles&&held_handles-released_handles<4096);
    printf("SOURCE_PLANE_QJS_PASS sweeps=16 geometry=byte-identical pixelPairs=32 planeMaxAbs=%.9g oomSites=%zu rejected=%u recoveredFaults=%u pendingSuccesses=%u warmUnchangedCalls=%zu warmPoseCalls=%zu held8Bytes=%zu baselineDelta=%lld\n",
        plane_error,sites,failures,successful_faults,pending_successes,unchanged_calls,changed_calls,held_handles-released_handles,(long long)released_handles-(long long)no_handles);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);CHECK(retained==0);free(fixture);return 0;
}
