#include "derby_fixed_workload.h"
#include "derby_measure.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef POCKET_DERBY_DETAIL_MEASURE
#define REPORT_ROWS 40u
#else
#define REPORT_ROWS 0u
#endif
#define REPORT_WRITES (3u*(REPORT_ROWS+1u)+1u)
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
    unsigned turns=0;do{record(rep);assert(++turns<=REPORT_ROWS+2);}while(df_status()==DF_TAIL);
    assert(turns==REPORT_ROWS+(rep==3?2:1));
}
int main(void){
    reset();for(unsigned rep=1;rep<=3;rep++){prepare(rep);finish(rep);}
    assert(df_status()==DF_DONE&&writes==REPORT_WRITES);
    assert(strstr(output,"reps=3 valid=1 quality=0 independent=0"));
    /* END and owner EXIT may share a tick, but not a live JS stack. */
    uint64_t before_exit=clock_us;
    assert(df_fixed_exit_record()&&writes==REPORT_WRITES+1&&clock_us-before_exit==110000);
    assert(strstr(output,"DERBY_DETAIL_EXIT v=1 session=1 clean=1"));
    assert(!df_fixed_exit_record()&&writes==REPORT_WRITES+1);
    reset();assert(!df_fixed_exit_record()&&!writes);
    for(unsigned rep=1;rep<=3;rep++){prepare(rep);finish(rep);}
    refuse_at=writes+1;assert(!df_fixed_exit_record());
    assert(!df_fixed_exit_record()&&writes==REPORT_WRITES+1&&!strstr(output,"DERBY_DETAIL_EXIT"));
    reset();for(unsigned rep=1;rep<=3;rep++){prepare(rep);finish(rep);}
    clock_us=179950001;assert(!df_fixed_exit_record()&&writes==REPORT_WRITES);
    for(unsigned fail=1;fail<=REPORT_ROWS+2;fail++){
        reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}
        prepare(3);refuse_at=writes+fail;
        for(unsigned i=0;i<fail;i++)record(3);
        assert(df_status()==DF_ABORT&&!strstr(output,"reps=3 valid=1"));
    }
    reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}prepare(3);
    for(unsigned i=0;i<REPORT_ROWS+1;i++)record(3);
    assert(df_status()==DF_TAIL&&!strstr(output,"DERBY_FIXED_END"));
    /* A final successful 110ms write must not start with only 50ms left. */
    clock_us=179950001;record(3);
    assert(df_status()==DF_ABORT&&strstr(output,"reps=3 valid=0")&&!strstr(output,"reps=3 valid=1"));
    reset();for(unsigned rep=1;rep<3;rep++){prepare(rep);finish(rep);}prepare(3);
    for(unsigned i=0;i<REPORT_ROWS+1;i++)record(3);
    clock_us=180000002;df_owner_begin(clock_us);
    assert(df_status()==DF_ABORT&&strstr(output,"reps=3 valid=0")&&!strstr(output,"reps=3 valid=1"));
    puts("PASS slow successful output: one record/return, 110ms native writes, all 42 refusal positions, timeout before final validity");
}
