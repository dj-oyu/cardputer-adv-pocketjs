#!/usr/bin/env python3
"""Independent cache/batch matrix. Compare real draws/pixels/race, no device."""
import argparse
import concurrent.futures
import filecmp
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import run_derby
from derby_project_batch import generate

ROOT=Path(__file__).resolve().parents[2]

def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def harness(out):
    text=(ROOT/'tools/games/test_derby_host.c').read_text()
    marker='/* ---- the procedural oracle */'
    if text.count(marker)!=1:raise ValueError('host marker drift')
    helper='''
static FILE *projection_draw_stream;
__attribute__((destructor)) static void close_projection_stream(void){
    if(projection_draw_stream)REQ(fclose(projection_draw_stream)==0);
}
static JSValue js_projection_ns(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;struct timespec t;
    REQ(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return JS_NewFloat64(c,(double)t.tv_sec*1e9+t.tv_nsec);
}
static JSValue js_projection_detach(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;REQ(argc==1);JS_DetachArrayBuffer(c,argv[0]);return JS_UNDEFINED;
}
static JSValue js_projection_reset(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;pocket_derby_reset();return JS_UNDEFINED;
}
static JSValue js_projection_stats(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;JSValue o=JS_NewObject(c);
    uint32_t cache=0;bool cache_enabled=false;
#ifdef POCKET_DERBY_COURSE_CACHE
    cache_enabled=true;
#endif
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
    DerbyProjectionCounts counts;pocket_derby_projection_counts(&counts);cache=counts.cache_bytes;
    JS_SetPropertyStr(c,o,"registration_poses",JS_NewUint32(c,counts.registration_poses));
    JS_SetPropertyStr(c,o,"view_poses",JS_NewUint32(c,counts.view_poses));
    JS_SetPropertyStr(c,o,"project_calls",JS_NewUint32(c,counts.project_calls));
    JS_SetPropertyStr(c,o,"project_points",JS_NewUint32(c,counts.project_points));
    JS_SetPropertyStr(c,o,"input_numbers",JS_NewUint32(c,counts.input_numbers));
    JS_SetPropertyStr(c,o,"output_numbers",JS_NewUint32(c,counts.output_numbers));
#endif
    JS_SetPropertyStr(c,o,"cache_enabled",JS_NewBool(c,cache_enabled));
    JS_SetPropertyStr(c,o,"cache_bytes",JS_NewUint32(c,cache));return o;
}
'''
    text=text.replace(marker,helper+'\n'+marker)
    marker='    REQ(ni<=KSN_PROC_INPUTS);'
    if text.count(marker)!=1:raise ValueError('draw input marker drift')
    text=text.replace(marker,marker+'''
    if(getenv("DERBY_PROJECT_STREAM")){
        if(!projection_draw_stream){projection_draw_stream=fopen(getenv("DERBY_PROJECT_STREAM"),"wb");REQ(projection_draw_stream);}
        const uint32_t header[4]={(uint32_t)handle,ni,cur_surface,frame_draws};
        REQ(fwrite(header,sizeof header,1,projection_draw_stream)==1);
        REQ(fwrite(in64,sizeof(double),ni,projection_draw_stream)==ni);
    }
''')
    marker='    eval(PRELUDE,strlen(PRELUDE),"prelude.js");'
    if text.count(marker)!=1:raise ValueError('prelude marker drift')
    text=text.replace(marker,marker+'''
    {JSValue g=JS_GetGlobalObject(ctx);
     JS_SetPropertyStr(ctx,g,"__projectionNs",JS_NewCFunction(ctx,js_projection_ns,"__projectionNs",0));
     JS_SetPropertyStr(ctx,g,"__projectionDetach",JS_NewCFunction(ctx,js_projection_detach,"__projectionDetach",1));
     JS_SetPropertyStr(ctx,g,"__projectionReset",JS_NewCFunction(ctx,js_projection_reset,"__projectionReset",0));
     JS_SetPropertyStr(ctx,g,"__projectionStats",JS_NewCFunction(ctx,js_projection_stats,"__projectionStats",0));
     JS_FreeValue(ctx,g);}
''')
    marker='    if(getenv("DERBY_JS"))REQ(eval(getenv("DERBY_JS"),strlen(getenv("DERBY_JS")),"env.js"));'
    if text.count(marker)!=1:raise ValueError('env marker drift')
    text=text.replace(marker,marker+'''
    if(getenv("DERBY_PROJECT_BENCH_ONLY")){
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();
        JS_FreeContext(ctx);JS_FreeRuntime(rt);puts("PROJECTION_BENCH_ONLY PASS");return 0;
    }
''')
    path=out/'test_projection_pipeline.c';path.write_text(text);return path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/derby_projection')
    p.add_argument('--all-tiers',action='store_true')
    p.add_argument('--verify-only',action='store_true',help='recheck saved completed pipeline logs/CSVs')
    p.add_argument('--benchmark-only',action='store_true',help='optimized unsanitized complete boundary costs; serial trials')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    h=harness(out);binaries={};apps={}
    for label,cache,batch in [('baseline',False,False),('cache',True,False),('batch',False,True),('combined',True,True)]:
        if args.verify_only:
            binaries[label]=None
            continue
        build=out/label/'build';build.mkdir(parents=True,exist_ok=True)
        # These objects are compiled without application feature definitions.
        shared=out/'baseline'/'build'
        for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
            prior=shared/f'{name}.o'
            if prior.exists() and build!=shared and not (build/f'{name}.o').exists():shutil.copy2(prior,build/f'{name}.o')
        flags=['-O2'] if args.benchmark_only else ['-fsanitize=address,undefined']
        binaries[label]=run_derby.build(flags,build,camera_cache=cache,project_batch=batch,harness=h)
        apps[label]=run_derby.lower(ROOT/'apps/derby',out/label/'app')
        if batch:generate(apps[label])
    oracle=(ROOT/'tools/games/derby_project_oracle.js').read_text()
    def trial(label,tier,every=False):
        path=out/label/f'tier{tier}{"-oracle" if every else ""}'
        csv=path.with_suffix('.csv')
        if args.verify_only:
            text=path.with_suffix('.log').read_text()
            assert text.rstrip().endswith('DERBY_HOST PASS')
        else:
            env=dict(os.environ,DERBY_APP_DIR=str(apps[label]),DERBY_TIER=str(tier),DERBY_HEAP_LIMIT='524288',
                     DERBY_CSV=str(csv),DERBY_JS=('globalThis.__projectionEveryCall=true;' if every else '')+oracle,
                     DERBY_SER_ORACLE='1',DERBY_SER_FUZZ='4000',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                     UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            if args.benchmark_only:env['DERBY_PROJECT_BENCH_ONLY']='1'
            else:env['DERBY_PROJECT_STREAM']=str(path.with_suffix('.draws'))
            process=subprocess.run([str(binaries[label])],cwd=ROOT,env=env,capture_output=True,text=True)
            path.with_suffix('.log').write_text(process.stdout+process.stderr)
            if process.returncode:raise RuntimeError('projection failed: '+str(path.with_suffix('.log')))
            text=process.stdout
        assert ('PROJECTION_BENCH_ONLY PASS' if args.benchmark_only else 'DERBY_HOST PASS') in text
        bench=json.loads(re.search(r'PROJECTION_BENCH (\{[^\n]+\})',text)[1])
        assert bench['counters']['cache_enabled']==(label in ('cache','combined'))
        pre=re.search(r'PROJECTION_PREFLIGHT (\{[^\n]+\})',text)
        if args.benchmark_only:return dict(mode=label,benchmark=bench,preflight=json.loads(pre[1]) if pre else None)
        replay=re.search(r'^replay: (.+)$',text,re.M)
        return dict(mode=label,tier=tier,oracle=every,pixel_hash=re.search(r'^pixels hash (\w+)',text,re.M)[1],
                    finish=re.search(r'^finish: (.+)$',text,re.M)[1],replay=replay[1] if replay else None,
                    csv_sha256=digest(csv),benchmark=bench,
                    draw_sha256=digest(path.with_suffix('.draws')),
                    preflight=json.loads(pre[1]) if pre else None,
                    heap=re.search(r'^guest heap \(.+$',text,re.M)[0],
                    churn=re.findall(r'^guest heap churn in frame\(\), (.+)$',text,re.M))
    jobs=[(label,tier,False) for tier in ((0,1,2) if args.all_tiers else (1,)) for label in binaries]
    if not args.benchmark_only:jobs.append(('combined',1,True))
    results=[]
    if args.benchmark_only:
        for label,tier,every in jobs:
            r=trial(label,tier,every);results.append(r);print('PROJECTION_COST',json.dumps(r),flush=True)
        (out/'result.json').write_text(json.dumps(dict(scope='optimized host boundary costs; not target speed',results=results),indent=2)+'\n')
        return
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        work={pool.submit(trial,*job):job for job in jobs}
        for f in concurrent.futures.as_completed(work):
            r=f.result();results.append(r);print('PROJECTION_PASS',r['mode'],r['tier'],'oracle',r['oracle'],r['pixel_hash'],flush=True)
    for tier in ((0,1,2) if args.all_tiers else (1,)):
        rows=[r for r in results if r['tier']==tier]
        for key in ('pixel_hash','finish','replay','csv_sha256','draw_sha256'):
            assert len({r[key] for r in rows})==1,(tier,key,[(r['mode'],r[key]) for r in rows])
        baseline=out/'baseline'/f'tier{tier}.draws'
        for r in rows:
            path=out/r['mode']/f'tier{tier}{"-oracle" if r["oracle"] else ""}.draws'
            assert filecmp.cmp(baseline,path,shallow=False),'draw stream bytes differ: '+str(path)
    report=dict(scope='exact host draws/pixels/race and full JS/native boundary costs; no target speed claim',results=results)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS independent cache, batch and combination; draw order, visibility, pixels and race unchanged',flush=True)

if __name__=='__main__':main()
