"""Inject only the managed skin workspace allocation and symbolize its call site."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[3]
files=[*Path(__file__).parent.glob('*.*'),ROOT/'main/pocket/pocket_ayatsuri_model.inc',ROOT/'apps/lib/ayatsuri_model.js']
def hashes():
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
before=hashes()
sys.path.insert(0,str(ROOT/'tools/kasane_contract'))
import run_meshbench_app as host

actual=[]
def run(command, **kwargs):
    command=list(map(str,command))
    if command[0]=='cc' and any(x.endswith('skin_oom/test_site.c') for x in command):
        command += ['-no-pie', '-Wl,--wrap=js_malloc', '-Wl,--wrap=ayatsuri_model_instance_evaluate', '-Wl,--wrap=ayatsuri_model_instance_evaluate_meshes']
        actual.append(command)
    return subprocess.run(command, **kwargs)
host.subprocess=SimpleNamespace(run=run,check_output=subprocess.check_output,DEVNULL=subprocess.DEVNULL)
fixture=ROOT/'.cache/skin-oom-fixture'
subprocess.run(['node',str(ROOT/'tools/ayatsuri/dq_mixed/fixture.cjs'),str(fixture)],check=True)
binary,out,env=host.build_harness('tools/ayatsuri/skin_oom/test_site.c','skin-oom-host')
commands=json.loads((out/'commands.json').read_text());commands['commands'][-1]=actual[-1]
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
sites=[]
for rows in (4,8):
    for direct in (0,1):
        result=subprocess.run([str(binary),str(rows),str(direct),str(fixture)],cwd=ROOT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (out/f'rows{rows}-direct{direct}.log').write_text(result.stdout)
        print(result.stdout,end='');result.check_returncode()
        matches=re.findall(r'ALLOCATION_SITE mode=(\d+) phase=(\d+) bytes=(\d+) pc=(0x[0-9a-f]+)',result.stdout)
        assert len(matches)==12
        for mode,phase,size,pc in matches:
            symbol=subprocess.check_output(['addr2line','-f','-e',str(binary),pc],text=True).strip()
            expected='managed_instance_evaluate_skins_mode' if int(mode)>=2 else 'managed_instance_evaluate_skin_mode'
            assert symbol.splitlines()[0].startswith(expected),symbol
            source=symbol.splitlines()[1].split(' (discriminator')[0]
            path,line=source.rsplit(':',1)
            assert 'js_malloc(ctx,bytes)' in Path(path).read_text().splitlines()[int(line)-1],symbol
            sites.append(dict(rows=rows,direct=direct,mode=int(mode),phase=int(phase),bytes=int(size),symbol=symbol))
assert before==hashes(), 'sources changed during run'
(out/'sites.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'sites':sites,'sources':hashes(),'logs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('rows*.log')}},indent=2)+'\n')
print('SKIN_NATIVE_OOM_ALL_ARMS_PASS 48 symbolized allocation failures; host only')
