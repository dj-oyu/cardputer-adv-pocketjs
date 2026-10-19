#!/usr/bin/env python3
"""Compare existing clip-cache/runner-batch ports on the actual normal game."""
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
from derby_runner_batch import generate


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=host.ROOT/'.cache/derby_clip_batch')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    text=(host.ROOT/'tools/games/test_derby_host.c').read_text()
    marker='/* ---- the procedural oracle */'
    assert text.count(marker)==1
    text=text.replace(marker,'''static FILE *clip_batch_stream;
__attribute__((destructor)) static void close_clip_batch_stream(void){
    if(clip_batch_stream)REQ(fclose(clip_batch_stream)==0);
}
'''+marker)
    marker='    REQ(ni<=KSN_PROC_INPUTS);'
    assert text.count(marker)==1
    text=text.replace(marker,marker+'''
    if(getenv("DERBY_CLIP_BATCH_STREAM")){
        if(!clip_batch_stream){clip_batch_stream=fopen(getenv("DERBY_CLIP_BATCH_STREAM"),"wb");REQ(clip_batch_stream);}
        const uint32_t header[4]={(uint32_t)handle,ni,cur_surface,frame_draws};
        REQ(fwrite(header,sizeof header,1,clip_batch_stream)==1);
        REQ(fwrite(in64,sizeof(double),ni,clip_batch_stream)==ni);
    }
''')
    harness=out/'test_clip_batch_pipeline.c';harness.write_text(text)
    binaries={};apps={}
    for label,clip,batch in [('baseline',False,False),('clip',True,False),('batch',False,True),('combined',True,True)]:
        build=out/label/'build';build.mkdir(parents=True,exist_ok=True)
        if label!='baseline':
            for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
                shutil.copy2(out/'baseline'/'build'/f'{name}.o',build/f'{name}.o')
        binaries[label]=host.build(['-fsanitize=address,undefined'],build,harness=harness,clip_norm=clip,runner_batch=batch)
        apps[label]={}
        for course in ('seed','oval'):
            app=host.force_course(host.lower(host.ROOT/'apps/derby',out/label/course/'app'),course)
            if batch:generate(app)
            apps[label][course]=app
    source=(host.ROOT/'apps/derby/derby_view.js').read_text();a=source.index('const rin = ')+len('const rin = ')
    oracle=out/'runner-oracle.js';oracle.write_text((host.ROOT/'tools/games/derby_runner_batch_oracle.js').read_text().replace('__REFERENCE_RIN__',source[a:source.index(';',a)]))
    def trial(label,tier,course="seed",with_oracle=False):
        path=out/label/f'{course}-tier{tier}{"-oracle" if with_oracle else ""}';csv=path.with_suffix('.csv')
        env=dict(os.environ,DERBY_APP_DIR=str(apps[label][course]),DERBY_TIER=str(tier),DERBY_HEAP_LIMIT='524288',DERBY_CSV=str(csv),
                 ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for variable in ('DERBY_RUNNER_BATCH_ORACLE','DERBY_SER_ORACLE','DERBY_SER_FUZZ','DERBY_JS','DERBY_CLIP_BATCH_STREAM'):
            env.pop(variable,None)
        if with_oracle:env['DERBY_RUNNER_BATCH_ORACLE']=str(oracle)
        else:env['DERBY_CLIP_BATCH_STREAM']=str(path.with_suffix('.draws'))
        process=subprocess.run([str(binaries[label])],cwd=host.ROOT,env=env,capture_output=True,text=True,timeout=600)
        log=process.stdout+process.stderr;path.with_suffix('.log').write_text(log)
        if process.returncode or not log.rstrip().endswith('DERBY_HOST PASS'):raise RuntimeError(str(path.with_suffix('.log')))
        result=dict(mode=label,tier=tier,course=course,oracle=with_oracle,pixel_hash=re.search(r'^pixels hash (\w+)',log,re.M)[1],
                    finish=re.search(r'^finish: (.+)$',log,re.M)[1],csv_sha256=digest(csv))
        if with_oracle:
            result['boundary_stats']=json.loads(re.search(r'RUNNER_BATCH stats (.+)$',log,re.M)[1])
            stats=result['boundary_stats']
            assert stats['checked'] is True and stats['calls']>0 and stats['runners']>0,stats
            assert 'RUNNER_BATCH 2000 randomized native-boundary cases complete' in log
        else:result['draw_sha256']=digest(path.with_suffix('.draws'))
        print('CLIP_BATCH_PASS',json.dumps(result),flush=True);return result
    jobs=[(label,tier,'seed') for tier in (0,1,2) for label in binaries]+[(label,1,'oval') for label in binaries]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:results=list(pool.map(lambda job:trial(*job),jobs))
    for course,tier in [('seed',0),('seed',1),('seed',2),('oval',1)]:
        rows=[r for r in results if r['tier']==tier and r['course']==course]
        for field in ('pixel_hash','finish','csv_sha256','draw_sha256'):assert len({r[field] for r in rows})==1,(tier,field,rows)
    check=trial('combined',1,'oval',True);baseline=next(r for r in results if r['mode']=='baseline' and r['tier']==1 and r['course']=='oval')
    assert check['finish']==baseline['finish']
    report=dict(scope='normal-game host correctness only; all experiment defaults OFF; no device acceptance or speed claim',
                hardware_tested=False,results=results,boundary_oracle=check)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS clip/batch/combined: exact draw inputs/order, frame work/pixels, race and native boundary oracle',flush=True)


if __name__=='__main__':main()
