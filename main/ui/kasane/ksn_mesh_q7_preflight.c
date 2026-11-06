#include "ksn_mesh_q7_preflight.h"
#include "ksn_mesh_q7_environment.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
/* Two faces suffice for overlapping depth/tie tests. Storage is raw, aligned
 * for the production flexible-array types; no fixed-capacity type punning. */
enum { FACES=2, CASES=25, PACKET_BYTES=1024 };
#define PREP_BYTES (sizeof(ksn_mesh_prepared)+FACES*sizeof(ksn_prepared_triangle))
#define Q7_BYTES (sizeof(ksn_mesh_q7_snapshot)+FACES*sizeof(ksn_mesh_q7_triangle))
typedef struct {
    _Alignas(ksn_mesh_prepared) unsigned char prepared[PREP_BYTES];
    _Alignas(ksn_mesh_q7_snapshot) unsigned char q7[Q7_BYTES], exact[Q7_BYTES];
    unsigned char packets[PACKET_BYTES];
    uint16_t rgb[2][KSN_PROC_W];
    float depth[2][KSN_PROC_W];
    uint8_t owner[2][KSN_PROC_W];
    uint32_t version, active;
    uintptr_t report_address;
    unsigned scene, row, phase;
    bool render_ok[2];
    ksn_q7_environment numeric_environment;
} workspace;
_Static_assert(sizeof(workspace)<16384,"preflight workspace budget");
size_t ksn_mesh_q7_preflight_bytes(void){return sizeof(workspace);}
size_t ksn_mesh_q7_preflight_alignment(void){return _Alignof(workspace);}
static bool probe(void){
    volatile double normal=DBL_MIN, half=.5;
    volatile float tiny=FLT_TRUE_MIN;
    double d=normal*half, f=tiny;
    return d==0x1p-1023 && f==0x1p-149;
}
static bool build(workspace *w,unsigned scene){
    static const float special[]={0.f,-0.f,FLT_TRUE_MIN,-FLT_TRUE_MIN,
        FLT_MIN,-FLT_MIN,FLT_MAX,-FLT_MAX,1.f,-1.f,.5f,
        0x1.000002p0f,0x1.fffffep-1f,1e-30f,1e30f};
    ksn_mesh_snapshot source={.packets={w->packets,sizeof w->packets,0,0},.enabled=true};
    for(unsigned face=0;face<FACES;face++){
        co_vertex v[3]={{0,0,-1,1,63488,31},{8,0,1,1,2016,65535},{0,8,0,1,31,0}};
        if(scene<15){
            for(unsigned k=0;k<3;k++){
                v[k].depth=special[(scene+k)%15];
                v[k].mask=special[(scene*3+k)%15];
            }
        }
        /* The x=4 samples give an exact F32 midpoint in the depth plane;
         * scene 31 is an exact overlapping tie, scene 32 mask cancellation. */
        if(scene==30){v[1].depth=0x1.000002p0f;}
        if(scene==31){for(unsigned k=0;k<3;k++)v[k].mask=1;}
        if(scene==33){for(unsigned k=0;k<3;k++){v[k].x-=4;v[k].y-=4;}}
        if(scene==34){v[1].x=.002f;v[2].y=120;}
        if(scene==35){v[0].x=-4096;v[1].x=4096;v[2].y=100;}
        if(scene==36){for(unsigned k=0;k<3;k++)v[k].x+=300;}
        if(scene==37){v[1].x=239.9f;v[2].y=134.9f;}
        if(scene==38){v[0].mask=-FLT_MAX;v[1].mask=FLT_MAX;v[2].mask=0;}
        if(scene==39){for(unsigned k=0;k<3;k++)v[k].depth=FLT_MAX;}
        if(face && scene!=31 && scene!=30){
            for(unsigned k=0;k<3;k++)v[k].depth=nextafterf(v[k].depth,0);
        }
        uint16_t ix[3]={0,1,2};co_input input={v,ix,0,3,1};
        if(!co_append(&source.packets,&input))return false;
    }
    ksn_mesh_prepared *p=(void *)w->prepared;
    return ksn_mesh_prepare(&source,p,sizeof w->prepared) &&
        p->count<=FACES && ksn_mesh_q7_prepare(p,(void *)w->q7,sizeof w->q7);
}
static void render_arm(workspace *w,ksn_mesh_q7_preflight_report *r,unsigned y,unsigned arm){
    memset(w->rgb[arm],0x35,sizeof w->rgb[arm]);
    memset(w->depth[arm],0xa5,sizeof w->depth[arm]);
    memset(w->owner[arm],0x7e,sizeof w->owner[arm]);
    w->render_ok[arm]=ksn_mesh_q7_render(arm?(void *)w->exact:(void *)w->q7,
        w->rgb[arm],y,1,w->depth[arm],w->owner[arm],1,arm?&r->disabled:&r->enabled);
}
static void compare_buffers(workspace *w,ksn_mesh_q7_preflight_report *r,bool invalid){
    bool a=w->render_ok[0],b=w->render_ok[1];
    r->failure_result_mismatches+=(a!=b);
    r->render_failures+=!a;
    if(invalid && (a||b))r->invariant_failures++;
    for(unsigned x=0;x<KSN_PROC_W;x++){
        if(invalid){
            for(unsigned arm=0;arm<2;arm++){
                uint32_t depth_bits;
                memcpy(&depth_bits,&w->depth[arm][x],sizeof depth_bits);
                if(w->rgb[arm][x]!=UINT16_C(0x3535) ||
                   depth_bits!=UINT32_C(0xa5a5a5a5) ||
                   w->owner[arm][x]!=UINT8_C(0x7e))r->invariant_failures++;
            }
        }
        r->pixels++;
        r->mismatch_count+=w->rgb[0][x]!=w->rgb[1][x] ||
            memcmp(&w->depth[0][x],&w->depth[1][x],sizeof(float)) ||
            w->owner[0][x]!=w->owner[1][x];
    }
}
static void compare(workspace *w,ksn_mesh_q7_preflight_report *r,unsigned y,bool invalid){
    render_arm(w,r,y,0);render_arm(w,r,y,1);compare_buffers(w,r,invalid);
}
static bool checkpoint(ksn_mesh_q7_preflight_report *r,const ksn_q7_environment *caller,
                       ksn_mesh_q7_preflight_checkpoint_fn callback,void *context){
    r->checkpoints++;
    if(!callback)return true;
    ksn_q7_environment numeric;
    if(ksn_q7_environment_get(&numeric)!=0 || ksn_q7_environment_set(caller)!=0){r->invariant_failures++;return false;}
    r->yield_count++;
    bool proceed=callback(context);
    if(!proceed)r->cancelled=true;
    if(ksn_q7_environment_set(&numeric)!=0){r->invariant_failures++;return false;}
    return proceed;
}
bool ksn_mesh_q7_preflight_run_cooperative(void *memory,size_t bytes,
    ksn_mesh_q7_preflight_report *r,
    ksn_mesh_q7_preflight_checkpoint_fn callback,void *context){
    if(!r)return false;
    memset(r,0,sizeof *r);
    r->round_before=r->round_after=fegetround();
    if(!memory || bytes<sizeof(workspace) || (uintptr_t)memory%_Alignof(workspace)){
        r->invariant_failures++;return false;
    }
    uintptr_t start=(uintptr_t)memory,end=(uintptr_t)r;
    if(start>UINTPTR_MAX-sizeof(workspace) || end>UINTPTR_MAX-sizeof *r ||
       (start<end+sizeof *r && end<start+sizeof(workspace))){
        r->invariant_failures++;return false;
    }
    ksn_q7_environment saved;
    if(ksn_q7_environment_get(&saved)!=0){r->invariant_failures++;return false;}
    workspace *w=memory;
    r->subnormal_ok=probe();
    for(unsigned scene=0;scene<CASES;scene++){
        if(!build(w,scene<15?scene:scene+15)){r->invariant_failures++;break;}
        ksn_mesh_q7_snapshot *q=(void *)w->q7,*e=(void *)w->exact;
        memcpy(e,q,sizeof w->q7);
        bool cert=false;
        for(unsigned f=0;f<q->count;f++){cert|=q->triangles[f].cert.valid;e->triangles[f].cert.valid=false;}
        r->cases++;r->certificate_cases+=cert&&!q->fallback;
        r->safe_fallback_cases+=!cert||q->fallback;
        for(unsigned y=0;y<KSN_PROC_H;y++){
            compare(w,r,y,false);
            if(!checkpoint(r,&saved,callback,context))goto restore;
        }
    }
    if(r->cases==CASES){
        /* Identical rejected-call result and untouched buffers, using the
         * production entry point rather than a synthetic failure oracle. */
        compare(w,r,KSN_PROC_H,true);
        r->cases++;
        if(!checkpoint(r,&saved,callback,context))goto restore;
    }
    if(r->cases!=CASES+1 || r->render_failures==0 || !r->safe_fallback_cases)r->invariant_failures++;
    if(r->round_before==FE_TONEAREST && r->subnormal_ok &&
       (!r->certificate_cases || !r->enabled.depth_fast || !r->enabled.depth_exact ||
        !r->enabled.mask_fast || !r->enabled.mask_exact))r->invariant_failures++;
    if(r->disabled.depth_fast || r->disabled.mask_fast)r->invariant_failures++;
    if((r->round_before!=FE_TONEAREST || !r->subnormal_ok) &&
       (r->enabled.depth_fast || r->enabled.mask_fast))r->invariant_failures++;
restore:
    if(ksn_q7_environment_set(&saved)!=0)r->invariant_failures++;
    else r->environment_restored=true;
    r->round_after=fegetround();
    if(r->round_before!=r->round_after){r->environment_restored=false;r->invariant_failures++;}
    return !r->cancelled && !r->mismatch_count && !r->failure_result_mismatches && !r->invariant_failures;
}

