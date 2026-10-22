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
import tempfile

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

def summarize_compact(raw):
    lines=raw.splitlines()
    rows=[json.loads(line) for line in lines if line.startswith('{')]
    assert len(rows)==10
    footer_line=max(i for i,line in enumerate(lines) if line.startswith('{'))
    markers=[i for i,line in enumerate(lines) if line=='PREPARED_BENCH_DONE no display/USB commands issued']
    assert markers==[footer_line+1]
    meta,footer=rows[0],rows[-1]
    assert meta['schema']=='prepared-bench-compact-v1' and meta['approximate'] is False and meta['selftest'] is True
    assert len(meta['head'])==40 and all(c in '0123456789abcdef' for c in meta['head'])
    assert meta['pairs']==64 and meta['warmup_pairs']==8 and meta['pointer_bytes'] in (4,8)
    assert 0<meta['summary_storage_bytes']<=2048
    assert footer['complete'] is True and footer['verified_trials']==576 and footer['recorded_trials']==512
    assert footer['warmup_trials']==64 and footer['prepared_hits_total']==144 and footer['prepared_hits_recorded']==128
    assert footer['elapsed_ms']<24000 and footer['summary_bytes_before_footer']<8192
    seen=set()
    for row in rows[1:-1]:
        key=(row['case'],row['pass'],row['mode']);assert key not in seen;seen.add(key)
        assert row['case'] in ('supported','fallback') and row['pass'] in ('timing','accounting') and row['mode'] in ('prepared','generic')
        assert row['n']==64 and row['hits']==(64 if key[0]=='supported' and key[2]=='prepared' else 0)
        assert 0<=row['min_ns']<=row['median_ns']<=row['p95_ns']<=row['max_ns']<24000000000
        keys={'js_calls','js_bytes','js_peak_delta','js_live_delta','js_baseline','js_peak_live','native_calls','native_bytes','native_peak_delta','native_live_delta','native_baseline','native_peak_live','internal_free_before','internal_min_free','internal_largest_after'}
        assert set(row['ranges'])==keys
        assert all(isinstance(v,list) and len(v)==2 and all(type(x) is int for x in v) and v[0]<=v[1] for v in row['ranges'].values())
    return {'metadata':meta,'groups':rows[1:-1],'verification':footer}

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

def verify_device(build):
    config={}
    for line in (build/'sdkconfig').read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key,value=line.split('=',1);config[key]=value.strip('"')
    assert config['CONFIG_IDF_TARGET']=='esp32s3' and config['CONFIG_PARTITION_TABLE_CUSTOM']=='y'
    assert config['CONFIG_LOG_DEFAULT_LEVEL']=='3' and int(config['CONFIG_LOG_MAXIMUM_LEVEL'])>=3
    assert config['CONFIG_ESP_MAIN_TASK_STACK_SIZE']=='32768' and config['CONFIG_FREERTOS_HZ']=='1000'
    # Product sdkconfig.defaults inherits these disabled IDF6.0.1 security defaults.
    security={key:config.get(key,'n') for key in ('CONFIG_SECURE_BOOT','CONFIG_SECURE_BOOT_V2_ENABLED','CONFIG_SECURE_FLASH_ENC_ENABLED','CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK')}
    assert all(value=='n' for value in security.values()),security
    partition=build/'partition_table/partition-table.bin'
    with tempfile.TemporaryDirectory(prefix='prepared-partition-') as tmp:
        expected=Path(tmp)/'product.bin'
        subprocess.run([sys.executable,str(Path(os.environ['IDF_PATH'])/'components/partition_table/gen_esp32part.py'),'-q','--offset','0x8000','--flash-size','8MB',str(ROOT/'partitions.csv'),str(expected)],check=True)
        assert expected.read_bytes()==partition.read_bytes(),'partition differs from product'
    elf=build/'cardputer_prepared_bench.elf'
    assert b'Application information:' in elf.read_bytes(),'INFO app_init boot gate missing'
    proof={'partition_matches_product':True,'partition_sha256':hashlib.sha256(partition.read_bytes()).hexdigest(),'product_partition_csv_sha256':hashlib.sha256((ROOT/'partitions.csv').read_bytes()).hexdigest(),'security':security,'info_application_information_present':True,'deadline_ms':24000,'capture_contract':{'max_ms':30000,'rx_bytes':65536,'retained_lines':256,'retained_bytes':32768},'summary_static_bytes_max':2048}
    (build/'device-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2))

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--m32',action='store_true');ap.add_argument('--pairs',type=int,default=200);ap.add_argument('--generate-only',type=Path);ap.add_argument('--summarize',type=Path);ap.add_argument('--compact-check',action='store_true');ap.add_argument('--verify-device-build',type=Path)
    args=ap.parse_args()
    if args.verify_device_build:
        verify_device(args.verify_device_build);return
    if args.summarize:
        raw=args.summarize.read_text();parse=summarize_compact if 'prepared-bench-compact-v1' in raw else summarize
        print(json.dumps(parse(raw),indent=2));return
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
    if args.compact_check:flags.append('-DBENCH_COMPACT');args.pairs=64
    binary=out/'bench'
    subprocess.run([sys.executable,str(ROOT/'tools/host_build.py'),'cc',*flags,*map(str,sources),'-lm','-o',str(binary)],cwd=ROOT,check=True)
    raw=subprocess.check_output([str(binary),str(args.pairs)],cwd=ROOT,text=True)
    (out/'trials.jsonl').write_text(raw)
    report=summarize_compact(raw) if args.compact_check else summarize(raw)
    if args.compact_check:
        assert len(raw.encode())<8192 and len(raw.splitlines())<=12
        for corrupt in ('PREPARED_BENCH_DONE no display/USB commands issued\n'+raw.replace('PREPARED_BENCH_DONE no display/USB commands issued\n',''),raw.replace('\"verified_trials\":576','\"verified_trials\":575')):
            try:summarize_compact(corrupt)
            except AssertionError:pass
            else:raise AssertionError('corrupt compact capture accepted')
        report['negative_checks']=['misplaced_done','wrong_verified_count']
    else:report['negative_checks']=check_corruption(raw)
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
