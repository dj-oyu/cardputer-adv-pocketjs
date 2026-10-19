#!/usr/bin/env python3
"""Host-only ON/OFF semantic and overhead control; no target speed claims."""
from pathlib import Path
import argparse,hashlib,json,statistics,subprocess,sys,shutil,os
p=argparse.ArgumentParser();p.add_argument('--baseline',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--batch',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--fast',action='store_true');p.add_argument('--verify-only',action='store_true');a=p.parse_args()
roots={v:getattr(a,v).resolve() for v in ('baseline','native','batch')};a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
summary={};reference=None;fast_objects=None
for variant,root in roots.items():
    results={}
    for mode in ('off','on'):
        out=a.out/(variant+'-'+mode)
        cmd=[sys.executable,str(root/'tools/games/run_derby_fixed.py'),'--out',str(out)]
        if mode=='on':cmd+=['--detail']
        if a.fast:cmd+=['--detail-fast']
        if a.verify_only:cmd+=['--verify-only']
        # All three exact trees share the QuickJS source; copy only an already
        # compiled O2 cache to avoid six identical recompilations in this run.
        if a.fast and fast_objects and not a.verify_only:
            qjs='components/quickjs-ng/quickjs-ng'
            for f in (roots['baseline']/qjs).glob('*'):
                if f.is_file():assert f.read_bytes()==(root/qjs/f.name).read_bytes()
            cache=out/('build-fast-detail' if mode=='on' else 'build-fast-off');cache.mkdir(parents=True,exist_ok=True)
            for f in fast_objects.glob('*.o'):shutil.copy2(f,cache/f.name)
        process=subprocess.run(cmd,cwd=root,capture_output=True,text=True)
        (a.out/(variant+'-'+mode+'-driver.log')).write_text(process.stdout+process.stderr)
        if process.returncode:raise RuntimeError(f'{variant} {mode} failed: '+process.stderr[-2000:])
        if a.fast and fast_objects is None:fast_objects=out/'build-fast-off'
        r=json.loads((out/'result.json').read_text());assert r['detail_enabled']==(mode=='on')
        assert r['trial']['all_repetitions_valid']
        for row in r['windows']:
            semantic={k:v for k,v in row.items() if k!='rep'}
            if reference is None:reference=semantic
            assert semantic==reference,(variant,mode,semantic)
        results[mode]=r
    assert results['on']['transfers']==results['off']['transfers']
    for before,after in zip(results['off']['trial']['repetitions'],results['on']['trial']['repetitions'],strict=True):
        for k in ('contexts','done','finish',*(f'c{i}' for i in range(10))):assert before[k]==after[k],(variant,k)
        for i in range(10):assert before[f's{i}'].split(':')[1]==after[f's{i}'].split(':')[1]
    off=[int(r['work_us']) for r in results['off']['trial']['repetitions']];on=[int(r['work_us']) for r in results['on']['trial']['repetitions']]
    summary[variant]={'off_work_us':off,'on_work_us':on,'median_ratio':statistics.median(on)/statistics.median(off),'semantic_equal':True,'transfers':results['on']['transfers'],'detail_events':results['on']['detail'][0]['events']}
report={'pass':True,'mode':'O2 without sanitizers' if a.fast else 'ASan/UBSan functional control','hardware_tested':False,'independent_trials':False,'scope':'3 dependent reps per run; host oracle and debug redraw included; not device overhead or target speed','reference_window':reference,'variants':summary}
(a.out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
