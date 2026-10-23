"""Separate observer binary counts the actual five assembly sites; no timing claim."""
from pathlib import Path
import hashlib,json,os,subprocess
import common
R=common.R;out=R/'.cache/bigwave-inputs-reached';apps=common.inputs(out)
base=subprocess.check_output(['git','show',common.BASE+':apps/bigwave/big_wave.js'],cwd=R,text=True).splitlines()
indices=[i for i,s in enumerate(base) if '.concat(w,' in s];assert len(indices)==5
for mode,app in apps.items():
 lines=app.read_text().splitlines()
 for site,i in enumerate(indices):
  s=lines[i]
  if site==3:
   prefix='    if (fb >= m0) ';assert s.startswith(prefix)
   lines[i]=prefix+'{__bwHit('+str(site)+');'+s[len(prefix):]+'}'
  else:lines[i]='    __bwHit('+str(site)+');'+s.lstrip()
 app.write_text('\n'.join(lines)+'\n')
s=(R/'.cache/bigwave-inputs/perf.c').read_text()
helper='''static uint32_t bw_hits[5];
static JSValue bw_hit(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;uint32_t i=5;if(argc)JS_ToUint32(c,&i,argv[0]);if(i<5)bw_hits[i]++;return JS_UNDEFINED;
}
'''
s=common.once(s,'static uint64_t bw_clock',helper+'static uint64_t bw_clock')
s=common.once(s,'    JS_SetPropertyStr(ctx,g,"__commit",JS_NewCFunction(ctx,js_commit,"commit",0));','    JS_SetPropertyStr(ctx,g,"__commit",JS_NewCFunction(ctx,js_commit,"commit",0));\n    JS_SetPropertyStr(ctx,g,"__bwHit",JS_NewCFunction(ctx,bw_hit,"hit",1));')
s=common.once(s,'    for(int i=0;i<120;i++)if(bw_tick(i))return 3;','    memset(bw_hits,0,sizeof bw_hits);\n    for(int i=0;i<120;i++)if(bw_tick(i))return 3;')
s=common.once(s,'    const size_t window_peak=peak_bytes,','    printf("BIGWAVE_HITS [%u,%u,%u,%u,%u]\\n",bw_hits[0],bw_hits[1],bw_hits[2],bw_hits[3],bw_hits[4]);\n    const size_t window_peak=peak_bytes,')
h=out/'reached.c';h.write_text(s);binary=common.build(out/'build',h)
rows=[]
for case in ('title','ride','pause'):
 checked={}
 for mode,app in apps.items():
  env={k:v for k,v in os.environ.items() if not k.startswith(('BW_','BIGWAVE_INPUTS_'))};env.update(BIGWAVE_INPUTS_CASE=case,BW_HEAP_LIMIT='524288')
  text=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True)
  hits=json.loads(next(l.removeprefix('BIGWAVE_HITS ') for l in text.splitlines() if l.startswith('BIGWAVE_HITS ')))
  d=json.loads(next(l.removeprefix('BIGWAVE_INPUTS_RESULT ') for l in text.splitlines() if l.startswith('BIGWAVE_INPUTS_RESULT ')))
  checked[mode]={'hits':hits,'hash':d['hash'],'input_hash':d['input_hash'],'state':d['state']}
 assert checked['baseline']==checked['candidate']
 rows.append({'case':case,**checked})
report={'rows':rows,'scope':'Counts actual assembly sites in a separate observer binary, not timings. Original five concat sites absent in uninstrumented candidate; audio concat unchanged.','binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(h.read_bytes()).hexdigest()}
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
