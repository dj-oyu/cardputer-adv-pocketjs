#!/usr/bin/env python3
"""Exercise the finite diagnostic through real QuickJS/Kasane, without USB."""
import argparse,ast,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
import run_bigwave_curl as bench
import prepare_bigwave_curl_diag as diag

p=argparse.ArgumentParser();p.add_argument('--common',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--objects',type=Path);a=p.parse_args()
out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
test=a.common.parent/'test_derby_app_diag.py'
node=next(n for n in ast.parse(test.read_text()).body if isinstance(n,ast.FunctionDef) and n.name=='test_owner')
env={'subprocess':subprocess,'tempfile':tempfile,'Path':Path}
exec(compile(ast.Module(body=[node],type_ignores=[]),str(test),'exec'),env)
env['test_owner'](diag.owner_template(a.common.parent/'device_diag',bench.BASE))
original=bench.harness
def harness(folder):
    h=original(folder);s=h.read_text()
    s=bench.replace(s,'        }else frames(120,"");','        }else frames(120,"");\n        if(getenv("CURL_DIAG"))run("if(__diagN!==120||__diagOpen)throw Error(\"diag sample count\")");'.replace('Error("diag sample count")','Error(\\"diag sample count\\")'))
    h.write_text(s);return h
bench.harness=harness
if a.objects:
    (out/'build').mkdir(exist_ok=True)
    for obj in a.objects.glob('*.o'):shutil.copy(obj,out/'build'/obj.name)
binary=bench.build(out)
base=subprocess.check_output(['git','show',bench.BASE+':'+bench.SOURCE],cwd=bench.ROOT,text=True)
sources={'base':base,'fast':(bench.ROOT/bench.SOURCE).read_text()}
mock='globalThis.__diagN=0;globalThis.__diagOpen=false;pocket.derby={_diag:(op,n)=>{if(op===0){if(__diagOpen||__diagN>=120)throw Error("diag start");__diagOpen=true}else{if(!__diagOpen||n!==__diagN)throw Error("diag end");__diagOpen=false;++__diagN}}};\n'
digests=[];rows=[]
for mode,src in sources.items():
    for wrapped in (False,True):
        name=mode+('-diagnostic' if wrapped else '-ordinary');source=out/(name+'.js')
        source.write_text(mock+src+(diag.WORKLOAD if wrapped else ''))
        if wrapped:os.environ['CURL_DIAG']='1'
        else:os.environ.pop('CURL_DIAG',None)
        rows.append(bench.run(binary,source,out/name,case='title',verify=True))
for ext in ('panels','draws','states'):
    values=[(out/(mode+'-'+kind+'.'+ext)).read_bytes() for mode in sources for kind in ('ordinary','diagnostic')]
    assert all(x==values[0] for x in values),ext
    digests.append({'kind':ext,'bytes':len(values[0]),'sha256':hashlib.sha256(values[0]).hexdigest()})
report={'base':bench.BASE,'callbacks':160,'warm':40,'measured':120,'panels':161,'owner_boundary_cases':6,'comparisons':digests,'runs':rows}
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));print('BIGWAVE_DIAGNOSTIC PASS')
