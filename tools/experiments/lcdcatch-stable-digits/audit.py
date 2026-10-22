"""Separate untimed loop-reach audit; never used for speed numbers."""
import json,os,subprocess
import bench,common
R=common.R

def main():
 out=R/'.cache/lcd7-audit';apps=common.inputs(out)
 loop='for (let d = 0, p = 100; d < 3; d++, p /= 10) {'
 for p in apps.values():p.write_text('globalThis.__digitLoops=0;\n'+common.once(p.read_text(),loop,loop+'if(d===0)globalThis.__digitLoops++;'))
 original=common.inputs;common.inputs=lambda _:apps
 old=bench.HELPER
 bench.HELPER=common.once(old,' for(int i=0;i<120;i++)if(lc_tick(i))return 7;',' const int loops=(int)num("__digitLoops");\n for(int i=0;i<120;i++)if(lc_tick(i))return 7;\n printf("LCD7_AUDIT %d\\n",(int)num("__digitLoops")-loops);')
 apps,binary,h=bench.prepare(out);common.inputs=original;result={}
 for case in ('natural','input','title'):
  result[case]={};hashes={}
  for mode,app in apps.items():
   env={**{k:v for k,v in os.environ.items() if not k.startswith(('LCD_','LCD7_'))},'LCD7_CASE':case}
   log=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True)
   result[case][mode]=int(next(l.split()[1] for l in log.splitlines() if l.startswith('LCD7_AUDIT ')))
   hashes[mode]=json.loads(next(l.removeprefix('LCD7_RESULT ') for l in log.splitlines() if l.startswith('LCD7_RESULT ')))['hash']
  assert hashes['baseline']==hashes['candidate'];assert result[case]['baseline']==120 and result[case]['candidate']==0,result
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
