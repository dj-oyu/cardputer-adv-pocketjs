"""Independent reproduction of sol's attract scan candidate on actual DERBY.
One common O2 native binary; only lowered app input differs between A and B.
"""
from pathlib import Path
import os,sys,subprocess,hashlib,json,datetime,argparse,shutil
R=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(R/'tools/games'))
import run_derby
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'

def prepare(out):
 changed=set(subprocess.check_output(['git','diff','--name-only',BASE,'--','apps/derby'],cwd=R,text=True).splitlines());assert changed <= {'apps/derby/derby_play.js'},changed
 out.mkdir(parents=True,exist_ok=True)
 baseline=out/'baseline-source'
 if baseline.exists():shutil.rmtree(baseline)
 shutil.copytree(R/'apps/derby',baseline)
 (baseline/'derby_play.js').write_bytes(subprocess.check_output(['git','show',BASE+':apps/derby/derby_play.js'],cwd=R))
 return {label:run_derby.lower(src,out/label/'apps/derby') for label,src in [('baseline',baseline),('candidate',R/'apps/derby')]}

def manifest(apps):
 return {mode:{str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(app.rglob('*')) if p.is_file()} for mode,app in apps.items()}

def main():
 p=argparse.ArgumentParser();p.add_argument('--build-only',action='store_true');p.add_argument('--m32',action='store_true');a=p.parse_args()
 out=R/('.cache/attract-deep-m32' if a.m32 else '.cache/attract-deep')
 apps=prepare(out);inputs=manifest(apps)
 binary=run_derby.build(['-O2']+(['-m32','-malign-double'] if a.m32 else []),out/'build')
 print('COMMON_BINARY',str(binary),hashlib.sha256(binary.read_bytes()).hexdigest(),flush=True)
 if a.build_only:return
 session=out/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ');session.mkdir()
 results={}
 for mode,app in apps.items():
  csv=session/(mode+'.csv');log=session/(mode+'.log')
  env={**{k:v for k,v in os.environ.items() if not k.startswith(('DERBY_','ATTRACT_'))},'DERBY_APP_DIR':str(app),'DERBY_TIER':'1','DERBY_CSV':str(csv)}
  with log.open('w') as f:subprocess.run([str(binary)],cwd=R,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
  text=log.read_text();assert 'DERBY_HOST PASS' in text
  results[mode]={'csv_sha256':hashlib.sha256(csv.read_bytes()).hexdigest(),'pixels':next(line for line in text.splitlines() if line.startswith('pixels hash ')),'frames':next(line for line in text.splitlines() if line.startswith('frames '))}
  print(mode,results[mode],flush=True)
 assert results['baseline']==results['candidate']
 assert manifest(apps)==inputs,'Input snapshots changed during trial'
 (session/'result.json').write_text(json.dumps({'inputs':inputs,'base':BASE,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'play_sha256':hashlib.sha256((R/'apps/derby/derby_play.js').read_bytes()).hexdigest(),'results':results},indent=2)+'\n')
 print('PASS actual MID scenario/input/demo/replay/next-race/Back and all frame pixels/CSV identical',session)
if __name__=='__main__':main()
