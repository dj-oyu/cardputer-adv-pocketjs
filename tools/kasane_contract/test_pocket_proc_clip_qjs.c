/* Reuse actual adapter ports/stubs, not its MEGADEMO main or JS workload. */
#define main procedural_baseline_fixture_main
#include "test_pocket_proc_qjs.c"
#undef main
static void same_mark(JSContext *ctx,const pocket_proc_mark *before){
    pocket_proc_mark after;JSValue v=pocket_proc_native_begin(ctx,"clip-test",&after);
    REQUIRE(!JS_IsException(v));JS_FreeValue(ctx,v);
    REQUIRE(!memcmp(before,&after,sizeof after));
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx),hone=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK&&pocket_shape_fill(ctx,hone,NULL)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0&&JS_SetPropertyStr(ctx,pocket,"hone",hone)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);JS_FreeValue(ctx,global);
    eval_ok(ctx,"globalThis.P=pocket.kasane.procedural;globalThis.H=pocket.hone;"
        "globalThis.buf=new ArrayBuffer(148);globalThis.lit=new Float32Array([0,0,1,0,0,1,1,1,1]);"
        "globalThis.xy=new Float32Array([20,10,60,10,60,50,20,50]);"
        "globalThis.initClip=()=>{new Float32Array(buf,0,8).set([20,0,0,40,0,20,0,30]);"
        "new Float32Array(buf,32,9).set([1,0,0,0,1,0,0,0,1]);new Float32Array(buf,68,8).set([1,0,0,16,0,1,.5,.25]);"
        "new Float32Array(buf,100,9).set([1,0,0,0,1,0,0,0,1]);new Uint16Array(buf,136,5).set([65535,0,255,255,0]);"
        "lit.set([0,0,1,0,0,1,1,1,1]);xy.set([20,10,60,10,60,50,20,50]);};"
        "initClip();P.beginFrame(31);H.ellipsoidClippedPacked(buf,0,lit,xy);P.commit();");
    REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);
    eval_ok(ctx,"new Uint8Array(buf).fill(0);lit.fill(NaN);xy.fill(NaN)");
    REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));
    pocket_proc_present_result(KSN_OK);
    /* Identical bbox, changed polygon: signatures must include all vertices. */
    eval_ok(ctx,"initClip();xy.set([40,10,60,30,40,50,20,30]);P.beginFrame(31);H.ellipsoidClippedPacked(buf,0,lit,xy);P.commit()");
    uint32_t changed;int16_t x0[17],x1[17];REQUIRE(pocket_proc_band_damage(0,&changed,x0,x1)&&changed);
    REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&memcmp(actual,expected,sizeof actual));
    memcpy(previous,actual,sizeof previous);pocket_proc_present_result(KSN_IO);
    REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));
    eval_error(ctx,"P.beginFrame(31)");pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"initClip();P.beginFrame(31);H.ellipsoidClippedPacked(buf,0,lit,xy)");
    pocket_proc_mark mark;JSValue result=pocket_proc_native_begin(ctx,"clip-test",&mark);REQUIRE(!JS_IsException(result));JS_FreeValue(ctx,result);
    eval_ok(ctx,"H.ellipsoidClippedPacked(buf,0,lit,new Float32Array(0))");same_mark(ctx,&mark);
    eval_error(ctx,"new Float32Array(buf,32,9).fill(0);H.ellipsoidClippedPacked(buf,0,lit,new Float32Array(0))");same_mark(ctx,&mark);
    eval_ok(ctx,"initClip()");
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,0,lit,new Float32Array([0,0,1,1,2,2]))");same_mark(ctx,&mark);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,0,lit,new Float32Array([0,0,20,20,0,20,20,0]))");same_mark(ctx,&mark);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,0,lit,new Float32Array(4))");same_mark(ctx,&mark);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,-1,lit,xy)");same_mark(ctx,&mark);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,1,lit,xy)");same_mark(ctx,&mark);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,0,lit,new Float64Array(8))");same_mark(ctx,&mark);
    eval_ok(ctx,"if(typeof SharedArrayBuffer==='function'){const shared=new Float32Array(new SharedArrayBuffer(32));shared.set(xy);let caught=false;try{H.ellipsoidClippedPacked(buf,0,lit,shared)}catch(e){caught=true}if(!caught)throw Error('shared clip accepted')}");same_mark(ctx,&mark);
    eval_ok(ctx,"if(typeof ArrayBuffer.prototype.transfer==='function'){const d=new Float32Array(8);d.buffer.transfer();let caught=false;try{H.ellipsoidClippedPacked(buf,0,lit,d)}catch(e){caught=true}if(!caught)throw Error('detached clip accepted')}");same_mark(ctx,&mark);
    eval_ok(ctx,"P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
    /* An unaligned packed source remains a byte snapshot, with ignored pad. */
    eval_ok(ctx,"globalThis.odd=new ArrayBuffer(149);new Uint8Array(odd,1,148).set(new Uint8Array(buf));P.beginFrame(31);H.ellipsoidClippedPacked(odd,1,lit,xy);P.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
    /* 37 * 27 entries fit 1024; the next append fails without poisoning the
     * candidate, and all 37 previous snapshots can still commit. */
    memcpy(previous,expected,sizeof previous);
    eval_ok(ctx,"P.beginFrame(31);for(let k=0;k<37;k++)H.ellipsoidClippedPacked(buf,0,lit,xy);P.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,135,expected)==KSN_OK);pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"P.beginFrame(31);for(let k=0;k<37;k++)H.ellipsoidClippedPacked(buf,0,lit,xy)");
    result=pocket_proc_native_begin(ctx,"clip-test",&mark);REQUIRE(!JS_IsException(result));JS_FreeValue(ctx,result);REQUIRE(mark.count==37*KSN_PROC_CLIPPED_ENTRIES);
    eval_error(ctx,"H.ellipsoidClippedPacked(buf,0,lit,xy)");same_mark(ctx,&mark);
    eval_ok(ctx,"P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,expected,sizeof actual));pocket_proc_present_result(KSN_OK);
    /* Explicit EMPTY clears the old image through old/new damage, then a new
     * clipped call recovers without any remembered clip state. */
    eval_ok(ctx,"P.beginFrame(31);H.ellipsoidClippedPacked(buf,0,lit,new Float32Array(0));P.commit()");
    REQUIRE(pocket_proc_band_damage(0,&changed,x0,x1)&&changed);REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK);
    for(unsigned k=0;k<240*135;k++)REQUIRE(actual[k]==31);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"P.beginFrame(31);H.ellipsoidClippedPacked(buf,0,lit,xy);P.commit()");REQUIRE(pocket_proc_backdrop(NULL,0,135,actual)==KSN_OK&&!memcmp(actual,previous,sizeof actual));pocket_proc_present_result(KSN_OK);
    pocket_proc_reset();pocket_proc_host_classes_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("CLIP_QJS_HOST pointer_bits=%zu ellipsoid_bytes=%zu clipped_bytes=%zu\n",sizeof(void*)*8,sizeof(ksn_proc_ellipsoid),sizeof(ksn_proc_clipped_ellipsoid));
    puts("PASS clipped real QuickJS: snapshot, malformed/shared/detached, EMPTY validation, changed-vertex damage, IO repair, unaligned packed input, quota NOT_APPLIED candidate commit/recovery");return 0;
}
