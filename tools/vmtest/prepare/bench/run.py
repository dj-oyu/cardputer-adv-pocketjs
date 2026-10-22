"""Build/run the real prepared startup benchmark. No device access.
Time and requested-payload allocator accounting are separate passes.
"""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
KASANE = ROOT / 'main/ui/kasane'

def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    fixture = (HERE.parent/'fixtures/runtime-startup.js').read_text()
    fallback = 'var benchCondition=1; if(benchCondition){\n' + fixture + '\n}'
    (out/'bench-input.h').write_text('static const char bench_fixture[]=' + json.dumps(fixture) + ';\nstatic const char bench_fallback[]=' + json.dumps(fallback) + ';\n')
    subprocess.run(['node',str(ROOT/'tools/kasane_ir/emit_rom_plans.mjs'),str(out/'runtime-rom.c'),f'derby={ROOT}/apps/derby/derby_prog.js'],check=True,cwd=ROOT)
    return fixture, fallback

def percentile(values, p):
    values=sorted(values)
    return values[round((len(values)-1)*p)]

def summarize(raw):
    rows=[json.loads(line) for line in raw.splitlines() if line.startswith('{')]
    assert rows[0]['schema']=='prepared-bench-v1' and rows[-1]=={'complete':True}
    samples=rows[1:-1]
    assert len(samples)==rows[0]['pairs']*8
    assert isinstance(rows[0]['pairs'],int) and 10<=rows[0]['pairs']<=10000
    identities=set()
    for r in samples:
        identity=(r['case'],r['pass'],r['pair'],r['order'])
        assert identity not in identities;identities.add(identity)
        assert r['case'] in ('supported','fallback') and r['pass'] in ('timing','accounting')
        assert 0<=r['pair']<rows[0]['pairs'] and r['order'] in (0,1)
        assert r['mode']==('prepared' if (r['pair']+r['order'])%2 else 'generic')
    result={'metadata':rows[0], 'distributions':{}}
    for case in ('supported','fallback'):
        for mode in ('generic','prepared'):
            for kind in ('timing','accounting'):
                group=[r for r in samples if (r['case'],r['mode'],r['pass'])==(case,mode,kind)]
                assert len(group)==rows[0]['pairs']
                assert all(r['hit']==int(case=='supported' and mode=='prepared') and r['handle']==1 and r['arg']==512 for r in group)
                keys=['ns'] if kind=='timing' else ['js_calls','js_bytes','js_peak_delta','js_live_delta','native_calls','native_bytes','native_peak_delta','native_live_delta','js_baseline','js_peak_live','native_baseline','native_peak_live']
                result['distributions'][f'{case}/{mode}/{kind}']={k:{'min':min(r[k] for r in group),'median':statistics.median(r[k] for r in group),'p95':percentile([r[k] for r in group],.95),'max':max(r[k] for r in group)} for k in keys}
        g={r['pair']:r['ns'] for r in samples if (r['case'],r['mode'],r['pass'])==(case,'generic','timing')}
        p={r['pair']:r['ns'] for r in samples if (r['case'],r['mode'],r['pass'])==(case,'prepared','timing')}
        ratios=[p[i]/g[i] for i in g]
        result['distributions'][case+'/paired_prepared_over_generic']={'median':statistics.median(ratios),'p05':percentile(ratios,.05),'p95':percentile(ratios,.95)}
    return result

