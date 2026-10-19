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
const char *df_variant(void){static char huge[2048];if(huge_variant){memset(huge,'x',sizeof huge-1);return huge;}return "baseline";}
int df_write(const char *data,size_t size){
    assert(size<=1024&&used+size<sizeof output);
    if(refuse||++writes==refuse_at)return 0;
    memcpy(output+used,data,size);used+=size;output[used]=0;return 1;
}
static void reset(void){clock_us=1;used=0;refuse=0;writes=refuse_at=0;huge_variant=false;output[0]=0;dm_reset();df_reset();}
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
static void finish(unsigned rep){df_command(3,rep,510,0x9112A7AB,8,1,0x5DC86E18);}
int main(void){
    reset();
    /* Warmup counts and cache calls must be outside the snapshot interval. */
    dm_dispatch(0,0);dm_present(1,0);df_count(DF_VIEW,999);
    for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
    assert(df_status()==DF_DONE);
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
    reset();refuse_at=4;for(unsigned rep=1;rep<=3;rep++){arm(rep);complete(rep);finish(rep);}
    assert(df_status()==DF_ABORT&&!strstr(output,"DERBY_FIXED_END"));
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
