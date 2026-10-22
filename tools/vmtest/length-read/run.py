"""One-change A/B against integration578d552. No prepared or hardware path."""
from pathlib import Path
import subprocess,json,sys,os,statistics,argparse,hashlib,datetime,math
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
K=ROOT/'main/ui/kasane'
BASE='578d552384c992cb852822d40e2635e7b317d498'
def main():
 p=argparse.ArgumentParser();p.add_argument('--pairs',type=int,default=40);p.add_argument('--loops',type=int,default=5000);p.add_argument('--m32',action='store_true');p.add_argument('--asan',action='store_true');p.add_argument('--out',type=Path);a=p.parse_args()
 if a.pairs<2 or not 1<=a.loops<=100000:p.error('pairs >= 2 and loops in 1..100000 required')
 out=(a.out or ROOT/'.cache/length-read')/('m32' if a.m32 else 'native')/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ');out=out.resolve();out.mkdir(parents=True,exist_ok=True)
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 baseline=subprocess.check_output(['git','show',BASE+':main/pocket/pocket_proc.c'],cwd=ROOT)
 (out/'baseline.c').write_bytes(baseline)
 subprocess.run(['node',str(ROOT/'tools/kasane_ir/emit_rom_plans.mjs'),str(out/'rom.c'),f'derby={ROOT}/apps/derby/derby_prog.js'],check=True)
 flags=['-std=gnu11','-O2','-g','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-DKSN_PROC_HOST_TEST','-Wno-unused-parameter',*[v for d in (QJS,ROOT/'main',ROOT/'main/pocket',K,ROOT/'tools/hostshim') for v in ('-I',str(d))]]
 if a.m32 and os.environ.get('LENGTH_READ_SYSTEM_M32')=='1':flags+=['-m32','-malign-double']
 elif a.m32:flags+=subprocess.check_output(['bash',str(ROOT/'tools/vmtest/m32_sysroot.sh')],env={**os.environ,'M32_SYSROOT':str(ROOT/'.cache/kasane_megademo_app/m32sys')},text=True).split()
 if a.asan:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 sources=[out/'rom.c',*(QJS/f'{n}.c' for n in ('quickjs','dtoa','libregexp','libunicode','quickjs-vm')),*(K/f'{n}.c' for n in ('ksn_procedural','ksn_proc_analysis','ksn_proc_plan','ksn_proc_plan_points','ksn_proc_points','ksn_proc_points_dispatch','ksn_proc_points_pie'))]
 bins={};allocbins={}
 for mode,proc in [('baseline',out/'baseline.c'),('candidate',ROOT/'main/pocket/pocket_proc.c')]:
  wrapper=out/(mode+'-bench.c');wrapper.write_text('#define PROC_SOURCE '+json.dumps(str(proc))+'\n#include '+json.dumps(str(HERE/'bench.c'))+'\n')
  mode_sources=sources
  binary=out/(mode+('-asan' if a.asan else ''))
  subprocess.run([sys.executable,str(ROOT/'tools/host_build.py'),'cc',*flags,str(wrapper),*map(str,mode_sources),'-lm','-o',str(binary)],check=True,cwd=ROOT);bins[mode]=binary
  allocbinary=out/(mode+'-alloc')
  allocwrapper=out/(mode+'-alloc-bench.c');allocwrapper.write_text('#define NUMBER_ACCOUNTING 1\n'+wrapper.read_text())
  subprocess.run([sys.executable,str(ROOT/'tools/host_build.py'),'cc',*flags,str(allocwrapper),*map(str,mode_sources),'-lm','-o',str(allocbinary)],check=True,cwd=ROOT);allocbins[mode]=allocbinary
 rows=[]
 for kind in ('startup','numeric','array'):
  for pair in range(-4,a.pairs):
   results={}
   for order in range(2):
    mode=('baseline','candidate')[(pair+order)&1]
    d=json.loads(subprocess.check_output([str(bins[mode]),kind,str(a.loops)],text=True));results[mode]=d
    if pair>=0:rows.append({**d,'pair':pair,'order':order,'mode':mode})
   assert all(results['baseline'][k]==results['candidate'][k] for k in ('hash','n','pointer_bytes','boundaries'))
 summary={'base':BASE,'baseline_source_sha256':hashlib.sha256(baseline).hexdigest(),'bench_sha256':hashlib.sha256((HERE/'bench.c').read_bytes()).hexdigest(),'candidate_source_sha256':hashlib.sha256((ROOT/'main/pocket/pocket_proc.c').read_bytes()).hexdigest(),'pairs':a.pairs,'loops':a.loops,'asan':a.asan,'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0],'flags':flags,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'cpu_affinity':sorted(os.sched_getaffinity(0)) if hasattr(os,'sched_getaffinity') else None,'groups':{},'allocations':{}}
 for kind in ('startup','numeric','array'):
  group={}
  for mode in bins:
   v=sorted(r['ns']/r['n'] for r in rows if r['case']==kind and r['mode']==mode)
   group[mode]={'median_ns_per_op':statistics.median(v),'p95_ns_per_op':v[math.ceil(len(v)*.95)-1]}
  for mode in bins:
   v=sorted(r['cpu_ns']/r['n'] for r in rows if r['case']==kind and r['mode']==mode)
   group[mode].update(median_cpu_ns_per_op=statistics.median(v),p95_cpu_ns_per_op=v[math.ceil(len(v)*.95)-1])
  ratios=[next(r['ns'] for r in rows if r['case']==kind and r['mode']=='candidate' and r['pair']==i)/next(r['ns'] for r in rows if r['case']==kind and r['mode']=='baseline' and r['pair']==i) for i in range(a.pairs)]
  group['paired_candidate_over_baseline']={'median':statistics.median(ratios),'p05':sorted(ratios)[math.ceil(len(ratios)*.05)-1],'p95':sorted(ratios)[math.ceil(len(ratios)*.95)-1]};summary['groups'][kind]=group
  summary['allocations'][kind]={}
  for mode,binary in allocbins.items():
   run=subprocess.run([str(binary),kind,str(a.loops)],text=True,capture_output=True,check=True)
   metrics=json.loads(run.stderr);result=json.loads(run.stdout);assert result['hash']==next(r['hash'] for r in rows if r['case']==kind)
   summary['allocations'][kind][mode]={'n':result['n'],**metrics}
 (out/'trials.json').write_text(json.dumps(rows,indent=2)+'\n');(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'output':str(out),**summary},indent=2))
if __name__=='__main__':main()
