#!/usr/bin/env python3
"""Real-app A/B for within-frame ordering reuse. No device access."""
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
SOURCE='apps/derby/derby_play.js'

def replace(s,old,new):
    assert s.count(old)==1,old
    return s.replace(old,new)

def harness(out):
    s=(ROOT/'tools/games/test_derby_host.c').read_text()
    s=replace(s,'/* ---- the procedural oracle */','''
static FILE *rank_times;
static uint64_t rank_start;
static uint64_t rank_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void rank_close(void){if(rank_times)fclose(rank_times);}
/* ---- the procedural oracle */''')
    s=replace(s,'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;',
              'churn_base=turn_hi=cur_bytes;rank_start=rank_now();return JS_UNDEFINED;')
    s=replace(s,'    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){','''    const uint64_t elapsed=rank_now()-rank_start;
    if(!rank_times){rank_times=fopen(getenv("RANK_TIMES"),"w");REQ(rank_times);}
    fprintf(rank_times,"%s,%llu,%zu,%zu,%zu\\n",scene,(unsigned long long)elapsed,n_alloc-churn_n,b_alloc-churn_b,turn_hi-churn_base);
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    s=replace(s,'    for(unsigned i=0;i<20;i++)frame(0);','''    for(unsigned i=0;i<20;i++)frame(0);
    if(getenv("RANK_BENCH")){
        const bool race=!strcmp(getenv("RANK_BENCH"),"race");
        if(race){tap("1");run_until("race",400);}
        unsigned warm=race&&getenv("RANK_WARMUP")?(unsigned)atoi(getenv("RANK_WARMUP")):0;
        for(unsigned i=0;i<warm+50;i++)frame(0);
        for(unsigned i=0;i<300;i++)frame(0);
        printf("RANK_WINDOW frames=%u..%u\\n",warm+51,warm+350);
        REQ(!strcmp(scene,race?"race":"pad")&&!exceptions&&!bad_present&&!framefails);
        printf("RANK_BENCH heap_after_eval=%zu eval_peak=%zu total_peak=%zu\\n",after_eval,eval_peak,peak_bytes);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("RANK_BENCH PASS");return 0;
    }''')
    s=replace(s,'static void present(uint64_t now_us){','''static uint64_t rank_panel=1469598103934665603ull;
__attribute__((destructor)) static void rank_panel_done(void){if(getenv("RANK_CAPTURE"))printf("RANK_PANEL %016llx\\n",(unsigned long long)rank_panel);}
static void present(uint64_t now_us){''')
    s=replace(s,'    else pocket_kasane_animations_presented(now_us);','''    else pocket_kasane_animations_presented(now_us);
    if(getenv("RANK_CAPTURE"))for(unsigned i=0;i<sizeof panel;i++){rank_panel^=((unsigned char*)panel)[i];rank_panel*=1099511628211ull;}''')
    p=out/'harness.c';p.write_text(s);return p

def verify_script():
    return r"""
const __rankFrame=frame;
globalThis.frame=function(b){
 const value=__rankFrame(b);
 if(scene==='race'&&t>0){const expected=order(rs);if(ro.join(',')!==expected.join(','))throw Error('frame rank '+t);}
 return value;
};
"""

def boundary_script():
    # A copied comparator is only an observer; calls to the actual order are counted separately.
    old=(ROOT/'apps/derby/derby_watch.js').read_text().split('const order = ',1)[1].split(';',1)[0]
    return 'const __rankReference='+old+';'+r"""
let __rankSteps=0,__rankZero=0,__rankMulti=0,__rankTotal=0;
const __rankStep=step,__rankFrame=frame;
step=function(f,s,d){++__rankSteps;return __rankStep(f,s,d)};
globalThis.frame=function(b){
 if(scene==='race'&&t===50){__rankCalls=0;__rankZero=__rankMulti=__rankTotal=0;}
 if(scene==='race'&&t===100)disp=2.5;
 if(scene==='race'&&t===110){slow=1;disp=0;}
 if(scene==='race'&&t===114)slow=0;
 __rankSteps=0;const result=__rankFrame(b);
 if(scene==='race'&&t>0&&ro.join(',')!==__rankReference(rs).join(','))throw Error('rank boundary '+t);
 if(scene==='race'&&t>50&&t<=350){__rankZero+=!__rankSteps;__rankMulti+=__rankSteps>1;__rankTotal+=__rankSteps;}
 if(scene==='race'&&t===350)log('RANK_REACH '+__rankCalls+' '+__rankZero+' '+__rankMulti+' '+__rankTotal);
 return result;
};
"""

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/rank-review')
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--pairs',type=int,default=15)
    p.add_argument('--verify-only',action='store_true')
    p.add_argument('--cases',default='pad,race')
    p.add_argument('--warmup',type=int,default=0)
    p.add_argument('--course',choices=['oval','straight'])
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    assert args.pairs>0
    source=(ROOT/SOURCE).read_bytes()
    flags=['-O2']+(['-fsanitize=address,undefined','-fno-pie','-no-pie'] if args.sanitize else [])
    binary=run_derby.build(flags,out/'build',harness=harness(out))
    apps={}
    for label in ('base','changed'):
        src=out/label/'source';shutil.copytree(ROOT/'apps/derby',src,dirs_exist_ok=True)
        if label=='base':(src/'derby_play.js').write_bytes(subprocess.check_output(['git','show',f'{BASE}:{SOURCE}'],cwd=ROOT))
        apps[label]=run_derby.lower(src,out/label/'app')
        if args.course:apps[label]=run_derby.force_course(apps[label],args.course)
    rows=[]
    def trial(label,name,**extra):
        prefix=out/label/name
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',RANK_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',**extra)
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(proc.stdout+proc.stderr)
        assert proc.returncode==0 and any(x in proc.stdout for x in ('DERBY_HOST PASS','RANK_BENCH PASS')),str(prefix)
        return proc.stdout,[line.split(',') for line in prefix.with_suffix('.times').read_text().splitlines()]
    # Full MID lifecycle includes photo zoom, screen lettering, demos and rebuilds.
    hashes=[]
    for label in apps:
        log,times=trial(label,'full',RANK_CAPTURE='1',DERBY_JS=verify_script(),DERBY_CSV=str(out/label/'full.csv'))
        item=dict(mode=label,case='full',pixels=re.search(r'pixels hash (\w+)',log)[1],panel=re.search(r'RANK_PANEL (\w+)',log)[1],csv_sha256=hashlib.sha256((out/label/'full.csv').read_bytes()).hexdigest(),finish=re.search(r'^finish: (.+)',log,re.M)[1])
        hashes.append((item['pixels'],item['panel'],item['csv_sha256'],item['finish']));rows.append(item);print('RANK_VERIFY',json.dumps(item),flush=True)
    assert hashes[0]==hashes[1]
    boundary=[]
    for label in apps:
        original=apps[label];copy=out/label/'boundary-app';shutil.copytree(original,copy,dirs_exist_ok=True)
        watch=copy/'derby_watch.js';script=watch.read_text();line=next(x for x in script.splitlines() if x.startswith('const order = '))
        expr=line[len('const order = s => '):-1]
        script=replace(script,line,'globalThis.__rankCalls=0;const order=s=>(++__rankCalls,'+expr+');');watch.write_text(script)
        apps[label]=copy
        log,_=trial(label,'boundary',RANK_BENCH='race',RANK_CAPTURE='1',DERBY_JS=boundary_script())
        apps[label]=original
        m=re.search(r'RANK_REACH (\d+) (\d+) (\d+) (\d+)',log);assert m
        row=dict(case='boundary',mode=label,calls=int(m[1]),zero=int(m[2]),multi=int(m[3]),steps=int(m[4]),panel=re.search(r'RANK_PANEL (\w+)',log)[1]);rows.append(row);boundary.append(row);print('RANK_BOUNDARY',json.dumps(row),flush=True)
    a,b=boundary
    assert a['zero']>0 and a['multi']>0
    assert (a['zero'],a['multi'],a['steps'],a['panel'])==(b['zero'],b['multi'],b['steps'],b['panel'])
    assert a['calls']==a['steps']+300 and b['calls']==b['steps']+b['zero']
    if not args.verify_only:
        for case in args.cases.split(','):
            for pair in range(args.pairs):
                pairrows=[]
                for label in (('base','changed') if pair%2==0 else ('changed','base')):
                    log,times=trial(label,f'{case}-{pair}',RANK_BENCH=case,RANK_WARMUP=str(args.warmup))
                    sample=times[-300:];assert len(sample)==300 and {x[0]for x in sample}=={case}
                    ns=[int(x[1])for x in sample]
                    item=dict(mode=label,case=case,pair=pair,n=300,ns_median=statistics.median(ns),ns_p95=sorted(ns)[284],allocations=sum(int(x[2])for x in sample),allocated_bytes=sum(int(x[3])for x in sample),max_turn_rise=max(int(x[4])for x in sample),heap=re.search(r'RANK_BENCH heap.+',log)[0])
                    rows.append(item);pairrows.append(item);print('RANK_TIMING',json.dumps(item),flush=True)
                a,b=sorted(pairrows,key=lambda x:x['mode'])
                assert a['allocations']>=b['allocations'] and a['allocated_bytes']>=b['allocated_bytes']
                if case=='pad':assert a['allocations']==b['allocations'] and a['allocated_bytes']==b['allocated_bytes']
    assert (ROOT/SOURCE).read_bytes()==source,'app source changed during run'
    report=dict(base=BASE,head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                app_sha256=hashlib.sha256(source).hexdigest(),compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                flags=flags,sanitized=args.sanitize,course=args.course,warmup=args.warmup,scope='real app host frame(), TLSF length model; not device allocator/LCD/scheduler',rows=rows)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('RANK_TARGET PASS')
if __name__=='__main__':main()
