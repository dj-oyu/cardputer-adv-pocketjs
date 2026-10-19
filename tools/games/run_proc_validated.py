#!/usr/bin/env python3
"""Compare OFF/reference/validated procedural execution on the real game."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import run_derby as host


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=host.ROOT/'.cache/derby_validated')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    text=(host.ROOT/'tools/games/test_derby_host.c').read_text()
    marker='/* ---- the procedural oracle */'
    assert text.count(marker)==1
    text=text.replace(marker,'''static FILE *validated_stream;
__attribute__((destructor)) static void close_validated_stream(void){
    if(validated_stream)REQ(fclose(validated_stream)==0);
}
'''+marker)
    marker='    REQ(ni<=KSN_PROC_INPUTS);'
    assert text.count(marker)==1
    text=text.replace(marker,marker+'''
    if(getenv("DERBY_PROC_VALIDATED_STREAM")){
        if(!validated_stream){validated_stream=fopen(getenv("DERBY_PROC_VALIDATED_STREAM"),"wb");REQ(validated_stream);}
        const uint32_t header[4]={(uint32_t)handle,ni,cur_surface,frame_draws};
        REQ(fwrite(header,sizeof header,1,validated_stream)==1);
        REQ(fwrite(in64,sizeof(double),ni,validated_stream)==ni);
    }
''')
    marker='    printf("%s\\n",pass?"DERBY_HOST PASS":"DERBY_HOST FAIL");'
    assert text.count(marker)==1
    text=text.replace(marker,'''#ifdef KSN_PROC_VALIDATED_PLAN
    {pocket_proc_validated_trace p;pocket_proc_validated_trace_read(&p);
     printf("PROC_VALIDATED_PROFILE {\\"prepared\\":%u,\\"fast_draws\\":%u,\\"reference_draws\\":%u,\\"live_bytes\\":%zu,\\"peak_bytes\\":%zu}\\n",
            p.prepared,p.fast_draws,p.reference_draws,p.live_bytes,p.peak_bytes);}
#endif
'''+marker)
    harness=out/'test_validated_pipeline.c';harness.write_text(text)
    binaries={};apps={}
    for label,enabled,reference in [('baseline',False,False),('reference',True,True),('validated',True,False)]:
        build=out/label/'build';build.mkdir(parents=True,exist_ok=True)
        if label!='baseline':
            for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
                shutil.copy2(out/'baseline'/'build'/f'{name}.o',build/f'{name}.o')
        binaries[label]=host.build(['-fsanitize=address,undefined'],build,harness=harness,validated_plan=enabled,validated_reference=reference,validated_trace=enabled)
        apps[label]={}
        for course in ('seed','oval'):
            app=host.force_course(host.lower(host.ROOT/'apps/derby',out/label/course/'app'),course)
            apps[label][course]=app
    def trial(label,tier,course="seed"):
        path=out/label/f'{course}-tier{tier}';csv=path.with_suffix('.csv')
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label][course]),DERBY_TIER=str(tier),DERBY_HEAP_LIMIT='524288',DERBY_CSV=str(csv),
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for variable in ('DERBY_RUNNER_BATCH_ORACLE','DERBY_SER_ORACLE','DERBY_SER_FUZZ','DERBY_JS','DERBY_PROC_VALIDATED_STREAM'):
            env.pop(variable,None)
        env['DERBY_PROC_VALIDATED_STREAM']=str(path.with_suffix('.draws'))
        process=subprocess.run([str(binaries[label])],cwd=host.ROOT,env=env,capture_output=True,text=True,timeout=600)
        log=process.stdout+process.stderr;path.with_suffix('.log').write_text(log)
        if process.returncode or not log.rstrip().endswith('DERBY_HOST PASS'):raise RuntimeError(str(path.with_suffix('.log')))
        result=dict(mode=label,tier=tier,course=course,pixel_hash=re.search(r'^pixels hash (\w+)',log,re.M)[1],
                    finish=re.search(r'^finish: (.+)$',log,re.M)[1],csv_sha256=digest(csv))
        result['draw_sha256']=digest(path.with_suffix('.draws'))
        if label!='baseline':
            result['profile']=json.loads(re.search(r'PROC_VALIDATED_PROFILE (.+)$',log,re.M)[1])
            p=result['profile'];assert p['prepared']>0 and p['peak_bytes']>0 and p['live_bytes']==0,p
            assert (p['fast_draws']>0)==(label=='validated'),p
            assert (p['reference_draws']>0)==(label=='reference'),p
        print('PROC_VALIDATED_PASS',json.dumps(result),flush=True);return result
    jobs=[(label,tier,'seed') for tier in (0,1,2) for label in binaries]+[(label,1,'oval') for label in binaries]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:results=list(pool.map(lambda job:trial(*job),jobs))
    for course,tier in [('seed',0),('seed',1),('seed',2),('oval',1)]:
        rows=[r for r in results if r['tier']==tier and r['course']==course]
        for field in ('pixel_hash','finish','csv_sha256','draw_sha256'):assert len({r[field] for r in rows})==1,(tier,field,rows)
    report=dict(scope='normal-game host correctness only; validated option OFF by default; setup/execute timings are separate host probe; no device acceptance or speed claim',
                hardware_tested=False,results=results)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS OFF/reference/validated: exact draw inputs/order, frame work/pixels, race and checked native execution profiles',flush=True)


if __name__=='__main__':main()
