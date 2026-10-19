#include "derby_fixed_workload.h"
#include "derby_measure.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t clock_us;
static unsigned writes,refuse_at;
static char output[16384];static size_t used;
uint64_t dm_now_us(void){return clock_us;}
uint64_t df_now_us(void){return clock_us;}
const char *df_variant(void){return "baseline";}
int df_write(const char *data,size_t size){
    assert(size<=1024&&used+size<sizeof output);
    clock_us+=110000; /* worst successful call allowance incl. tick rounding */
    if(++writes==refuse_at)return 0;
    memcpy(output+used,data,size);used+=size;output[used]=0;return 1;
}
static void reset(void){clock_us=1;writes=refuse_at=used=0;output[0]=0;dm_reset();df_reset();}
static void prepare(unsigned rep){
    df_command(1,rep,390,0x9112A7AB,8,1,0);assert(df_status()==DF_ARMED);df_present(true,false,false);
    for(unsigned step=391;step<=510;step++){
        df_owner_begin(clock_us);assert(df_measuring());dm_dispatch(0,0);df_dispatch(false,false,0);
        df_command(2,rep,step,0x9112A7AB,8,1,0);dm_present(1,0);df_present(true,false,false);
        clock_us+=100;df_owner_work_end(clock_us);df_owner_end(clock_us,true);
    }
    assert(df_status()==DF_TAIL);
}
static void record(unsigned rep){
    df_owner_begin(clock_us);unsigned before=writes;uint64_t began=clock_us;
    df_command(3,rep,510,0x9112A7AB,8,1,0x5DC86E18);
    if(df_status()!=DF_ABORT){assert(writes==before+1);assert(clock_us-began==110000);}
    else assert(clock_us-began<=220000); /* refusal + one invalid final record */
    df_owner_end(clock_us,true);
}
static void finish(unsigned rep){
    unsigned turns=0;do{record(rep);assert(++turns<=42);}while(df_status()==DF_TAIL);
    assert(turns==(rep==3?42:41));
}
int main(void){
    reset();for(unsigned rep=1;rep<=3;rep++){prepare(rep);finish(rep);}
    assert(df_status()==DF_DONE&&writes==124);
    assert(strstr(output,"reps=3 valid=1 quality=0 independent=0"));
    for(unsigned fail=1;fail<=42;fail++){
        reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}
        prepare(3);refuse_at=writes+fail;
        for(unsigned i=0;i<fail;i++)record(3);
        assert(df_status()==DF_ABORT&&!strstr(output,"reps=3 valid=1"));
    }
    reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}prepare(3);
    for(unsigned i=0;i<41;i++)record(3);
    assert(df_status()==DF_TAIL&&!strstr(output,"DERBY_FIXED_END"));
    /* A final successful 110ms write must not start with only 50ms left. */
    clock_us=179950001;record(3);
    assert(df_status()==DF_ABORT&&strstr(output,"reps=3 valid=0")&&!strstr(output,"reps=3 valid=1"));
    reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}prepare(3);
    for(unsigned i=0;i<41;i++)record(3);
    clock_us=180000002;df_owner_begin(clock_us);
    assert(df_status()==DF_ABORT&&strstr(output,"reps=3 valid=0")&&!strstr(output,"reps=3 valid=1"));
    puts("PASS slow successful output: one record/return, 110ms native writes, all 42 refusal positions, timeout before final validity");
}
