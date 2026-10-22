"""Actual app frame() measurement, separate from the full oracle run.
Common C binary, shipping native JS bindings, no JS draw/node wrappers or VM
oracle in the measured path. Host allocator is the existing TLSF-charge model.
"""
from pathlib import Path
import sys,os,json,subprocess,datetime,statistics,math,argparse,hashlib,random,shutil,importlib.util
R=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(R/'tools/games'))
import run_derby
spec=importlib.util.spec_from_file_location('result_protocol',Path(__file__).with_name('run.py'))
protocol=importlib.util.module_from_spec(spec);spec.loader.exec_module(protocol)

def once(s,old,new):
 assert s.count(old)==1,old[:80]
 return s.replace(old,new)

HELPER=r'''
static uint64_t result_clock(clockid_t id){struct timespec ts;clock_gettime(id,&ts);return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;}
static struct {uint64_t ns,cpu;size_t calls,bytes,rise;} result_samples[60];
static uint64_t result_hash=1469598103934665603ull;
static JSValue result_fn,result_global;
static void result_tick(int index){
    pocket_input_pump(0);frame_ms=round(now_ms*1000)/1000;
    REQ(JS_SetPropertyStr(ctx,result_global,"__now",JS_NewFloat64(ctx,frame_ms))>=0);now_ms+=dt_ms;
    JSValue buttons=JS_NewInt32(ctx,0);
    const size_t calls=n_alloc,bytes=b_alloc,base=cur_bytes;turn_hi=cur_bytes;
    uint64_t cpu=result_clock(CLOCK_THREAD_CPUTIME_ID),start=result_clock(CLOCK_MONOTONIC);
    JSValue result=JS_Call(ctx,result_fn,JS_UNDEFINED,1,&buttons);
    uint64_t elapsed=result_clock(CLOCK_MONOTONIC)-start,cpu_elapsed=result_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
    size_t nc=n_alloc-calls,nb=b_alloc-bytes,rise=turn_hi-base;
    REQ(!JS_IsException(result));JS_FreeValue(ctx,result);
    if(index>=0)result_samples[index]=(typeof(result_samples[0])){elapsed,cpu_elapsed,nc,nb,rise};
    JSContext *jobs;while(JS_ExecutePendingJob(rt,&jobs)>0){}
    pocket_kasane_end_turn();present((uint64_t)tick*33333u);
    if(index>=0)for(size_t i=0;i<sizeof panel;i++){result_hash^=((unsigned char *)panel)[i];result_hash*=1099511628211ull;}
    tick++;scene_t++;
}
static int result_run(const char *kind){
    result_global=JS_GetGlobalObject(ctx);result_fn=JS_GetPropertyStr(ctx,result_global,"frame");REQ(JS_IsFunction(ctx,result_fn));
    if(!strcmp(kind,"photo")){tap("1");run_until("photo",4000);}
    else REQ(!strcmp(kind,"pad"));
    for(unsigned i=0;i<40;i++)result_tick(-1);
    for(int i=0;i<60;i++)result_tick(i);
    REQ(!exceptions&&!bad_present&&!framefails);
    REQ(!in_demo&&!strcmp(scene,kind));
    printf("PHOTO_ORDER_RESULT {\"case\":\"%s\",\"pointer_bytes\":%zu,\"hash\":\"%016llx\",\"scene\":\"%s\",\"demo\":%s,\"samples\":[",kind,sizeof(void *),(unsigned long long)result_hash,scene,in_demo?"true":"false");
    for(int i=0;i<60;i++)printf("%s[%llu,%llu,%zu,%zu,%zu]",i?",":"",(unsigned long long)result_samples[i].ns,(unsigned long long)result_samples[i].cpu,result_samples[i].calls,result_samples[i].bytes,result_samples[i].rise);
    printf("]}\n");
    JS_FreeValue(ctx,result_fn);JS_FreeValue(ctx,result_global);
    pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
'''

