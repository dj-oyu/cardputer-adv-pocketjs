#!/usr/bin/env python3
"""Real-app A/B for paddock selection bars. No device access."""
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
static FILE *bars_times;
static uint64_t bars_start;
static uint64_t bars_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void bars_close(void){if(bars_times)fclose(bars_times);}
/* ---- the procedural oracle */''')
    s=replace(s,'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;',
              'churn_base=turn_hi=cur_bytes;bars_start=bars_now();return JS_UNDEFINED;')
    s=replace(s,'    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){','''    const uint64_t elapsed=bars_now()-bars_start;
    if(!bars_times){bars_times=fopen(getenv("BARS_TIMES"),"w");REQ(bars_times);}
    fprintf(bars_times,"%s,%llu,%zu,%zu,%zu\\n",scene,(unsigned long long)elapsed,n_alloc-churn_n,b_alloc-churn_b,turn_hi-churn_base);
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    # Test-only observer of rectangles accepted by native API calls.
    check=r'''
(function(){
  const K=kasane,RP=K.replace,PA=K.patch;
  function wrap(tx){
    const p=Object.getPrototypeOf(tx);
    if(p.__bars)return;p.__bars=1;
    const rect=p.rect;
    p.rect=function(sp){
      const r=rect.call(this,sp);r.__rect=sp.bounds.slice();
      const q=Object.getPrototypeOf(r);
      if(!q.__bars){q.__bars=1;const st=q.setRect;q.setRect=function(t,b){const v=st.call(this,t,b);this.__rect=b.slice();return v}}
      return r;
    };
  }
  K.replace=function(fn){return RP.call(K,tx=>{wrap(tx);return fn(tx)})};
  K.patch=function(fn){return PA.call(K,tx=>{wrap(tx);return fn(tx)})};
  globalThis.__barsCheck=()=>{
    if(scene==='pad'&&!need){
      const h=F.h[pick],values=[(h.top-TOP)/SPR,(h.st-ST0)/ST1,(h.kick-KI0)/KI1];
      const expected=[[1+30*pick,13,31+30*pick,24]];
      for(let i=0;i<3;i++)expected.push([180,43+12*i,182+Math.round(52*Math.max(0,Math.min(1,values[i]))),49+12*i]);
      const refs=[R.sel].concat(R.bar);
      for(let i=0;i<4;i++)if(String(refs[i].__rect)!==String(expected[i]))throw Error('stale bar '+i+': '+refs[i].__rect+' != '+expected[i]);
    }
  };
})();
'''
    extra='\n    if(getenv("BARS_CHECK")){const char *s='+json.dumps(check)+';REQ(eval(s,strlen(s),"bars-observer.js"));}\n'
    extra+='    if(getenv("BARS_DELAY_STORAGE")){const char *s="pocket.storage.get=k=>new Promise(r=>{globalThis.__barsLoad=()=>r({value:__store[k],revision:1})})";REQ(eval(s,strlen(s),"defer-storage.js"));}\n'
    s=replace(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");','    eval(PRELUDE,strlen(PRELUDE),"prelude.js");'+extra)
    s=replace(s,'    eval(call,strlen(call),"frame.js");','    eval(call,strlen(call),"frame.js");\n    if(getenv("BARS_CHECK"))REQ(eval("__barsCheck()",13,"bars-check.js"));')
    s=replace(s,'    REQ(loaded_seen==1);','    REQ(loaded_seen==(getenv("BARS_DELAY_STORAGE")?0u:1u));')
    s=replace(s,'    for(unsigned i=0;i<20;i++)frame(0);','''    for(unsigned i=0;i<20;i++)frame(0);
    if(getenv("BARS_DELAY_STORAGE")){REQ(eval("__barsLoad()",12,"resolve-storage.js"));REQ(loaded_seen==1);frame(0);}
    if(getenv("BARS_BOUNDARIES")){
        for(unsigned n=0;n<18;n++){
            const char *s="pick=(pick+1)%8";REQ(eval(s,strlen(s),"bar-selection.js"));frame(0);
        }
        const char *s="globalThis.__savedHorse=F.h[pick];F.h[pick]=Object.assign({},F.h[pick],{top:TOP-1,st:ST0+2*ST1,kick:KI0+KI1*.5});need=1";
        REQ(eval(s,strlen(s),"bar-boundaries.js"));frame(0);
        s="F.h[pick]=__savedHorse;delete globalThis.__savedHorse;need=1";
        REQ(eval(s,strlen(s),"bar-restore.js"));frame(0);
    }
    if(getenv("BARS_NEGATIVE")){
        const char *s="F.h[pick].top=F.h[pick].top>TOP+SPR*.5?TOP:TOP+SPR";
        REQ(eval(s,strlen(s),"missing-rebuild.js"));frame(0);
    }
    if(getenv("BARS_BENCH")){
        for(unsigned i=0;i<330;i++)frame(0);
        REQ(!strcmp(scene,"pad")&&!exceptions&&!bad_present&&!framefails);
        printf("BARS_BENCH heap_after_eval=%zu eval_peak=%zu total_peak=%zu\\n",after_eval,eval_peak,peak_bytes);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("BARS_BENCH PASS");return 0;
    }''')
    s=replace(s,'static void present(uint64_t now_us){','''static uint64_t bars_panel=1469598103934665603ull;
__attribute__((destructor)) static void bars_panel_done(void){if(getenv("BARS_CAPTURE"))printf("BARS_PANEL %016llx\\n",(unsigned long long)bars_panel);}
static void present(uint64_t now_us){''')
    s=replace(s,'    else pocket_kasane_animations_presented(now_us);','''    else pocket_kasane_animations_presented(now_us);
    if(getenv("BARS_CAPTURE"))for(unsigned i=0;i<sizeof panel;i++){bars_panel^=((unsigned char*)panel)[i];bars_panel*=1099511628211ull;}''')
    p=out/'harness.c';p.write_text(s);return p

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/bars-review')
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
    for label in ('base','changed'):
        src=out/label/'source';shutil.copytree(ROOT/'apps/derby',src,dirs_exist_ok=True)
        if label=='base':(src/'derby_scene.js').write_bytes(subprocess.check_output(['git','show',f'{BASE}:{SOURCE}'],cwd=ROOT))
        apps[label]=run_derby.lower(src,out/label/'app')
    rows=[]
    def trial(label,name,**extra):
        prefix=out/label/name
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',BARS_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',**extra)
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(proc.stdout+proc.stderr)
        assert proc.returncode==0 and any(x in proc.stdout for x in ('DERBY_HOST PASS','BARS_BENCH PASS')),str(prefix)
        return proc.stdout,[line.split(',') for line in prefix.with_suffix('.times').read_text().splitlines()]
    # Full app lifecycle, then delayed storage, selection wrap and clamp edges.
    for case,extra in [('normal',{}),('lifecycle',dict(BARS_DELAY_STORAGE='1',BARS_BOUNDARIES='1'))]:
        hashes=[]
        for label in apps:
            log,times=trial(label,case,BARS_CHECK='1',BARS_CAPTURE='1',DERBY_CSV=str(out/label/(case+'.csv')),**extra)
            item=dict(mode=label,case=case,pixels=re.search(r'pixels hash (\w+)',log)[1],
                      panel=re.search(r'BARS_PANEL (\w+)',log)[1],csv_sha256=hashlib.sha256((out/label/(case+'.csv')).read_bytes()).hexdigest(),
                      finish=re.search(r'^finish: (.+)',log,re.M)[1],heap=re.search(r'^guest heap \(.+',log,re.M)[0])
            hashes.append((item['pixels'],item['csv_sha256'],item['panel'],item['finish']));rows.append(item)
            print('BARS_VERIFY',json.dumps(item),flush=True)
        assert hashes[0]==hashes[1],case
    # A future in-place horse-stat change without rebuild must be caught.
    for label in apps:
        prefix=out/label/'negative'
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',BARS_CHECK='1',
                 BARS_BENCH='1',BARS_NEGATIVE='1',BARS_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        log=proc.stdout+proc.stderr;prefix.with_suffix('.log').write_text(log)
        assert (proc.returncode==0 and 'BARS_BENCH PASS' in log) if label=='base' else (proc.returncode!=0 and 'stale bar 1:' in log),str(prefix)
    print('BARS_NEGATIVE PASS: in-place mutation without rebuild is detected',flush=True)
    if not args.verify_only:
        for pair in range(args.pairs):
            pairrows=[]
            for label in (('base','changed') if pair%2==0 else ('changed','base')):
                log,times=trial(label,f'pair-{pair}',BARS_BENCH='1')
                sample=times[50:];assert len(sample)==300 and {x[0] for x in sample}=={'pad'}
                ns=[int(x[1]) for x in sample]
                item=dict(mode=label,pair=pair,n=300,ns_median=statistics.median(ns),ns_p95=sorted(ns)[284],
                          allocations=sum(int(x[2]) for x in sample),allocated_bytes=sum(int(x[3]) for x in sample),
                          max_turn_rise=max(int(x[4]) for x in sample),heap=re.search(r'BARS_BENCH heap.+',log)[0])
                rows.append(item);pairrows.append(item);print('BARS_TIMING',json.dumps(item),flush=True)
            base,changed=sorted(pairrows,key=lambda x:x['mode'])
            assert base['allocations']-changed['allocations']==300*15 and base['allocated_bytes']-changed['allocated_bytes']==300*744
    assert (ROOT/SOURCE).read_bytes()==source,'app source changed during run'
    report=dict(base=BASE,head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                app_sha256=hashlib.sha256(source).hexdigest(),compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                flags=flags,sanitized=args.sanitize,scope='real app host frame(), TLSF length model; not device allocator/LCD/scheduler',rows=rows)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('BARS_CHANGE PASS')
if __name__=='__main__':main()
