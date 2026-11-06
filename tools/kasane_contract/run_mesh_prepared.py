"""Private opt-in setup backend regression; original oracle remains linked unchanged."""
import os, runpy, subprocess, sys, json, hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
mode=sys.argv[1] if len(sys.argv)>1 else 'unit'
sys.argv=sys.argv[:1]+sys.argv[2:]
no_lsan='--no-lsan' in sys.argv
if no_lsan:
    sys.argv.remove('--no-lsan')
    print('LSAN_DISABLED: ptrace-host limitation; ASan/UBSan remain enabled',flush=True)
edge4='--edge4' in sys.argv
if edge4:sys.argv.remove('--edge4')
host_stats='--no-stats' not in sys.argv
if not host_stats:sys.argv.remove('--no-stats')
edge_root=ROOT/'main/ui/kasane'
original=subprocess.run
trace_dir=ROOT/'.cache/prepared';trace_dir.mkdir(parents=True,exist_ok=True)
trace=trace_dir/(mode+('-edge4' if edge4 else '')+('-no-stats' if not host_stats else '')+'.commands.jsonl');trace.write_text('')
if edge4:
    (trace_dir/'edge4-inputs.json').write_text(json.dumps({name:hashlib.sha256((edge_root/name).read_bytes()).hexdigest() for name in ['pie_edge4.h','pie_edge4.c','pie_edge4_raw.c']},indent=2)+'\n')

def run(cmd, *args, **kwargs):
    cmd=list(map(str,cmd))
    if cmd and cmd[0]=='cc':
        cmd+=['-DKSN_MESH_PREPARED_PROTOTYPE=1','-I'+str(ROOT/'.cache/prepared')]
        if any(x.endswith('/test_mesh_depth_qjs.c') for x in cmd):
            out=ROOT/'.cache/prepared';out.mkdir(parents=True,exist_ok=True)
            src=(ROOT/'tools/kasane_contract/test_mesh_depth_qjs.c').read_text()
            (out/'original_depth_suite.inc').write_text(src.replace('int main(void){','int original_mesh_depth_suite(void){'))
        cmd=[x.replace('/test_mesh_depth_qjs.c','/test_mesh_prepared_lifecycle.c') for x in cmd]
        if any(x.endswith('/ksn_mesh_depth.c') for x in cmd) and not any(x.endswith('/ksn_mesh_prepared.c') for x in cmd):
            cmd+=[str(ROOT/'main/ui/kasane/ksn_mesh_prepared.c')]
        if edge4 and any(x.endswith('/ksn_mesh_prepared.c') for x in cmd):
            cmd+=['-DKSN_MESH_PREPARED_EDGE4=1',*(['-DKSN_MESH_EDGE4_HOST_STATS=1'] if host_stats else []),'-I'+str(edge_root),str(edge_root/'pie_edge4.c'),str(edge_root/'pie_edge4_raw.c')]
    if no_lsan and 'env' in kwargs:
        kwargs['env']=dict(kwargs['env'],ASAN_OPTIONS='detect_leaks=0:abort_on_error=1')
    with trace.open('a') as output: output.write(json.dumps(cmd)+'\n')
    return original(cmd,*args,**kwargs)

if mode in ('depth','app'):
    subprocess.run=run
    runpy.run_path(str(ROOT/'tools/kasane_contract'/('run_mesh_depth.py' if mode=='depth' else 'run_meshbench_app.py')),run_name='__main__')
elif mode=='unit':
    out=ROOT/'.cache/prepared';out.mkdir(parents=True,exist_ok=True)
    sources=['main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_mesh_depth.c','main/ui/kasane/ksn_mesh_prepared.c','main/ui/kasane/ksn_shiki_frame.c','tools/kasane_contract/test_mesh_prepared.c']
    run(['cc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),*[str(ROOT/p) for p in sources],'-lm','-o',str(out/'test')],check=True)
    run([str(out/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS=('detect_leaks=0:abort_on_error=1' if no_lsan else 'detect_leaks=1:abort_on_error=1'),UBSAN_OPTIONS='halt_on_error=1'))
else: raise SystemExit('expected unit, depth, or app')