def main():
 p=argparse.ArgumentParser();p.add_argument('--pairs',type=int,default=40);p.add_argument('--m32',action='store_true');a=p.parse_args();assert a.pairs>=2
 out=R/('.cache/photo-order-m32' if a.m32 else '.cache/photo-order');apps=protocol.prepare(out);inputs=protocol.manifest(apps);src=(R/'tools/games/test_derby_host.c').read_text()
 src=once(src,'static bool ser_oracle;','static bool result_bench;\nstatic bool ser_oracle;')
 src=once(src,'void pocket_proc_draw_hook(uint32_t handle,const double *in,unsigned n){','void pocket_proc_draw_hook(uint32_t handle,const double *in,unsigned n){\n    if(result_bench)return;')
 src=once(src,'    printf("  [%5u] %s\\n",tick,s);','    if(!result_bench)printf("  [%5u] %s\\n",tick,s);')
 start=src.index('    "(function(){const P=kasane.procedural')
 end=src.index('    "globalThis.__turn=0;',start)
 src=src[:start]+'    "globalThis.__ov=[];globalThis.__refs=0;"\n'+src[end:]
 src=once(src,'static const char PRELUDE[]=',HELPER+'\nstatic const char PRELUDE[]=')
 src=once(src,'int main(int argc,char **argv){','int main(int argc,char **argv){\n    result_bench=getenv("PHOTO_ORDER_BENCH")!=NULL;')
 src=once(src,'    if(normal_race){','    if(result_bench)return result_run(getenv("PHOTO_ORDER_BENCH"));\n    if(normal_race){')
 harness=out/'result_perf.c';harness.write_text(src)
 if not all((out/'build'/(n+'.o')).exists() for n in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm')):
  run_derby.build(['-O2']+(['-m32','-malign-double'] if a.m32 else []),out/'build')
 (out/'perf-build').mkdir(exist_ok=True)
 for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
  shutil.copy2(out/'build'/(name+'.o'),out/'perf-build'/(name+'.o'))
 binary=run_derby.build(['-O2']+(['-m32','-malign-double'] if a.m32 else []),out/'perf-build',harness=harness)
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 session=out/('bench-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));session.mkdir()
 rows=[]
 for case in ('photo','pad'):
  for pair in range(-2,a.pairs):
   checked={}
   for order in range(2):
    mode=('baseline','candidate')[(pair+order)&1]
    env={**{k:v for k,v in os.environ.items() if not k.startswith(('DERBY_','PHOTO_ORDER_'))},'DERBY_APP_DIR':str(apps[mode]),'PHOTO_ORDER_BENCH':case,'DERBY_TIER':'1'}
    text=subprocess.check_output([str(binary)],cwd=R,env=env,text=True)
    d=json.loads(next(l.removeprefix('PHOTO_ORDER_RESULT ') for l in text.splitlines() if l.startswith('PHOTO_ORDER_RESULT ')))
    checked[mode]=d
    if pair>=0:rows.append({**d,'mode':mode,'pair':pair,'order':order})
   assert all(checked['baseline'][k]==checked['candidate'][k] for k in ('hash','scene','demo','pointer_bytes'))
 assert protocol.manifest(apps)==inputs,'Input snapshots changed during benchmark'
 summary={'inputs':inputs,'baseline':protocol.BASE,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),'candidate_scene_sha256':hashlib.sha256((apps['candidate']/'derby_scene.js').read_bytes()).hexdigest(),'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'cpu_affinity':sorted(os.sched_getaffinity(0)) if hasattr(os,'sched_getaffinity') else None,'pairs':a.pairs,'frames_per_trial':60,'sample_fields':['wall_ns','thread_cpu_ns','js_requests','tlsf_charged_bytes','peak_rise'],'groups':{}}
 for case in ('photo','pad'):
  g={}
  for mode in ('baseline','candidate'):
   group=[x for x in rows if x['case']==case and x['mode']==mode]
   v=[statistics.mean(s[0] for s in x['samples']) for x in group]
   all_frames=[s for x in group for s in x['samples']]
   g[mode]={'median_trial_mean_wall_ns':statistics.median(v),'p95_frame_wall_ns':sorted(s[0] for s in all_frames)[math.ceil(len(all_frames)*.95)-1],'mean_js_requests':statistics.mean(s[2] for s in all_frames),'mean_tlsf_charged_bytes':statistics.mean(s[3] for s in all_frames),'max_peak_rise':max(s[4] for s in all_frames),'median_trial_mean_cpu_ns':statistics.median(statistics.mean(s[1] for s in x['samples']) for x in group)}
  ratios=[statistics.mean(s[0] for s in next(x for x in rows if x['case']==case and x['pair']==i and x['mode']=='candidate')['samples'])/statistics.mean(s[0] for s in next(x for x in rows if x['case']==case and x['pair']==i and x['mode']=='baseline')['samples']) for i in range(a.pairs)]
  g['paired_mean_wall_ratio']=statistics.median(ratios)
  rng=random.Random(423);boots=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(5000))
  g['paired_mean_wall_ratio_bootstrap95']=[boots[124],boots[4874]]
  g['paired_mean_cpu_ratio']=statistics.median(statistics.mean(s[1] for s in next(x for x in rows if x['case']==case and x['pair']==i and x['mode']=='candidate')['samples'])/statistics.mean(s[1] for s in next(x for x in rows if x['case']==case and x['pair']==i and x['mode']=='baseline')['samples']) for i in range(a.pairs))
  summary['groups'][case]=g
 (session/'raw.json').write_text(json.dumps(rows,separators=(',',':'))+'\n');(session/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'out':str(session),**summary},indent=2))
if __name__=='__main__':main()