bool ksn_mesh_q7_preflight_run(void *memory,size_t bytes,ksn_mesh_q7_preflight_report *r){
    return ksn_mesh_q7_preflight_run_cooperative(memory,bytes,r,NULL,NULL);
}

/* Version is private to this exact state layout. Caller must not persist or
 * copy active state across module versions or workspace addresses. */
#define PREFLIGHT_STATE_VERSION UINT32_C(0x51375003)
static bool incremental_arguments(void *memory,size_t bytes,
                                  ksn_mesh_q7_preflight_report *r){
    if(!memory || !r || bytes<sizeof(workspace) ||
       (uintptr_t)memory%_Alignof(workspace))return false;
    uintptr_t start=(uintptr_t)memory,end=(uintptr_t)r;
    return start<=UINTPTR_MAX-sizeof(workspace) && end<=UINTPTR_MAX-sizeof *r &&
        !(start<end+sizeof *r && end<start+sizeof(workspace));
}
bool ksn_mesh_q7_preflight_begin(void *memory,size_t bytes,ksn_mesh_q7_preflight_report *r){
    if(!incremental_arguments(memory,bytes,r))return false;
    ksn_q7_environment caller;
    if(ksn_q7_environment_get(&caller)!=0)return false;
    workspace *w=memory;
    memset(w,0,sizeof *w);memset(r,0,sizeof *r);
    r->round_before=r->round_after=fegetround();
    r->subnormal_ok=probe();
    bool saved=ksn_q7_environment_get(&w->numeric_environment)==0;
    bool restored=ksn_q7_environment_set(&caller)==0 && fegetround()==r->round_before;
    r->environment_restored=restored;
    if(!saved || !restored){r->invariant_failures++;return false;}
    w->version=PREFLIGHT_STATE_VERSION;w->active=1;w->report_address=(uintptr_t)r;
    return true;
}
static void incremental_validate(ksn_mesh_q7_preflight_report *r){
    if(r->cases!=CASES+1 || r->checkpoints!=CASES*KSN_PROC_H+1 ||
       !r->render_failures || !r->safe_fallback_cases)r->invariant_failures++;
    if(r->round_before==FE_TONEAREST && r->subnormal_ok &&
       (!r->certificate_cases || !r->enabled.depth_fast || !r->enabled.depth_exact ||
        !r->enabled.mask_fast || !r->enabled.mask_exact))r->invariant_failures++;
    if(r->disabled.depth_fast || r->disabled.mask_fast)r->invariant_failures++;
    if((r->round_before!=FE_TONEAREST || !r->subnormal_ok) &&
       (r->enabled.depth_fast || r->enabled.mask_fast))r->invariant_failures++;
}
ksn_mesh_q7_preflight_step_result ksn_mesh_q7_preflight_quantum(
    void *memory,size_t bytes,ksn_mesh_q7_preflight_report *r){
    if(!incremental_arguments(memory,bytes,r))return KSN_MESH_Q7_PREFLIGHT_FAILED;
    workspace *w=memory;
    uint32_t version;
    /* Read the version as object bytes before interpreting any state. */
    memcpy(&version,&w->version,sizeof version);
    if(version!=PREFLIGHT_STATE_VERSION || w->active!=1 ||
       w->report_address!=(uintptr_t)r)return KSN_MESH_Q7_PREFLIGHT_FAILED;
    if(w->scene>CASES || w->row>=KSN_PROC_H || w->phase>=KSN_MESH_Q7_PHASE_COUNT ||
       (w->phase==KSN_MESH_Q7_SETUP && (w->row!=0 || w->scene==CASES)) ||
       r->checkpoints!=(uint64_t)w->scene*KSN_PROC_H+w->row){
        w->active=0;r->invariant_failures++;return KSN_MESH_Q7_PREFLIGHT_FAILED;
    }
    ksn_q7_environment caller;
    if(ksn_q7_environment_get(&caller)!=0){w->active=0;r->invariant_failures++;return KSN_MESH_Q7_PREFLIGHT_FAILED;}
    int caller_round=fegetround();
    ksn_mesh_q7_preflight_step_result result=KSN_MESH_Q7_PREFLIGHT_MORE;
    if(ksn_q7_environment_set(&w->numeric_environment)!=0){r->invariant_failures++;goto step_restore;}
    if(fegetround()!=r->round_before){r->invariant_failures++;goto step_restore;}
    if(w->phase==KSN_MESH_Q7_SETUP){
        if(!build(w,w->scene<15?w->scene:w->scene+15)){r->invariant_failures++;goto step_restore;}
        ksn_mesh_q7_snapshot *q=(void *)w->q7,*e=(void *)w->exact;
        memcpy(e,q,sizeof w->q7);
        bool cert=false;
        for(unsigned f=0;f<q->count;f++){cert|=q->triangles[f].cert.valid;e->triangles[f].cert.valid=false;}
        r->cases++;r->certificate_cases+=cert&&!q->fallback;
        r->safe_fallback_cases+=!cert||q->fallback;
        w->phase=KSN_MESH_Q7_RENDER_ENABLED;
    }else if(w->phase==KSN_MESH_Q7_RENDER_ENABLED || w->phase==KSN_MESH_Q7_RENDER_DISABLED){
        render_arm(w,r,w->scene==CASES?KSN_PROC_H:w->row,
                   w->phase==KSN_MESH_Q7_RENDER_DISABLED);
        w->phase++;
    }else{
        compare_buffers(w,r,w->scene==CASES);r->checkpoints++;
        if(w->scene==CASES){
            r->cases++;incremental_validate(r);result=KSN_MESH_Q7_PREFLIGHT_DONE;
        }else{
            w->phase=KSN_MESH_Q7_RENDER_ENABLED;
            if(++w->row==KSN_PROC_H){
                w->row=0;w->scene++;
                if(w->scene<CASES)w->phase=KSN_MESH_Q7_SETUP;
            }
        }
    }
    r->round_after=fegetround();
    if(r->round_after!=r->round_before || ksn_q7_environment_get(&w->numeric_environment)!=0)r->invariant_failures++;
step_restore:
    if(ksn_q7_environment_set(&caller)!=0 || fegetround()!=caller_round){
        r->environment_restored=false;r->invariant_failures++;
    }
    if(r->invariant_failures || r->mismatch_count || r->failure_result_mismatches || r->cancelled)
        result=KSN_MESH_Q7_PREFLIGHT_FAILED;
    if(result!=KSN_MESH_Q7_PREFLIGHT_MORE)w->active=0;
    return result;
}

