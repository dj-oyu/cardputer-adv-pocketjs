"""Read public QuickJS final-bytecode dumps outside all timing runs."""
from pathlib import Path
import hashlib,json,os,re,subprocess
import common
R=common.R;out=R/'.cache/bigwave-inputs-compile';apps=common.inputs(out)
s=(R/'.cache/bigwave-inputs/perf.c').read_text()
s=common.once(s,'    bool ok=eval(src,n,"big_wave.js");','    JS_SetDumpFlags(rt,JS_DUMP_BYTECODE_FINAL);\n    bool ok=eval(src,n,"big_wave.js");\n    JS_SetDumpFlags(rt,0);')
h=out/'compile.c';h.write_text(s);binary=common.build(out/'build',h)
rows={}
for mode,app in apps.items():
 env={k:v for k,v in os.environ.items() if not k.startswith(('BW_','BIGWAVE_INPUTS_'))};env.update(BIGWAVE_INPUTS_CASE='pause',BW_HEAP_LIMIT='524288')
 text=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True);(out/(mode+'.dump')).write_text(text)
 rows[mode]=[{'function':m[0],'stack_size':int(m[1]),'bytecode_len':int(m[2])} for m in re.findall(r'function: ([^\n]*)\n(?:(?!function: )[\s\S])*?  stack_size: (\d+)\n  byte_code_len: (\d+)',text)]
 assert rows[mode] and any(x['function']=='draws' for x in rows[mode])
assert len(rows['baseline'])==len(rows['candidate'])
changes=[{'baseline':a,'candidate':b} for a,b in zip(rows['baseline'],rows['candidate']) if a!=b]
report={'rows':rows,'changes':changes,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'scope':'Pinned engine public final-bytecode dump, outside timings. Static operand stack slots, not measured device/native C stack.'}
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'changes':changes,'max_stack':{m:max(x['stack_size'] for x in a) for m,a in rows.items()}},indent=2))
