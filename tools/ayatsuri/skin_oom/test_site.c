#define SKIN_JS_MAIN retained_skin_main
#include "../skin/test_motion.c"
#undef SKIN_JS_MAIN

static bool allocation_armed, fail_allocation, observe_core;
static size_t allocation_size, allocation_hits;
static void *allocation_pointer, *allocation_caller;
static ayatsuri_model_instance *actor;
static unsigned core_calls;
extern void *__real_js_malloc(JSContext *, size_t);
void *__wrap_js_malloc(JSContext *ctx, size_t bytes) {
    if (allocation_armed && bytes == allocation_size) {
        allocation_hits++;
        allocation_caller = __builtin_return_address(0);
        if (fail_allocation) { JS_ThrowOutOfMemory(ctx); return NULL; }
        allocation_pointer = __real_js_malloc(ctx, bytes);
        return allocation_pointer;
    }
    return __real_js_malloc(ctx, bytes);
}
extern ayatsuri_model_status __real_ayatsuri_model_instance_evaluate_meshes(ayatsuri_model_instance *, const void *, uint32_t, const float[9], const float[6], const ayatsuri_model_mesh_geometry *, size_t);
ayatsuri_model_status __wrap_ayatsuri_model_instance_evaluate_meshes(ayatsuri_model_instance *i, const void *o, uint32_t g, const float c[9], const float l[6], const ayatsuri_model_mesh_geometry *m, size_t n) {
    if (observe_core) { actor=i; core_calls++; }
    return __real_ayatsuri_model_instance_evaluate_meshes(i,o,g,c,l,m,n);
}
extern ayatsuri_model_status __real_ayatsuri_model_instance_evaluate(ayatsuri_model_instance *, const void *, uint32_t, const float[9], const float[6]);
ayatsuri_model_status __wrap_ayatsuri_model_instance_evaluate(ayatsuri_model_instance *i, const void *o, uint32_t g, const float c[9], const float l[6]) {
    if (observe_core) { actor=i; core_calls++; }
    return __real_ayatsuri_model_instance_evaluate(i,o,g,c,l);
}
static void arm(size_t bytes, bool fail) {
    allocation_size=bytes; allocation_hits=0; allocation_pointer=NULL;
    allocation_caller=NULL; allocation_armed=true; fail_allocation=fail;
}
static size_t bytes_for(unsigned parts, unsigned joints, bool batch) {
    return (batch ? _Alignof(ayatsuri_model_mesh_geometry)-1+parts*sizeof(ayatsuri_model_mesh_geometry) : 0)
        +parts*4*(sizeof(ayatsuri_skin_vertex)+sizeof(ayatsuri_skin_result))
        +parts*joints*sizeof(ayatsuri_transform);
}
static uint16_t saved_panel[240*135];
int main(int argc, char **argv) {
    CHECK(argc==4); strip_rows=(unsigned)atoi(argv[1]); g_ksn_image_rows=atoi(argv[2]); delayed=true;
    JSRuntime *rt=JS_NewRuntime(); CHECK(rt);
    JSContext *ctx=JS_NewContext(rt); CHECK(ctx&&JS_PocketHarden(ctx)==0); JS_SetMemoryLimit(rt,160*1024);
    host_capabilities_clear(); CHECK(shiki_heap_begin(0));
    CHECK(pocket_kasane_install(ctx,NULL)==ESP_OK); CHECK(pocket_shape_install(ctx,NULL)==ESP_OK);
    expose_fixture(ctx,argv[3],"plain","fixture");
    eval(ctx,"globalThis.pocket={kasane,ayatsuri};globalThis.P=kasane.shiki;const resource=P.resource();kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});})");
    CHECK(present()==KSN_OK); motion_file(ctx,"apps/lib/ayatsuri_model.js"); motion_file(ctx,"tools/ayatsuri/skin_oom/scenario.js");
    JSValue fresh=function(ctx,"fresh"), attempt=function(ctx,"attempt"), failed=function(ctx,"failed"), succeeded=function(ctx,"succeeded");
    unsigned scenarios=0;
    for (unsigned mode=0;mode<4;mode++) for (unsigned phase=0;phase<3;phase++) {
        const bool growth=phase>0, dirty=phase==2;
        const bool batch=mode>=2;
        size_t small=bytes_for(1,1,batch), large=bytes_for(batch?2:1,2,batch);
        call(ctx,fresh,JS_NewInt32(ctx,(int)mode)); actor=NULL; core_calls=0; observe_core=true;
        void *old_work=NULL;
        if (growth) {
            arm(small,false); eval(ctx,"warm()"); allocation_armed=false;
            CHECK(allocation_hits==1&&allocation_pointer); old_work=allocation_pointer;
        } else eval(ctx,"cold()");
        CHECK(actor&&core_calls>=1&&present()==KSN_OK); memcpy(saved_panel,panel,sizeof panel);
        /* Commit a distinguishable image B, then leave image A pending.
         * An erroneous fallback to the old committed B must not pass. */
        eval(ctx,"draw(18)"); CHECK(present()==KSN_OK&&memcmp(saved_panel,panel,sizeof panel)!=0);
        eval(ctx,"draw()"); CHECK(pocket_proc_pending());
        ayatsuri_model_instance core_before; memcpy(&core_before,actor,sizeof core_before);
        void *arena_before=malloc(actor->arena_bytes); CHECK(arena_before); memcpy(arena_before,actor->arena,actor->arena_bytes);
        void *work_before=growth?malloc(small):NULL; CHECK(!growth||work_before);
        if (growth) memcpy(work_before,old_work,small);
        if (dirty) eval(ctx,"dirty()");
        core_calls=0; arm(large,true);
        JSValue result=JS_Call(ctx,attempt,JS_UNDEFINED,0,NULL); allocation_armed=false;
        CHECK(JS_IsException(result)&&allocation_hits==1&&allocation_caller&&core_calls==0);
        JSValue error=JS_GetException(ctx); JS_FreeValue(ctx,error); JS_FreeValue(ctx,result);
        if (dirty) {
            /* Scene applies dirty targets before the skin allocation. This is
             * intentionally NOT a full Scene transaction or rollback promise. */
            CHECK(actor->stats.target_changed==core_before.stats.target_changed+1);
            CHECK(actor->mutation_epoch!=core_before.mutation_epoch&&!actor->evaluated);
            float pose[9]; CHECK(ayatsuri_model_instance_read_pose(actor,actor->owner,actor->generation,1,pose)==AYATSURI_MODEL_OK);
            CHECK(pose[0]==1&&pose[1]==0&&pose[2]==0);
            CHECK(memcmp(arena_before,actor->arena,actor->arena_bytes)!=0);
            eval(ctx,"dirtyApplied()");
        } else {
            CHECK(!memcmp(&core_before,actor,sizeof core_before));
            CHECK(!memcmp(arena_before,actor->arena,actor->arena_bytes));
        }
        CHECK(!growth||!memcmp(work_before,old_work,small));
        printf("ALLOCATION_SITE mode=%u phase=%u bytes=%zu pc=%p\n",mode,phase,large,(void *)((uintptr_t)allocation_caller-1));
        call(ctx,failed,JS_NewInt32(ctx,growth?(int)small:0));
        fail_send=true; CHECK(present()==KSN_IO&&pocket_proc_pending());
        CHECK(present()==KSN_OK&&!memcmp(saved_panel,panel,sizeof panel));
        arm(large,false); call(ctx,attempt,JS_UNDEFINED); allocation_armed=false;
        CHECK(allocation_hits==1&&allocation_pointer&&core_calls==1);
        call(ctx,succeeded,JS_NewInt32(ctx,(int)large)); CHECK(present()==KSN_OK);
        CHECK(memcmp(saved_panel,panel,sizeof panel)!=0);
        free(work_before); free(arena_before); observe_core=false; scenarios++;
    }
    eval(ctx,"finish()"); JS_FreeValue(ctx,fresh); JS_FreeValue(ctx,attempt); JS_FreeValue(ctx,failed); JS_FreeValue(ctx,succeeded);
    pocket_shiki_api_reset(); pocket_kasane_reset(); JS_FreeContext(ctx); JS_FreeRuntime(rt); shiki_heap_end(); host_capabilities_clear();
    CHECK(!live_bytes&&!live_blocks);
    printf("SKIN_NATIVE_OOM_PASS rows=%u direct=%d scenarios=%u exact_site_failure clean_core_arena_unchanged dirty_targets_not_rolled_back old_workspace_unchanged no_core_eval pending_distinct_IO_retry recovery_different_pixels ledger0\n",strip_rows,g_ksn_image_rows,scenarios);
    return 0;
}
