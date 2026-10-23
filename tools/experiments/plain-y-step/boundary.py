"""Independent whole-walk pixel oracle, outside all timing paths."""
from pathlib import Path
import argparse,json,subprocess
import renderer
R=renderer.ROOT
p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');p.add_argument('--m32',action='store_true');p.add_argument('--negative',action='store_true');p.add_argument('--positive',action='store_true');a=p.parse_args()
out=R/'.cache/plain-y-step-boundary';out.mkdir(parents=True,exist_ok=True)
src=out/'renderer.c';code=renderer.source()
if a.negative:
 assert code.count('    if(!height)return;')==1
 code=code.replace('    if(!height)return;','',1)
src.write_text(code)
exe=out/('boundary'+('-sanitize' if a.sanitize else '')+('-m32' if a.m32 else '')+('-negative' if a.negative else ''))
flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'] if a.sanitize else ['-O2']
subprocess.run(['gcc',*flags,*(['-m32','-malign-double'] if a.m32 else []),'-std=c11','-I',str(R/'main/ui/kasane'),str(Path(__file__).with_suffix('.c')),str(src),'-lm','-o',str(exe)],check=True)
r=subprocess.run([str(exe)]+(["--positive"] if a.positive else []),text=True,capture_output=True)
print(r.stdout+r.stderr,end='');(out/(exe.name+'.log')).write_text(r.stdout+r.stderr)
if a.negative:
 assert r.returncode==1 and 'pixel mismatch mode=1 ext=1 y=0 h=0' in r.stderr
 print('PLAIN_NEGATIVE PASS zero-height mutation detected')
else:r.check_returncode()
