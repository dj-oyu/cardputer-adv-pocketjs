#!/usr/bin/env python3
"""Real-app A/B for skipped invisible screen projection. No device access."""
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
# Isolate paint() against the same adopted scene; retain historical e0 evidence separately.
BASE='06523d9fe78c3bd1db293eb09ab17522954807f9'
SOURCE='apps/derby/derby_scene.js'

def replace(s,old,new):
    assert s.count(old)==1,old
    return s.replace(old,new)

def harness(out):
    s=(ROOT/'tools/games/test_derby_host.c').read_text()
    s=replace(s,'/* ---- the procedural oracle */','''
static FILE *screen_times;
static uint64_t screen_start;
static uint64_t screen_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void screen_close(void){if(screen_times)fclose(screen_times);}
/* ---- the procedural oracle */''')
    s=replace(s,'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;',
              'churn_base=turn_hi=cur_bytes;screen_start=screen_now();return JS_UNDEFINED;')
    s=replace(s,'    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){','''    const uint64_t elapsed=screen_now()-screen_start;
    if(!screen_times){screen_times=fopen(getenv("SCREEN_TIMES"),"w");REQ(screen_times);}
    fprintf(screen_times,"%s,%llu,%zu,%zu,%zu\\n",scene,(unsigned long long)elapsed,n_alloc-churn_n,b_alloc-churn_b,turn_hi-churn_base);
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    s=replace(s,'    for(unsigned i=0;i<20;i++)frame(0);','''    for(unsigned i=0;i<20;i++)frame(0);
    if(getenv("SCREEN_BENCH")){
        const bool race=!strcmp(getenv("SCREEN_BENCH"),"race");
        if(race){tap("1");run_until("race",400);}
        for(unsigned i=0;i<350;i++)frame(0);
        REQ(!strcmp(scene,race?"race":"pad")&&!exceptions&&!bad_present&&!framefails);
        printf("SCREEN_BENCH heap_after_eval=%zu eval_peak=%zu total_peak=%zu\\n",after_eval,eval_peak,peak_bytes);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("SCREEN_BENCH PASS");return 0;
    }''')
    s=replace(s,'static void present(uint64_t now_us){','''static uint64_t screen_panel=1469598103934665603ull;
__attribute__((destructor)) static void screen_panel_done(void){if(getenv("SCREEN_CAPTURE"))printf("SCREEN_PANEL %016llx\\n",(unsigned long long)screen_panel);}
static void present(uint64_t now_us){''')
    s=replace(s,'    else pocket_kasane_animations_presented(now_us);','''    else pocket_kasane_animations_presented(now_us);
    if(getenv("SCREEN_CAPTURE"))for(unsigned i=0;i<sizeof panel;i++){screen_panel^=((unsigned char*)panel)[i];screen_panel*=1099511628211ull;}''')
    p=out/'harness.c';p.write_text(s);return p

def verify_script():
    old=subprocess.check_output(['git','show',f'{BASE}:{SOURCE}'],cwd=ROOT,text=True)
    block=old.split('  const a = pose(VS[0] - VS[2], VS[1]),',1)[1].split('  if (vr) ++von;',1)[0]
    block='  const a = pose(VS[0] - VS[2], VS[1]),'+block
    return 'function __screenReference(c){const saveVr=vr,saveVq=vq;'+block+r"""
 const result=[vr,vq];vr=saveVr;vq=saveVq;return result;
}
const __screenPaint=paint;
paint=function(c,xs,close,gate,extra){
 const expected=__screenReference(c),before=von;
 const result=__screenPaint(c,xs,close,gate,extra);
 if(JSON.stringify(vr)!==JSON.stringify(expected[0]))throw Error('screen vr '+scene+'/'+cm);
 if(vr&&pc&&JSON.stringify(vq)!==JSON.stringify(expected[1]))throw Error('screen vq');
 if(von!==before+(expected[0]?1:0))throw Error('screen warmup');
 return result;
};
"""

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/screen-review')
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
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_HEAP_LIMIT='524288',SCREEN_TIMES=str(prefix)+'.times',
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',**extra)
        proc=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(proc.stdout+proc.stderr)
        assert proc.returncode==0 and any(x in proc.stdout for x in ('DERBY_HOST PASS','SCREEN_BENCH PASS')),str(prefix)
        return proc.stdout,[line.split(',') for line in prefix.with_suffix('.times').read_text().splitlines()]
    # Full MID lifecycle includes photo zoom, screen lettering, demos and rebuilds.
    hashes=[]
    for label in apps:
        log,times=trial(label,'full',SCREEN_CAPTURE='1',DERBY_JS=verify_script(),DERBY_CSV=str(out/label/'full.csv'))
        item=dict(mode=label,case='full',pixels=re.search(r'pixels hash (\w+)',log)[1],panel=re.search(r'SCREEN_PANEL (\w+)',log)[1],csv_sha256=hashlib.sha256((out/label/'full.csv').read_bytes()).hexdigest(),finish=re.search(r'^finish: (.+)',log,re.M)[1])
        hashes.append((item['pixels'],item['panel'],item['csv_sha256'],item['finish']));rows.append(item);print('SCREEN_VERIFY',json.dumps(item),flush=True)
    assert hashes[0]==hashes[1]
    heads=[]
    for label in apps:
        log,_=trial(label,'headon',SCREEN_BENCH='race',SCREEN_CAPTURE='1',DERBY_JS=verify_script(),DERBY_EACH="if(scene==='race'&&t===80){cm=6;hold=0;}if(scene==='race'&&t===85){cm=0;hold=0;}")
        row=dict(mode=label,case='headon',panel=re.search(r'SCREEN_PANEL (\w+)',log)[1]);rows.append(row);heads.append(row['panel']);print('SCREEN_HEADON',json.dumps(row),flush=True)
    assert heads[0]==heads[1]
    if not args.verify_only:
        for case in ['pad','race']:
            for pair in range(args.pairs):
                pairrows=[]
                for label in (('base','changed') if pair%2==0 else ('changed','base')):
                    log,times=trial(label,f'{case}-{pair}',SCREEN_BENCH=case)
                    sample=times[-300:];assert len(sample)==300 and {x[0]for x in sample}=={case}
                    ns=[int(x[1])for x in sample]
                    item=dict(mode=label,case=case,pair=pair,n=300,ns_median=statistics.median(ns),ns_p95=sorted(ns)[284],allocations=sum(int(x[2])for x in sample),allocated_bytes=sum(int(x[3])for x in sample),max_turn_rise=max(int(x[4])for x in sample),heap=re.search(r'SCREEN_BENCH heap.+',log)[0])
                    rows.append(item);pairrows.append(item);print('SCREEN_TIMING',json.dumps(item),flush=True)
                a,b=sorted(pairrows,key=lambda x:x['mode'])
                assert a['allocations']-b['allocations']==300*(9 if case=='pad' else 0) and a['allocated_bytes']-b['allocated_bytes']==300*(456 if case=='pad' else 0)
    assert (ROOT/SOURCE).read_bytes()==source,'app source changed during run'
    report=dict(base=BASE,head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                app_sha256=hashlib.sha256(source).hexdigest(),compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                flags=flags,sanitized=args.sanitize,scope='real app host frame(), TLSF length model; not device allocator/LCD/scheduler',rows=rows)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('SCREEN_HOIST PASS')
if __name__=='__main__':main()