def check_corruption(raw):
    # Negative controls use actual rows; malformed captures must never produce a result.
    rows=[json.loads(line) for line in raw.splitlines() if line.startswith('{')]
    bad=[]
    duplicate=[*rows];duplicate[2]=duplicate[1];bad.append(('duplicate',duplicate))
    bad.append(('missing_row',rows[:2]+rows[3:]))
    wrong=[dict(r) for r in rows];wrong[1]['mode']='prepared' if wrong[1]['mode']=='generic' else 'generic';bad.append(('wrong_order',wrong))
    wrong=[dict(r) for r in rows];wrong[1]['hit']=1-wrong[1]['hit'];bad.append(('wrong_hit',wrong))
    bad.append(('truncated',rows[:-1]))
    for name, mutated in bad:
        try: summarize('\n'.join(json.dumps(r) for r in mutated))
        except (AssertionError,KeyError): continue
        raise AssertionError(f'corrupt capture accepted: {name}')
    return [name for name,_ in bad]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--m32',action='store_true');ap.add_argument('--pairs',type=int,default=200);ap.add_argument('--generate-only',type=Path);ap.add_argument('--summarize',type=Path)
    args=ap.parse_args()
    if args.summarize:
        print(json.dumps(summarize(args.summarize.read_text()),indent=2));return
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    out=args.generate_only or ROOT/'.cache/prepared-bench'/('m32' if args.m32 else 'native')/stamp
    fixture,fallback=generate(out)
    if args.generate_only:return
    affinity=None
    if hasattr(os,"sched_getaffinity"):
        affinity=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{affinity})
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    dirty_start=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip())
    diff_start=subprocess.check_output(['git','diff','HEAD','--binary'],cwd=ROOT)
    abi=[]
    if args.m32:
        env=os.environ.copy();env.setdefault('M32_SYSROOT',str(ROOT/'.cache/kasane_megademo_app/m32sys'))
        abi=subprocess.check_output(['bash',str(ROOT/'tools/vmtest/m32_sysroot.sh')],cwd=ROOT,env=env,text=True).split()
    sources=[HERE/'bench.c',out/'runtime-rom.c',*(QJS/f'{n}.c' for n in ('quickjs','dtoa','libregexp','libunicode','quickjs-vm')),*(KASANE/f'{n}.c' for n in ('ksn_procedural','ksn_proc_analysis','ksn_proc_plan','ksn_proc_plan_points','ksn_proc_points','ksn_proc_points_dispatch','ksn_proc_points_pie'))]
    flags=['-std=gnu11','-O2','-g',*abi,'-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-DKSN_PROC_HOST_TEST','-DPOCKET_PREPARED_TEST',f'-DBENCH_HEAD="{head}"','-Wno-unused-parameter',*[v for d in (QJS,ROOT/'main',ROOT/'main/pocket',KASANE,ROOT/'tools/hostshim',out) for v in ('-I',str(d))]]
    binary=out/'bench'
    subprocess.run([sys.executable,str(ROOT/'tools/host_build.py'),'cc',*flags,*map(str,sources),'-lm','-o',str(binary)],cwd=ROOT,check=True)
    raw=subprocess.check_output([str(binary),str(args.pairs)],cwd=ROOT,text=True)
    (out/'trials.jsonl').write_text(raw)
    report=summarize(raw)
    report['negative_checks']=check_corruption(raw)
    report['recorded_at_utc']=stamp
    report['raw_sha256']=hashlib.sha256(raw.encode()).hexdigest()
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==head, 'HEAD changed during benchmark; rerun'
    assert subprocess.check_output(['git','diff','HEAD','--binary'],cwd=ROOT)==diff_start, 'source changed during benchmark; rerun'
    report['dirty']=dirty_start or bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip())
    report['host']={'cpu_affinity':affinity,'platform':platform.platform(),'machine':platform.machine(),'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0]}
    report['source_sha256']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [*sources,HERE/'run.py',QJS/'quickjs-prepared.h',QJS/'quickjs.h',ROOT/'main/pocket/pocket_proc.h',KASANE/'ksn_proc_plan.h',HERE.parent/'fixtures/runtime-startup.js',ROOT/'main/pocket/pocket_proc.c',QJS/'quickjs-prepared.inc'] if p.is_relative_to(ROOT)}
    report['compile_flags']=flags
    report['fixture_sha256']=hashlib.sha256(fixture.encode()).hexdigest();report['fallback_sha256']=hashlib.sha256(fallback.encode()).hexdigest()
    (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    print(f'BENCH_EVIDENCE {out}')
if __name__=='__main__':main()
