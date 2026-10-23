"""Actual JS_Call(frame) time and guest allocation/retention, outside the oracle."""
from pathlib import Path
import argparse,datetime,hashlib,json,math,os,random,statistics,subprocess
import common
R=common.R;once=common.once
HELPER=r'''
static uint64_t bw_clock(clockid_t id){struct timespec t;clock_gettime(id,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static struct {uint64_t ns,cpu;size_t calls,bytes,rise;} bw_samples[120];
static JSValue bw_fn;
static bool bw_ride;
static size_t bw_frame_peak;
static uint64_t bw_input_hash=1469598103934665603ull;
static uint64_t bw_panel_hash=1469598103934665603ull;
static int bw_tick(int index){
    const char *keys=bw_ride?bot(0):"";hold(keys);
    if(index>=0){for(const char *p=keys;*p;p++){bw_input_hash^=(unsigned char)*p;bw_input_hash*=1099511628211ull;}bw_input_hash*=1099511628211ull;}
    pocket_input_pump(0);frame_no++;
    JSValue button=JS_NewInt32(ctx,0);size_t calls=n_alloc,bytes=b_alloc,base=cur_bytes;turn_hi=base;
    uint64_t cpu=bw_clock(CLOCK_THREAD_CPUTIME_ID),start=bw_clock(CLOCK_MONOTONIC);
    JSValue result=JS_Call(ctx,bw_fn,JS_UNDEFINED,1,&button);
    uint64_t elapsed=bw_clock(CLOCK_MONOTONIC)-start,cpu_elapsed=bw_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    if(JS_IsException(result)){JS_FreeValue(ctx,result);return 1;}
    JS_FreeValue(ctx,result);
    if(index>=0&&turn_hi>bw_frame_peak)bw_frame_peak=turn_hi;
    if(index>=0)bw_samples[index]=(typeof(bw_samples[0])){elapsed,cpu_elapsed,n_alloc-calls,b_alloc-bytes,turn_hi-base};
    JSContext *jobs;while(JS_ExecutePendingJob(rt,&jobs)>0){}
    pocket_kasane_end_turn();present();
    if(index>=0)for(size_t i=0;i<sizeof panel;i++){bw_panel_hash^=((unsigned char*)panel)[i];bw_panel_hash*=1099511628211ull;}
    return 0;
}
static int bw_run(const char *kind,size_t eval_retained,size_t eval_peak){
    JSValue global=JS_GetGlobalObject(ctx);bw_fn=JS_GetPropertyStr(ctx,global,"frame");JS_FreeValue(ctx,global);
    if(!JS_IsFunction(ctx,bw_fn))return 2;
    frames(40,"");
    bw_ride=!strcmp(kind,"ride")||!strcmp(kind,"pause");
    if(bw_ride){frames(1,"1");frames(1,"");if(num("bigWave.S.st")!=1)return 5;if(!strcmp(kind,"pause")){frames(1,"tab");frames(1,"");if(num("bigWave.S.pause")!=1)return 5;}}else if(strcmp(kind,"title"))return 2;
    for(unsigned i=0;i<40;i++)if(bw_tick(-1))return 3;
    JS_RunGC(rt);size_t before=cur_bytes;JSMemoryUsage memory;JS_ComputeMemoryUsage(rt,&memory);
    const size_t prior_peak=peak_bytes;peak_bytes=cur_bytes;
    for(int i=0;i<120;i++)if(bw_tick(i))return 3;
    const size_t window_peak=peak_bytes,whole_peak=prior_peak>window_peak?prior_peak:window_peak;
    JS_RunGC(rt);size_t retained=cur_bytes;
    double state=num("bigWave.S.st"),paused=num("bigWave.S.pause");if(state!=(bw_ride?1:0)||paused!=(!strcmp(kind,"pause")?1:0)||exceptions||present_failures){fprintf(stderr,"workload state=%g exceptions=%u present=%u\n",state,exceptions,present_failures);return 4;}
    printf("BIGWAVE_ROCK_RESULT {\"case\":\"%s\",\"pointer_bytes\":%zu,\"hash\":\"%016llx\",\"state\":%.0f,\"input_hash\":\"%016llx\",\"frame_live_peak\":%zu,\"eval_retained\":%zu,\"eval_peak\":%zu,\"retained_before\":%zu,\"retained_after\":%zu,\"measured_window_peak\":%zu,\"whole_workload_peak\":%zu,\"js_func_code_size\":%lld,\"js_func_size\":%lld,\"samples\":[",kind,sizeof(void*),(unsigned long long)bw_panel_hash,state,(unsigned long long)bw_input_hash,bw_frame_peak,eval_retained,eval_peak,before,retained,window_peak,whole_peak,(long long)memory.js_func_code_size,(long long)memory.js_func_size);
    for(int i=0;i<120;i++)printf("%s[%llu,%llu,%zu,%zu,%zu]",i?",":"",(unsigned long long)bw_samples[i].ns,(unsigned long long)bw_samples[i].cpu,bw_samples[i].calls,bw_samples[i].bytes,bw_samples[i].rise);
    puts("]}");JS_FreeValue(ctx,bw_fn);pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--pairs',type=int,default=40);p.add_argument('--m32',action='store_true');a=p.parse_args();assert a.pairs>=2
 out=R/('.cache/bigwave-rock-m32' if a.m32 else '.cache/bigwave-rock');apps=common.inputs(out)
 src=(R/'tools/games/test_big_wave_host.c').read_text()
 src=once(src,'#include <math.h>','#include <math.h>\n#include <time.h>')
 src=once(src,'static size_t cur_bytes,peak_bytes;','static bool bw_perf;\nstatic size_t cur_bytes,peak_bytes,n_alloc,b_alloc,turn_hi;')
 src=once(src,'cur_bytes+=add;cur_bytes-=sub;','cur_bytes+=add;cur_bytes-=sub;if(add){n_alloc++;b_alloc+=add;}if(cur_bytes>turn_hi)turn_hi=cur_bytes;')
 src=once(src,'    printf("  [%5u] %s\\n",frame_no,s);','    if(!bw_perf)printf("  [%5u] %s\\n",frame_no,s);')
 src=once(src,'static int oracle(void){','static int oracle(void){\n    if(bw_perf)return 0;')
 start=src.index('    "(function(){const P=pocket.kasane.procedural');end=src.index('    /* The rider formula',start);src=src[:start]+src[end:]
 src=once(src,'int main(int argc,char **argv){',HELPER+'\nint main(int argc,char **argv){\n    bw_perf=true;')
 src=once(src,'    /* ---- 1. title:','    return bw_run(getenv("BIGWAVE_ROCK_CASE"),cur_bytes,eval_peak);\n    /* ---- 1. title:')
 harness=out/'perf.c';harness.write_text(src);binary=common.build(out/'build',harness,a.m32)
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 session=out/('bench-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));session.mkdir()
 rows=[]
 for case in ('title','ride','pause'):
  for pair in range(-2,a.pairs):
   checked={}
   for order in range(2):
    mode=('baseline','candidate')[(pair+order)&1]
    env={**{k:v for k,v in os.environ.items() if not k.startswith(('BW_','BIGWAVE_ROCK_'))},'BIGWAVE_ROCK_CASE':case,'BW_HEAP_LIMIT':'163840' if a.m32 else '524288'}
    text=subprocess.check_output([str(binary),str(apps[mode])],cwd=R,env=env,text=True)
    d=json.loads(next(l.removeprefix('BIGWAVE_ROCK_RESULT ') for l in text.splitlines() if l.startswith('BIGWAVE_ROCK_RESULT ')));checked[mode]=d
    if pair>=0:rows.append({**d,'mode':mode,'pair':pair,'order':order})
   assert all(checked['baseline'][k]==checked['candidate'][k] for k in ('hash','state','pointer_bytes','input_hash'))
 summary={'heap_limit':163840 if a.m32 else 524288,'initial_gc_threshold_clamp':81920 if a.m32 else 262144,'base':common.BASE,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'pairs':a.pairs,'frames_per_trial':120,'inputs':{m:hashlib.sha256(p.read_bytes()).hexdigest() for m,p in apps.items()},'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'engine_build_sha256':hashlib.sha256((out/'build/engine-build.json').read_bytes()).hexdigest(),'cpu_affinity':sorted(os.sched_getaffinity(0)),'source_bytes':{m:p.stat().st_size for m,p in apps.items()},'groups':{}}
 for case in ('title','ride','pause'):
  g={}
  for mode in apps:
   group=[r for r in rows if r['case']==case and r['mode']==mode];frames=[s for r in group for s in r['samples']]
   g[mode]={'median_trial_mean_wall_ns':statistics.median(statistics.mean(s[0] for s in r['samples']) for r in group),'p95_frame_wall_ns':sorted(s[0] for s in frames)[math.ceil(len(frames)*.95)-1],'mean_successful_allocations':statistics.mean(s[2] for s in frames),'mean_charged_bytes':statistics.mean(s[3] for s in frames),'max_peak_rise':max(s[4] for s in frames),**{k:sorted(set(r[k] for r in group)) for k in ('eval_retained','eval_peak','retained_before','retained_after','measured_window_peak','whole_workload_peak','js_func_code_size','js_func_size','frame_live_peak')}}
  ratios=[statistics.mean(s[0] for s in next(r for r in rows if r['case']==case and r['pair']==i and r['mode']=='candidate')['samples'])/statistics.mean(s[0] for s in next(r for r in rows if r['case']==case and r['pair']==i and r['mode']=='baseline')['samples']) for i in range(a.pairs)]
  rng=random.Random(423);boots=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(5000));g.update(paired_ratio=statistics.median(ratios),bootstrap95=[boots[124],boots[4874]])
  summary['groups'][case]=g
 (session/'raw.json').write_text(json.dumps(rows,separators=(',',':'))+'\n');(session/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'out':str(session),**summary},indent=2))
if __name__=='__main__':main()
