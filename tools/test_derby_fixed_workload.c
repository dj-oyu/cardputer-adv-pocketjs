#include "derby_fixed_workload.h"
#include "derby_measure.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t clock_us;
static char output[8192];static size_t used;static int refuse;static bool huge_variant;static unsigned writes,refuse_at;
uint64_t dm_now_us(void){return clock_us;}
uint64_t df_now_us(void){return clock_us;}
static unsigned resource_reads;static bool bad_resource;static uint64_t resource_delay,write_delay;
static unsigned deadline_checks,fail_deadline_check;
bool derby_runtime_resources_deadline_ok(uint32_t reserve_us){
    assert(reserve_us==0||reserve_us==125000);return ++deadline_checks!=fail_deadline_check;
}
bool derby_runtime_resources_read(df_resources *out){
    assert(df_status()==DF_TAIL&&!df_measuring());++resource_reads;clock_us+=resource_delay;
    *out=(df_resources){8192,100000,80000,100000,163840,true};return !bad_resource;
}
const char *df_variant(void){static char huge[2048];if(huge_variant){memset(huge,'x',sizeof huge-1);return huge;}return "baseline";}
int df_write(const char *data,size_t size){
    assert(size<=1024&&used+size<sizeof output);
    clock_us+=write_delay;
    if(refuse||++writes==refuse_at)return 0;
    memcpy(output+used,data,size);used+=size;output[used]=0;return 1;
}
static void reset(void){clock_us=1;used=0;refuse=0;writes=refuse_at=resource_reads=deadline_checks=fail_deadline_check=0;bad_resource=false;resource_delay=write_delay=0;huge_variant=false;output[0]=0;dm_reset();df_reset();}
static void arm(unsigned rep){df_command(1,rep,390,0x9112A7AB,8,1,0);assert(df_status()==DF_ARMED);df_present(true,false,false);}
static void context(unsigned rep,unsigned step){
    df_owner_begin(clock_us);assert(df_measuring());
    dm_dispatch(0,0);df_dispatch(false,false,0);
    {DM_SCOPE(DM_GUEST);clock_us+=10;{DM_SCOPE(DM_BRIDGE);clock_us+=4;{DM_SCOPE(DM_NATIVE);clock_us+=2;}}}
    df_count(DF_VIEW,1);df_command(2,rep,step,0x9112A7AB,8,1,0);
    dm_present(1,0);df_present(true,false,false);
    clock_us+=14;df_owner_work_end(clock_us);
    df_pace_begin(DF_PACE_FLOOR,clock_us,1);clock_us+=2;df_pace_end(clock_us);
    df_pace_begin(DF_PACE_CAP,clock_us,7);clock_us+=7;df_pace_end(clock_us);
    df_owner_end(clock_us,true);
}
static void complete(unsigned rep){for(unsigned i=391;i<=510;i++)context(rep,i);assert(df_status()==DF_TAIL);}
static void finish(unsigned rep){
    df_command(3,rep,510,0x9112A7AB,8,1,0x5DC86E18);
    while(rep==3&&df_status()==DF_TAIL)df_command(3,rep,510,0x9112A7AB,8,1,0x5DC86E18);
}
int main(void){
    reset();
    /* Warmup counts and cache calls must be outside the snapshot interval. */
    dm_dispatch(0,0);dm_present(1,0);df_count(DF_VIEW,999);
    for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
    assert(df_status()==DF_DONE);
    assert(resource_reads==1&&strstr(output,"DERBY_RESOURCE v=1 session=1 reps=3 phase=post_trial_tail"));
    assert(strstr(output,"contexts=120 done=120 cont=0"));
    assert(strstr(output,"wall_us=4680 work_us=3600"));
    assert(strstr(output,"p1=240:120:120 p2=840:120:840"));
    assert(strstr(output,"c1=120"));
    assert(strstr(output,"s0=1200:120 s1=480:120 s2=240:120"));
    assert(strstr(output,"reps=3 valid=1 quality=0 independent=0"));
    if(getenv("DF_TEST_OUTPUT")){
        FILE *f=fopen(getenv("DF_TEST_OUTPUT"),"wb");assert(f);
        assert(fwrite(output,1,used,f)==used);fclose(f);
    }
    /* One invalid replay poisons the whole trial; later good reps cannot salvage it. */
    reset();arm(1);complete(1);finish(1);arm(2);context(2,391);
    df_command(2,2,391,0x9112A7AB,8,1,0);assert(df_status()==DF_ABORT);
    df_command(1,3,390,0x9112A7AB,8,1,0);assert(df_status()==DF_ABORT);
    assert(strstr(output,"reps=1 valid=0"));
    /* A continuation is a separate owner turn/wait, never a new context. */
    reset();arm(1);df_owner_begin(clock_us);dm_dispatch(1,0);df_dispatch(true,false,0);
    clock_us+=8;df_owner_work_end(clock_us);
    df_pace_begin(DF_PACE_CONT,clock_us,1);clock_us+=2;df_pace_end(clock_us);df_owner_end(clock_us,true);
    complete(1);finish(1);assert(strstr(output,"contexts=120 done=120 cont=1"));
    assert(strstr(output,"p0=2:1:1"));
    reset();arm(1);df_owner_begin(clock_us);df_dispatch(false,false,1);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);df_present(false,true,false);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);df_present(false,false,true);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);df_owner_work_end(0);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(180000002);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);clock_us+=1;df_owner_work_end(clock_us);
    df_owner_end(180000002,true);assert(df_status()==DF_ABORT);
    reset();arm(1);complete(1);df_command(3,1,510,0x9112A7AB,8,1,1);assert(df_status()==DF_ABORT);
    reset();arm(1);complete(1);refuse=1;finish(1);assert(df_status()==DF_ABORT);
    for(unsigned fail=4;fail<=5;fail++){
        reset();refuse_at=fail;for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
        assert(df_status()==DF_ABORT&&!strstr(output,"reps=3 valid=1"));
    }
    for(unsigned failure=0;failure<2;failure++){
        reset();bad_resource=failure==0;resource_delay=failure?125001:0;
        for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
        assert(df_status()==DF_ABORT&&resource_reads==1&&!strstr(output,"DERBY_RESOURCE")&&!strstr(output,"reps=3 valid=1"));
    }
    for(unsigned failure=1;failure<=3;failure++){
        reset();fail_deadline_check=failure;
        for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
        assert(df_status()==DF_ABORT&&!strstr(output,"reps=3 valid=1"));
        assert(resource_reads==(failure==1?0u:1u));
    }
    /* 125ms observation + refused 110ms write must not start a second write.
     * The previous invalid-END fallback extended this native turn to 345ms. */
    reset();for(unsigned rep=1;rep<3;rep++){arm(rep);complete(rep);finish(rep);}
    arm(3);complete(3);df_command(3,3,510,0x9112A7AB,8,1,0x5DC86E18);
    assert(df_status()==DF_TAIL&&writes==3);
    resource_delay=125000;write_delay=110000;refuse_at=writes+1;
    uint64_t resource_began=clock_us;
    df_command(3,3,510,0x9112A7AB,8,1,0x5DC86E18);
    assert(df_status()==DF_ABORT&&writes==4&&clock_us-resource_began==235000);
    assert(!strstr(output,"DERBY_RESOURCE")&&!strstr(output,"DERBY_FIXED_END"));
    reset();dm_refused();arm(1);df_owner_begin(clock_us);assert(df_status()==DF_ABORT);
    reset();dm_dispatch(0,1);arm(1);df_owner_begin(clock_us);assert(df_status()==DF_ABORT);
    reset();dm_transfer(0,1,0);arm(1);df_owner_begin(clock_us);assert(df_status()==DF_ABORT);
    reset();dm_transfer(0,0,1);arm(1);df_owner_begin(clock_us);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);df_count(DF_VIEW,UINT64_MAX);df_count(DF_VIEW,1);assert(df_status()==DF_ABORT);
    /* An oversized summary refuses the entire trial, never a truncated success. */
    reset();arm(1);complete(1);huge_variant=true;finish(1);
    assert(df_status()==DF_ABORT);assert(strstr(output,"valid=0"));
    reset();arm(1);df_present(false,true,false);assert(df_status()==DF_ABORT);
    reset();arm(1);df_owner_begin(clock_us);df_owner_end(clock_us,true);assert(df_status()==DF_ABORT);
    reset();df_command(1,1,390,0x9112A7AA,8,1,0);assert(df_status()==DF_ABORT);
    puts("PASS fixed workload: exact 120x3 boundaries, direct waits, snapshot exclusion, poison-on-invalid, limits and failures");
}
