#!/usr/bin/env python3
"""Compare the pinned main with view norm reuse through the real DERBY app.
No device access. All instrumentation lives in generated host-only sources.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import platform
import subprocess
import run_derby

ROOT = Path(__file__).resolve().parents[2]
BASE = 'e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
SOURCE = 'main/pocket/pocket_derby.c'

def replace(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)

def instrument(source):
    source = source.replace('#include <math.h>', '''#include <math.h>
#include <time.h>
unsigned long long norm_views, norm_oval, norm_ou, norm_sqrt, norm_view_ns, norm_ser_ns;
static unsigned long long norm_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
''')
    source = replace(source, 'static bool ou(unsigned j,double w,double m,double zf){',
                     'static bool ou(unsigned j,double w,double m,double zf){\n    ++norm_ou;')
    source = replace(source, 'sqrt(f*f+25600)', '(++norm_sqrt,sqrt(f*f+25600))')
    source = replace(source, '    course->in_view=0;', '    ++norm_views;if(course->sections)++norm_oval;\n    course->in_view=0;')
    for name in ('view', 'ser'):
        signature = f'static JSValue js_{name}(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)'
        start = source.index(signature)
        end = source.index('\n}', start) + 2
        wrapper = f'''\n{signature}{{
    const unsigned long long start=norm_now();
    JSValue result=js_{name}_impl(ctx,self,argc,argv);
    norm_{name}_ns+=norm_now()-start;return result;
}}'''
        source = source[:end] + wrapper + source[end:]
        source = source[:start] + source[start:].replace(signature, signature.replace(f'js_{name}(', f'js_{name}_impl('), 1)
    return source

def harness(out):
    text = (ROOT/'tools/games/test_derby_host.c').read_text()
    helper = '''
void norm_boundary_test(void);
void norm_lifecycle_test(JSContext *ctx);
extern unsigned long long norm_views,norm_oval,norm_ou,norm_sqrt,norm_view_ns,norm_ser_ns;
static FILE *norm_stream,*norm_times;
static unsigned long long norm_start;
static unsigned long long norm_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void norm_close(void){
    if(norm_stream)fclose(norm_stream);
    if(norm_times)fclose(norm_times);
    printf("NORM_COUNTS %llu %llu %llu %llu %llu %llu\\n",norm_views,norm_oval,norm_ou,norm_sqrt,norm_view_ns,norm_ser_ns);
}
'''
    text = replace(text, '/* ---- the procedural oracle */', helper+'\n/* ---- the procedural oracle */')
    text = replace(text, '    REQ(ni<=KSN_PROC_INPUTS);', '''    REQ(ni<=KSN_PROC_INPUTS);
    if(getenv("NORM_STREAM")){
        if(!norm_stream){norm_stream=fopen(getenv("NORM_STREAM"),"wb");REQ(norm_stream);}
        const uint32_t h[4]={(uint32_t)handle,ni,cur_surface,frame_draws};
        REQ(fwrite(h,sizeof h,1,norm_stream)==1);
        REQ(fwrite(in64,sizeof(double),ni,norm_stream)==ni);
    }''')
    text = replace(text, 'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;',
                   'churn_base=turn_hi=cur_bytes;norm_start=norm_now();return JS_UNDEFINED;')
    text = replace(text, '    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){', '''    const unsigned long long elapsed=norm_now()-norm_start;
    if(!norm_times){norm_times=fopen(getenv("NORM_TIMES"),"w");REQ(norm_times);}
    fprintf(norm_times,"%s,%llu,%zu,%zu\\n",scene,elapsed,n_alloc-churn_n,b_alloc-churn_b);
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    text = replace(text, '    REQ(loaded_seen==1);', '''    norm_boundary_test();
    norm_views=norm_oval=norm_ou=norm_sqrt=norm_view_ns=norm_ser_ns=0;
    REQ(loaded_seen==1);''')
    text=replace(text, '    eval(PRELUDE,strlen(PRELUDE),\"prelude.js\");', '    norm_lifecycle_test(ctx);\n    eval(PRELUDE,strlen(PRELUDE),\"prelude.js\");')
    path = out/'harness.c';path.write_text(text);return path

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,default=ROOT/'.cache/view-norm')
    ap.add_argument('--pairs',type=int,default=3)
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--rebuild',action='store_true',help='discard previous build objects when changing compiler flags')
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    assert args.pairs>0
    if args.rebuild:
        for label in ('base','reuse'):shutil.rmtree(out/label/'build',ignore_errors=True)
    if hasattr(os,'sched_getaffinity'):
        cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
    test_source=(ROOT/'tools/games/test_derby_view_norm.inc').read_text()
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    base=subprocess.check_output(['git','show',f'{BASE}:{SOURCE}'],cwd=ROOT,text=True)
    candidate=(ROOT/SOURCE).read_text()
    h=harness(out);bins={};apps={}
    flags=['-O2']+(['-fsanitize=address,undefined','-fno-pie','-no-pie'] if args.sanitize else [])
    for label,source in [('base',base),('reuse',candidate)]:
        folder=out/label;folder.mkdir(exist_ok=True)
        generated=folder/'pocket_derby.c'
        generated.write_text(instrument(source)+'\n#include <stdio.h>\n'+('#define NORM_REUSE\n' if label=='reuse' else '')+test_source)
        run_derby.EXTRA=[str(generated) if p.endswith('pocket_derby.c') else p for p in run_derby.EXTRA]
        if label=='reuse':
            for obj in (out/'base/build').glob('*.o'):
                (folder/'build').mkdir(exist_ok=True);shutil.copy2(obj,folder/'build'/obj.name)
        bins[label]=run_derby.build(flags,folder/'build',harness=h)
        apps[label]={}
        for course in ('oval','straight'):
            apps[label][course]=run_derby.force_course(run_derby.lower(ROOT/'apps/derby',folder/course),course)
    rows=[]
    for course in ('oval','straight'):
        for pair in range(args.pairs+1):
            for label in (('base','reuse') if pair%2==0 else ('reuse','base')):
                prefix=out/label/f'{course}-{pair}'
                env=dict(os.environ,DERBY_APP_DIR=str(apps[label][course]),DERBY_TIER='2',DERBY_PAN='0',
                         DERBY_HEAP_LIMIT='524288',NORM_TIMES=str(prefix)+'.times',
                         ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
                if args.sanitize and pair==0:env.update(DERBY_SER_ORACLE='1',DERBY_SER_FUZZ='4000')
                if pair==0:env.update(NORM_STREAM=str(prefix)+'.draws',DERBY_CSV=str(prefix)+'.csv')
                p=subprocess.run([str(bins[label])],cwd=ROOT,env=env,text=True,capture_output=True)
                prefix.with_suffix('.log').write_text(p.stdout+p.stderr)
                assert p.returncode==0 and p.stdout.rstrip().splitlines()[-2]=='DERBY_HOST PASS',str(prefix)
                times=[line.split(',') for line in prefix.with_suffix('.times').read_text().splitlines()]
                race=[int(r[1]) for r in times if r[0]=='race']
                row=dict(mode=label,course=course,pair=pair,
                         counts=list(map(int,re.search(r'NORM_COUNTS (.+)',p.stdout)[1].split())),
                         race_frames=len(race),race_ns_median=statistics.median(race),race_ns_p95=sorted(race)[round((len(race)-1)*.95)],
                         race_ns_total=sum(race),allocations=sum(int(r[2]) for r in times),allocated_bytes=sum(int(r[3]) for r in times),
                         pixels=re.search(r'pixels hash (\w+)',p.stdout)[1],finish=re.search(r'^finish: (.+)',p.stdout,re.M)[1])
                rows.append(row);print(json.dumps(row),flush=True)
                if pair==0:row['draw_sha256']=hashlib.sha256(prefix.with_suffix('.draws').read_bytes()).hexdigest()
        a,b=[r for r in rows if r['course']==course and r['pair']==0]
        for key in ('draw_sha256','pixels','finish','allocations','allocated_bytes','race_frames'):
            assert a[key]==b[key],(course,key)
        assert a['counts'][:3]==b['counts'][:3]
        assert a['counts'][3]==a['counts'][2] and b['counts'][3]==b['counts'][1]
        assert (out/'base'/f'{course}-0.csv').read_bytes()==(out/'reuse'/f'{course}-0.csv').read_bytes()
        for pair in range(1,args.pairs+1):
            a,b=[r for r in rows if r['course']==course and r['pair']==pair]
            for key in ('pixels','finish','allocations','allocated_bytes','race_frames'):assert a[key]==b[key]
            assert a['counts'][:3]==b['counts'][:3]
    result=dict(base=BASE,head=head,compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],machine=platform.machine(),flags=flags,cpu_affinity=sorted(os.sched_getaffinity(0)),counter_order=['views','oval_views','ou_calls','norm_evaluations','view_ns','ser_ns'],timing_pairs='pair>0 only; pair0 writes draw stream and CSV',test_sha256=hashlib.sha256(test_source.encode()).hexdigest(),candidate_sha256=hashlib.sha256(candidate.encode()).hexdigest(),sanitize=args.sanitize,
                scope='instrumented real app host frames; native timers include draw-hook validation, not device FPS',rows=rows)
    assert (ROOT/SOURCE).read_text()==candidate,'source changed during run'
    assert (ROOT/'tools/games/test_derby_view_norm.inc').read_text()==test_source,'test changed during run'
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('VIEW_NORM PASS',flush=True)
if __name__=='__main__':main()
