#!/usr/bin/env python3
"""Real-app A/B for fixed paddock odds text. No device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import run_derby

ROOT=Path(__file__).resolve().parents[2]
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
SOURCE='apps/derby/derby_scene.js'

def replace(s,old,new):
    assert s.count(old)==1,old
    return s.replace(old,new)

def harness(out):
    s=(ROOT/'tools/games/test_derby_host.c').read_text()
    s=replace(s,'/* ---- the procedural oracle */','''
static FILE *odds_times;
static uint64_t odds_start;
static uint64_t odds_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void odds_close(void){if(odds_times)fclose(odds_times);}
/* ---- the procedural oracle */''')
    s=replace(s,'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;',
              'churn_base=turn_hi=cur_bytes;odds_start=odds_now();return JS_UNDEFINED;')
    s=replace(s,'    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){','''    const uint64_t elapsed=odds_now()-odds_start;
    if(!odds_times){odds_times=fopen(getenv("ODDS_TIMES"),"w");REQ(odds_times);}
    fprintf(odds_times,"%s,%llu,%zu,%zu,%zu\\n",scene,(unsigned long long)elapsed,n_alloc-churn_n,b_alloc-churn_b,turn_hi-churn_base);
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    # Test-only observer of arguments delivered to the real native text API.
    check=r'''
(function(){
  const K=kasane,RP=K.replace,PA=K.patch;
  function wrap(tx){
    const p=Object.getPrototypeOf(tx);
    if(p.__odds)return;p.__odds=1;
    const text=p.text;
    p.text=function(sp){
      const r=text.call(this,sp);r.__text=sp.text;
      const q=Object.getPrototypeOf(r);
      if(!q.__odds){q.__odds=1;const st=q.setText;q.setText=function(t,s){const v=st.call(this,t,s);this.__text=s;return v}}
      return r;
    };
  }
  K.replace=function(fn){return RP.call(K,tx=>{wrap(tx);return fn(tx)})};
  K.patch=function(fn){return PA.call(K,tx=>{wrap(tx);return fn(tx)})};
  globalThis.__oddsCheck=()=>{
    if(scene==='pad'&&!need){
      if(R.od.length!==8)throw Error('odds labels absent');
      for(let i=0;i<8;i++){
        const wanted=od[i]<10?od[i].toFixed(1):''+Math.round(od[i]);
        if(R.od[i].__text!==wanted)throw Error('stale odds label '+i+': '+R.od[i].__text+' != '+wanted);
      }
    }
  };
})();
'''
    extra='\n    if(getenv("ODDS_CHECK")){const char *s='+json.dumps(check)+';REQ(eval(s,strlen(s),"odds-observer.js"));}\n'
    extra+='    if(getenv("ODDS_DELAY_STORAGE")){const char *s="pocket.storage.get=k=>new Promise(r=>{globalThis.__oddsLoad=()=>r({value:__store[k],revision:1})})";REQ(eval(s,strlen(s),"defer-storage.js"));}\n'
    s=replace(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");','    eval(PRELUDE,strlen(PRELUDE),"prelude.js");'+extra)
    s=replace(s,'    eval(call,strlen(call),"frame.js");','    eval(call,strlen(call),"frame.js");\n    if(getenv("ODDS_CHECK"))REQ(eval("__oddsCheck()",13,"odds-check.js"));')
    s=replace(s,'    REQ(loaded_seen==1);','    REQ(loaded_seen==(getenv("ODDS_DELAY_STORAGE")?0u:1u));')
    s=replace(s,'    for(unsigned i=0;i<20;i++)frame(0);','''    for(unsigned i=0;i<20;i++)frame(0);
    if(getenv("ODDS_DELAY_STORAGE")){REQ(eval("__oddsLoad()",12,"resolve-storage.js"));REQ(loaded_seen==1);frame(0);}
    if(getenv("ODDS_BOUNDARIES")){
        const char *s="globalThis.__savedOdds=od;od=[1.1,9.9,9.95,10,10.5,99.9,998.9,999];need=1";
        REQ(eval(s,strlen(s),"odds-boundaries.js"));frame(0);
        s="od=__savedOdds;delete globalThis.__savedOdds;need=1";
        REQ(eval(s,strlen(s),"odds-restore.js"));frame(0);
    }
    if(getenv("ODDS_NEGATIVE")){
        const char *s="od[0]=od[0]===999?1.1:999";
        REQ(eval(s,strlen(s),"missing-rebuild.js"));frame(0);
    }
    if(getenv("ODDS_BENCH")){
        for(unsigned i=0;i<330;i++)frame(0);
        REQ(!strcmp(scene,"pad")&&!exceptions&&!bad_present&&!framefails);
        printf("ODDS_BENCH heap_after_eval=%zu eval_peak=%zu total_peak=%zu\\n",after_eval,eval_peak,peak_bytes);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("ODDS_BENCH PASS");return 0;
    }''')
    p=out/'harness.c';p.write_text(s);return p

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/odds-review')
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--pairs',type=int,default=15)
    p.add_argument('--verify-only',action='store_true')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    assert args.pairs>0
    source=(ROOT/SOURCE).read_bytes()
    flags=['-O2']+(['-fsanitize=address,undefined','-fno-pie','-no-pie'] if args.sanitize else [])
    binary=run_derby.build(flags,out/'build',harness=harness(out))
    apps={}
    for label in ('base','initial'):
        src=out/label/'source';shutil.copytree(ROOT/'apps/derby',src,dirs_exist_ok=True)
        if label=='base':(src/'derby_scene.js').write_bytes(subprocess.check_output(['git','show',f'{BASE}:{SOURCE}'],cwd=ROOT))
        apps[label]=run_derby.lower(src,out/label/'app')
    rows=[]
    def trial(label,name,**extra):
        prefix=out/label/name
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',ODDS_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',**extra)
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(proc.stdout+proc.stderr)
        assert proc.returncode==0 and proc.stdout.rstrip().endswith(('DERBY_HOST PASS','ODDS_BENCH PASS')),str(prefix)
        return proc.stdout,[line.split(',') for line in prefix.with_suffix('.times').read_text().splitlines()]
    # Same full app script, then delayed persisted-state arrival and formatting edges.
    for case,extra in [('normal',{}),('lifecycle',dict(ODDS_DELAY_STORAGE='1',ODDS_BOUNDARIES='1'))]:
        hashes=[]
        for label in apps:
            log,times=trial(label,case,ODDS_CHECK='1',DERBY_CSV=str(out/label/(case+'.csv')),**extra)
            item=dict(mode=label,case=case,pixels=re.search(r'pixels hash (\w+)',log)[1],
                      csv_sha256=hashlib.sha256((out/label/(case+'.csv')).read_bytes()).hexdigest(),
                      finish=re.search(r'^finish: (.+)',log,re.M)[1],heap=re.search(r'^guest heap \(.+',log,re.M)[0])
            hashes.append((item['pixels'],item['csv_sha256'],item['finish']));rows.append(item)
            print('ODDS_VERIFY',json.dumps(item),flush=True)
        assert hashes[0]==hashes[1],case
    # A future in-place odds change without need=1 must be caught by the observer.
    for label in apps:
        prefix=out/label/'negative'
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',ODDS_CHECK='1',
                 ODDS_BENCH='1',ODDS_NEGATIVE='1',ODDS_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        log=proc.stdout+proc.stderr;prefix.with_suffix('.log').write_text(log)
        assert (proc.returncode==0 and 'ODDS_BENCH PASS' in log) if label=='base' else (proc.returncode!=0 and 'stale odds label 0:' in log),str(prefix)
    print('ODDS_NEGATIVE PASS: in-place mutation without rebuild is detected',flush=True)
    if not args.verify_only:
        for pair in range(args.pairs):
            pairrows=[]
            for label in (('base','initial') if pair%2==0 else ('initial','base')):
                log,times=trial(label,f'pair-{pair}',ODDS_BENCH='1')
                sample=times[50:];assert len(sample)==300 and {x[0] for x in sample}=={'pad'}
                ns=[int(x[1]) for x in sample]
                item=dict(mode=label,pair=pair,n=300,ns_median=statistics.median(ns),ns_p95=sorted(ns)[284],
                          allocations=sum(int(x[2]) for x in sample),allocated_bytes=sum(int(x[3]) for x in sample),
                          max_turn_rise=max(int(x[4]) for x in sample),heap=re.search(r'ODDS_BENCH heap.+',log)[0])
                rows.append(item);pairrows.append(item);print('ODDS_TIMING',json.dumps(item),flush=True)
            base,initial=sorted(pairrows,key=lambda x:x['mode'])
            assert base['allocations']-initial['allocations']==300*13 and base['allocated_bytes']-initial['allocated_bytes']==300*572
    assert (ROOT/SOURCE).read_bytes()==source,'app source changed during run'
    report=dict(base=BASE,head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                app_sha256=hashlib.sha256(source).hexdigest(),compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                flags=flags,sanitized=args.sanitize,scope='real app host frame(), TLSF length model; not device allocator/LCD/scheduler',rows=rows)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('ODDS_INITIAL PASS')
if __name__=='__main__':main()
