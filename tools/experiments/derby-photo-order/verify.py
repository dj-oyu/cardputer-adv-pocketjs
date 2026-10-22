"""Compare every composed panel byte across the actual full game scenario."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
import run as protocol
R=protocol.R;run_derby=protocol.run_derby

def once(s,old,new):
 assert s.count(old)==1,old
 return s.replace(old,new)

def harness(out):
 s=(R/'tools/games/test_derby_host.c').read_text()
 s=once(s,'#include <time.h>','#include <time.h>\n#include <unistd.h>')
 s=once(s,'static void present(uint64_t now_us){',r'''
static void photo_panel_stream(void){
 static int fd=-2;if(fd==-2){const char *s=getenv("PHOTO_PANEL_FD");fd=s?atoi(s):-1;}
 if(fd<0)return;
 const unsigned char *p=(const unsigned char*)panel;size_t left=sizeof panel;
 while(left){ssize_t n=write(fd,p,left);REQ(n>0);p+=n;left-=(size_t)n;}
}
static void present(uint64_t now_us){''')
 s=once(s,'    else pocket_kasane_animations_presented(now_us);','    else pocket_kasane_animations_presented(now_us);\n    photo_panel_stream();')
 p=out/'verify.c';p.write_text(s);return p

def env(app,csv,case):
 e={**{k:v for k,v in os.environ.items() if not k.startswith(('DERBY_','PHOTO_'))},'DERBY_APP_DIR':str(app),'DERBY_TIER':'1','DERBY_CSV':str(csv),'DERBY_HEAP_LIMIT':'524288'}
 return e

def read_frame(fd):
 data=b''
 while len(data)<64800:
  chunk=os.read(fd,64800-len(data))
  if not chunk:break
  data+=chunk
 assert len(data) in (0,64800),'partial composed panel'
 return data

def compare(binary,apps,out,case):
 pipes={m:os.pipe() for m in apps};children={};logs={};count=0;sha=hashlib.sha256()
 try:
  for mode,app in apps.items():
   logs[mode]=(out/(mode+'-'+case+'.log')).open('w')
   r,w=pipes[mode];e=env(app,out/(mode+'-'+case+'.csv'),case);e['PHOTO_PANEL_FD']=str(w)
   children[mode]=subprocess.Popen([str(binary)],cwd=R,env=e,stdout=logs[mode],stderr=subprocess.STDOUT,pass_fds=(w,));os.close(w)
  while True:
   a,b=[read_frame(pipes[m][0]) for m in apps]
   assert a==b,f'{case}: final panel differs at present {count}'
   if not a:break
   count+=1;sha.update(a)
  for p in children.values():assert p.wait()==0,case
 finally:
  for p in children.values():
   if p.poll() is None:p.terminate();p.wait()
  for r,_ in pipes.values():os.close(r)
  for f in logs.values():f.close()
 assert count==14564
 rows={}
 for mode in apps:
  log=(out/(mode+'-'+case+'.log')).read_text();assert 'DERBY_HOST PASS' in log
  rows[mode]={'csv_sha256':hashlib.sha256((out/(mode+'-'+case+'.csv')).read_bytes()).hexdigest(),'events':[l for l in log.splitlines() if 'DERBY ' in l and any(w in l for w in ('SCENE ','FINISH ','PICK ','RESULT ','SAVE ','DEMO '))]}
 assert rows['baseline']==rows['candidate'],case
 return {'case':case,'all_panel_bytes_equal':True,'presents':count,'panel_bytes':64800,'panel_sha256':sha.hexdigest(),'rows':rows}

def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');a=p.parse_args()
 out=R/('.cache/photo-order-verify-m32' if a.m32 else '.cache/photo-order-verify');out.mkdir(parents=True,exist_ok=True)
 apps=protocol.prepare(out);inputs=protocol.manifest(apps)
 binary=run_derby.build(['-O2']+(['-m32','-malign-double'] if a.m32 else []),out/'build',harness=harness(out))
 rows=[compare(binary,apps,out,case) for case in ('normal',)]
 assert protocol.manifest(apps)==inputs
 report={'base':protocol.BASE,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'inputs':inputs,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256((out/'verify.c').read_bytes()).hexdigest(),'rows':rows}
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));print('PHOTO_ORDER_VERIFY PASS')
if __name__=='__main__':main()
