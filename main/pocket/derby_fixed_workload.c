#include "derby_detail.h"
#include "derby_fixed_workload.h"
#include "derby_measure.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Owner task only. No heap, retry, pacing-policy change or asynchronous access. */
const char df_fixture_id[]="oval-pan8-390-510-v1";
const char df_fixture_manifest[]="84e0341ee1ac10c232e3008f28f8aada8823155e19229f926e633d7780a2021d";
static struct {
    enum df_phase phase;
    unsigned rep,seen,presented,dispatches,continuations,quality,trial_quality,reported;
    bool arm_presented,turn_open,wait_open,work_ended;
    unsigned wait_kind;
    uint64_t session_start,start,turn_start,wait_start,wall,work;
    uint64_t pace[DF_PACES],pace_calls[DF_PACES],requested[DF_PACES],counts[DF_COUNTERS];
    dm_snapshot before,after;
#ifdef POCKET_DERBY_DETAIL_MEASURE
    dd_snapshot detail;
    unsigned detail_row;
#endif
} s;
static unsigned serial;

static bool elapsed(uint64_t now,uint64_t began,uint64_t *out){
    if(now<began){s.quality|=DF_CLOCK;return false;}
    *out=now-began;return true;
}
bool df_session(void){return s.phase!=DF_OFF;}
bool df_measuring(void){return s.phase==DF_MEASURE;}
unsigned df_status(void){return (unsigned)s.phase;}
static void final_record(void){
    char line[192];
    int n=snprintf(line,sizeof line,"DERBY_FIXED_END v=1 session=%u reps=%u valid=%u quality=%u independent=0\n",
                   serial,s.reported,s.reported==3&&!s.trial_quality,s.trial_quality);
    if(n<=0||(size_t)n>=sizeof line||!df_write(line,(size_t)n)){
        s.trial_quality|=DF_OUTPUT;s.phase=DF_ABORT;
    }
}
void df_fail(unsigned reason){
    if(s.phase==DF_OFF||s.phase==DF_ABORT||s.phase==DF_DONE)return;
#ifdef POCKET_DERBY_DETAIL_MEASURE
    dd_cancel();
#endif
    s.quality|=reason;s.trial_quality|=s.quality;s.phase=DF_ABORT;
    s.turn_open=s.wait_open=false;final_record();
}
void df_reset(void){
    memset(&s,0,sizeof s);s.phase=DF_READY;s.session_start=df_now_us();++serial;
}
void df_count(enum df_counter counter,uint64_t amount){
    if(s.phase!=DF_MEASURE)return;
    if((unsigned)counter>=DF_COUNTERS||UINT64_MAX-s.counts[counter]<amount){df_fail(DF_MEASUREMENT);return;}
    s.counts[counter]+=amount;
}
static bool identity(unsigned rep,unsigned step,uint32_t seed,unsigned camera,unsigned tier){
    return rep==s.rep&&seed==UINT32_C(0x9112A7AB)&&camera==8&&tier==1&&step>=390&&step<=510;
}
#ifdef POCKET_DERBY_DETAIL_MEASURE
/* One output record per logical reporting frame, outside measured work. */
static bool detail_report_step(void){
    char line[512];int n;unsigned row=s.detail_row;
    if(row<DD_STAGES){
        const dd_counter *c=&s.detail.stage[row];
        n=snprintf(line,sizeof line,"DERBY_DETAIL v=1 session=%u rep=%u stage=%u inc_us=%"PRIu64" self_us=%"PRIu64" calls=%"PRIu64"\n",
          serial,s.rep,row,c->inclusive_us,c->self_us,c->calls);
    }else if(row<DD_STAGES+DD_EVENTS){
        unsigned event=row-DD_STAGES;
        n=snprintf(line,sizeof line,"DERBY_DETAIL_BYTES v=1 session=%u rep=%u event=%u amount=%"PRIu64"\n",serial,s.rep,event,s.detail.event[event]);
    }else{
        n=snprintf(line,sizeof line,"DERBY_DETAIL_END v=1 session=%u rep=%u stages=%u events=%u faults=%"PRIu64" pairs=%"PRIu64" elapsed=1 logical_bytes=1\n",
          serial,s.rep,DD_STAGES,DD_EVENTS,s.detail.faults,s.detail.pairs);
    }
    if(n<=0||(size_t)n>=sizeof line||!df_write(line,(size_t)n))return false;
    ++s.detail_row;return true;
}
#endif
static void report(uint32_t finish){
#ifdef POCKET_DERBY_DETAIL_MEASURE
    /* Reserve a full 250ms turn before any qualifying output. The device USB
     * write is bounded to 100ms plus tick rounding, so even the final success
     * cannot cross the 180s session limit before DONE suppresses owner checks. */
    uint64_t duration=0;
    if(!elapsed(df_now_us(),s.session_start,&duration)){df_fail(DF_CLOCK);return;}
    if(duration>180000000-250000){df_fail(DF_TIMEOUT);return;}
    if(s.detail_row<DD_STAGES+DD_EVENTS+1){
        if(!detail_report_step())df_fail(DF_OUTPUT);
        return;
    }
    if(s.detail_row==DD_STAGES+DD_EVENTS+2){
        s.phase=DF_DONE;final_record();return;
    }
#endif
    char line[1024];
    int n=snprintf(line,sizeof line,
      "DERBY_FIXED v=1 fixture=%s manifest=%s variant=%s session=%u rep=%u seq=%u valid=%u quality=%u independent=0"
      " seed=9112A7AB oval=1 tier=1 cam=8 start=390 end=510 contexts=%u done=%u cont=%u"
      " capture=0 faults=0 refused=0 tx_fail=0 wall_us=%"PRIu64" work_us=%"PRIu64" finish=%08"PRIX32"",
      df_fixture_id,df_fixture_manifest,df_variant(),serial,s.rep,s.rep,!s.quality&&!s.trial_quality,s.quality|s.trial_quality,
      s.dispatches,s.presented,s.continuations,s.wall,s.work,finish);
    for(unsigned i=0;i<DF_PACES&&n>0&&(size_t)n<sizeof line;i++)
        n+=snprintf(line+n,sizeof line-(size_t)n," p%u=%"PRIu64":%"PRIu64":%"PRIu64,
                    i,s.pace[i],s.pace_calls[i],s.requested[i]);
    for(unsigned i=0;i<DF_COUNTERS&&n>0&&(size_t)n<sizeof line;i++)
        n+=snprintf(line+n,sizeof line-(size_t)n," c%u=%"PRIu64,i,s.counts[i]);
    for(unsigned i=0;i<DM_STAGES&&n>0&&(size_t)n<sizeof line;i++){
        dm_counter a=s.before.stage[i],b=s.after.stage[i];
        if(b.self_us<a.self_us||b.calls<a.calls){s.quality|=DF_MEASUREMENT;break;}
        n+=snprintf(line+n,sizeof line-(size_t)n," s%u=%"PRIu64":%"PRIu64,
                    i,b.self_us-a.self_us,b.calls-a.calls);
    }
    if(s.quality||n<=0||(size_t)n+1>=sizeof line){df_fail(s.quality?s.quality:DF_OUTPUT);return;}
    line[n++]='\n';
    if(!df_write(line,(size_t)n)){df_fail(DF_OUTPUT);return;}
    ++s.reported;
#ifdef POCKET_DERBY_DETAIL_MEASURE
    if(s.rep==3)++s.detail_row;else s.phase=DF_READY;
#else
    if(s.rep==3){s.phase=DF_DONE;final_record();}else s.phase=DF_READY;
#endif
}
void df_command(unsigned command,unsigned rep,unsigned step,uint32_t seed,unsigned camera,unsigned tier,uint32_t finish){
    if(command==0)return;
    if(command==4){df_fail(DF_STATE);return;}
    if(command==1){
        if(s.phase!=DF_READY||rep!=s.reported+1||rep>3||step!=390||seed!=UINT32_C(0x9112A7AB)||camera!=8||tier!=1){df_fail(DF_IDENTITY);return;}
        s.rep=rep;s.seen=s.presented=s.dispatches=s.continuations=s.quality=0;
        s.arm_presented=false;s.phase=DF_ARMED;return;
    }
    if(!identity(rep,step,seed,camera,tier)){df_fail(DF_IDENTITY);return;}
    if(command==2){
        if(s.phase!=DF_MEASURE||step!=391+s.seen||s.seen>=120){df_fail(DF_CONTEXT);return;}
        ++s.seen;return;
    }
    if(command==3){
        /* Expected finish is bound by generated fixture and strict host parser. */
        if(s.phase!=DF_TAIL||step!=510||finish!=UINT32_C(0x5DC86E18)){df_fail(DF_STATE);return;}
        report(finish);return;
    }
    df_fail(DF_STATE);
}
void df_dispatch(bool continuing,bool failed,uint32_t buttons){
    if(!df_measuring())return;
    if(buttons||failed){df_fail(buttons?DF_INPUT:DF_CONTEXT);return;}
    if(continuing)++s.continuations;else if(++s.dispatches>120)df_fail(DF_CONTEXT);
}
void df_present(bool completed,bool capture,bool failed){
    if(df_session()&&s.phase!=DF_DONE&&s.phase!=DF_ABORT&&(capture||failed)){
        df_fail(capture?DF_CAPTURE:DF_PRESENT);return;
    }
    if(s.phase==DF_ARMED){
        if(completed)s.arm_presented=true;
        return;
    }
    if(!df_measuring())return;
    if(completed&&s.seen>s.presented){
        if(s.seen!=s.presented+1){df_fail(DF_CONTEXT);return;}
        ++s.presented;
    }
}
void df_owner_begin(uint64_t now){
    if(!df_session()||s.phase==DF_DONE||s.phase==DF_ABORT)return;
    uint64_t duration=0;
    if(!elapsed(now,s.session_start,&duration)||duration>180000000){df_fail(DF_TIMEOUT);return;}
    if(s.phase==DF_ARMED&&s.arm_presented){
        if(!dm_read(&s.before)){df_fail(DF_MEASUREMENT);return;}
        if(s.before.faults||s.before.output_refused||s.before.dispatch_failures||
           s.before.transfer_failures||s.before.capture_samples){df_fail(DF_MEASUREMENT);return;}
        memset(s.counts,0,sizeof s.counts);memset(s.pace,0,sizeof s.pace);
        memset(s.pace_calls,0,sizeof s.pace_calls);memset(s.requested,0,sizeof s.requested);
        s.wall=s.work=0;s.start=now;s.phase=DF_MEASURE;
#ifdef POCKET_DERBY_DETAIL_MEASURE
        dd_start();
#endif
    }
    if(df_measuring()){
        if(s.turn_open||s.wait_open){df_fail(DF_STATE);return;}
        s.turn_open=true;s.work_ended=false;s.turn_start=now;
    }
}
void df_owner_work_end(uint64_t now){
    if(!df_measuring())return;
    uint64_t dt=0;
    if(!s.turn_open||s.work_ended||!elapsed(now,s.turn_start,&dt)){df_fail(DF_STATE);return;}
    s.work+=dt;s.work_ended=true;
}
void df_pace_begin(enum df_pace kind,uint64_t now,uint32_t requested_ticks){
    if(!df_measuring())return;
    if(!s.turn_open||!s.work_ended||s.wait_open||(unsigned)kind>=DF_PACES){df_fail(DF_STATE);return;}
    s.wait_open=true;s.wait_kind=(unsigned)kind;s.wait_start=now;
    s.requested[kind]+=requested_ticks;
}
void df_pace_end(uint64_t now){
    if(!df_measuring())return;
    uint64_t dt=0;
    if(!s.wait_open||!elapsed(now,s.wait_start,&dt)){df_fail(DF_STATE);return;}
    s.pace[s.wait_kind]+=dt;++s.pace_calls[s.wait_kind];s.wait_open=false;
}
void df_owner_end(uint64_t now,bool running){
    if(!df_session()||s.phase==DF_DONE||s.phase==DF_ABORT)return;
    uint64_t duration=0;
    if(!elapsed(now,s.session_start,&duration)||duration>180000000){df_fail(DF_TIMEOUT);return;}
    if(!running){df_fail(DF_STATE);return;}
    if(!df_measuring())return;
    if(!s.turn_open||!s.work_ended||s.wait_open){df_fail(DF_STATE);return;}
    s.turn_open=false;
    if(s.seen==120&&s.presented==120){
        if(s.dispatches!=120||!dm_read(&s.after)||!elapsed(now,s.start,&s.wall)){df_fail(DF_MEASUREMENT);return;}
        if(s.after.faults!=s.before.faults||s.after.output_refused!=s.before.output_refused||
           s.after.dispatch_failures!=s.before.dispatch_failures||s.after.transfer_failures!=s.before.transfer_failures||
           s.after.contexts-s.before.contexts!=120||s.after.completed_contexts-s.before.completed_contexts!=120){df_fail(DF_MEASUREMENT);return;}
        if(s.after.capture_samples!=s.before.capture_samples){df_fail(DF_CAPTURE);return;}
#ifdef POCKET_DERBY_DETAIL_MEASURE
        if(!dd_finish(&s.detail)||s.detail.faults){df_fail(DF_MEASUREMENT);return;}
        s.detail_row=0;
#endif
        s.phase=DF_TAIL;
    }
}
