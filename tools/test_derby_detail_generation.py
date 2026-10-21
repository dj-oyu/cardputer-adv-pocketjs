"""Regeneration and exact OFF object-code preservation against the source HEAD."""
from pathlib import Path
import sys,subprocess,tempfile,hashlib,json,re
R=Path(__file__).resolve().parents[1];sys.path.insert(0,str(R/'tools'))
from derby_fixed_workload_chunks import generate,CONTROLLER_LOAD,CONTROLLER_STRICT
VARIANT=re.search(r'df_variant\(void\)\{return "(baseline|native|batch)";', (R/'main/pocket/pocket_derby.c').read_text())[1]
BASE=json.loads((R/'tools/derby_detail_bases.json').read_text())[VARIANT]
# Intentional production memory change, independently frozen from the
# historical detail-instrumentation baseline. Other files keep that baseline.
# This reference is a fixed published product commit, never the running HEAD.
PROCEDURAL_WORKSPACE_BASE='4e4d7bccd6ed461d4b17072db118b453ce62722b'
with tempfile.TemporaryDirectory() as tmp:
    out=Path(tmp);current=out/'generated';generate(R/'apps/derby',current,True)
    def hashes():return {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in current.glob('*.js')}
    on=hashes();generate(R/'apps/derby',current,True);assert on==hashes()
    generate(R/'apps/derby',current,False);generate(R/'apps/derby',current,True);assert on==hashes()
    oldmod=out/'old_generator.py';oldmod.write_bytes(subprocess.check_output(['git','show',BASE+':tools/derby_fixed_workload_chunks.py'],cwd=R))
    subprocess.run([sys.executable,str(oldmod),str(R/'apps/derby'),str(out/'old')],env=__import__('os').environ|{'PYTHONPATH':str(R/'tools')},check=True)
    generate(R/'apps/derby',out/'off')
    for p in (out/'old').glob('*.js'):
        current=(out/'off'/p.name).read_bytes()
        if p.name=='derby_play.js':
            controller=(out/'off/derby_fixed.js').read_bytes()
            assert controller.startswith(CONTROLLER_STRICT.encode())
            assert 'if(dfMode===5)' in controller.decode()
            oldplay=p.read_bytes();begin=oldplay.index(b'\n// DF fixture oval-pan8-390-510-v1:')
            end=oldplay.index(b"\nenter('pad');",begin)
            current=current.replace(CONTROLLER_LOAD.encode(),oldplay[begin:end],1)
        assert p.read_bytes()==current,p.name
    print('PASS repeated ON/OFF generation; replacing only reviewed controller exactly reconstructs comparison-base OFF play')
    # Native VM/procedural/renderer code must compile to identical .text at O0.
    headers=['components/quickjs-ng/quickjs-ng','components/pocketjs_guest/include','tools/hostshim','main','main/pocket','main/ui','main/ui/kasane','main/text','main/hal']
    (out/'main/ui/kasane').mkdir(parents=True);(out/'main/pocket').symlink_to(R/'main/pocket',target_is_directory=True)
    for f in ('main/pocket/pocket_derby.c','main/pocket/pocket_proc.c','main/ui/kasane/ksn_procedural.c','main/ui/kasane/ksn_proc_plan.c','main/ui/kasane/ksn_render.c'):
        old=out/('main/ui/kasane/'+Path(f).name) if '/ui/' in f else out/Path(f).name
        reference=PROCEDURAL_WORKSPACE_BASE if f=='main/pocket/pocket_proc.c' else BASE
        old.write_bytes(subprocess.check_output(['git','show',reference+':'+f],cwd=R))
        texts=[]
        for i,p in enumerate((old,R/f)):
            obj=out/f'{i}.o';raw=out/f'{i}.text'
            subprocess.run(['gcc','-std=gnu11','-O0','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',*['-I'+str(R/h) for h in headers],'-c',str(p),'-o',str(obj)],check=True)
            subprocess.run(['objcopy','--only-section=.text','-O','binary',str(obj),str(raw)],check=True)
            texts.append(raw.read_bytes())
            if i==1:assert ' dd_' not in subprocess.check_output(['nm',str(obj)],text=True)
        assert texts[0]==texts[1],f+' OFF .text changed'
        if f=='main/pocket/pocket_proc.c':
            # A new OFF behavior change must still be rejected after moving
            # this one reference. Mutate a return that is outside DD scopes.
            mutated=out/'mutated_proc.c'
            source=(R/f).read_text()
            before='if(!count)return KSN_OK;'
            assert source.count(before)==1
            mutated.write_text(source.replace(before,'if(!count)return KSN_INVALID;'))
            obj=out/'mutated.o';raw=out/'mutated.text'
            subprocess.run(['gcc','-std=gnu11','-O0','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',
                            *['-I'+str(R/h) for h in headers],'-c',str(mutated),'-o',str(obj)],check=True)
            subprocess.run(['objcopy','--only-section=.text','-O','binary',str(obj),str(raw)],check=True)
            assert raw.read_bytes()!=texts[0],'OFF behavior mutation escaped frozen reference'

    print('PASS five production C objects OFF .text byte-identical to frozen references at O0; no dd symbols; workspace OFF mutation rejected')