bool ksn_mesh_q7_preflight_progress(void *memory,size_t bytes,
    ksn_mesh_q7_preflight_report *r,ksn_mesh_q7_preflight_work *work){
    if(!work || !incremental_arguments(memory,bytes,r))return false;
    workspace *w=memory;uint32_t version;
    memcpy(&version,&w->version,sizeof version);
    if(version!=PREFLIGHT_STATE_VERSION || w->active!=1 ||
       w->report_address!=(uintptr_t)r || w->scene>CASES || w->row>=KSN_PROC_H ||
       w->phase>=KSN_MESH_Q7_PHASE_COUNT ||
       (w->phase==KSN_MESH_Q7_SETUP && (w->row!=0 || w->scene==CASES)) ||
       r->checkpoints!=(uint64_t)w->scene*KSN_PROC_H+w->row)return false;
    *work=(ksn_mesh_q7_preflight_work){w->phase,w->scene,
        w->scene==CASES?39:w->scene<15?w->scene:w->scene+15,
        w->scene==CASES?KSN_PROC_H:w->row};
    return true;
}
ksn_mesh_q7_preflight_step_result ksn_mesh_q7_preflight_step(
    void *memory,size_t bytes,ksn_mesh_q7_preflight_report *r){
    ksn_mesh_q7_preflight_work work;
    if(!ksn_mesh_q7_preflight_progress(memory,bytes,r,&work))return KSN_MESH_Q7_PREFLIGHT_FAILED;
    uint64_t before=r->checkpoints;
    ksn_mesh_q7_preflight_step_result result;
    do{result=ksn_mesh_q7_preflight_quantum(memory,bytes,r);}
    while(result==KSN_MESH_Q7_PREFLIGHT_MORE && r->checkpoints==before);
    return result;
}
