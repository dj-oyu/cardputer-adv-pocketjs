"""Compare every composed panel byte, plus result rebuild/cache boundaries."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys
import run as protocol
R=protocol.R;run_derby=protocol.run_derby

def once(s,old,new):
 assert s.count(old)==1,old
 return s.replace(old,new)

OBSERVER=r'''
(function(){
 const rp=V.replace,pa=V.patch;
 function wrap(tx){
  const p=Object.getPrototypeOf(tx);
  if(p.__resultObserver)return;p.__resultObserver=1;
  const text=p.text;
  p.text=function(sp){
   const r=text.call(this,sp);r.__resultText=sp.text;
   const q=Object.getPrototypeOf(r);
   if(!q.__resultObserver){q.__resultObserver=1;const set=q.setText;
    q.setText=function(t,s){const v=set.call(this,t,s);this.__resultText=s;return v};}
   return r;
  };
 }
 V.replace=function(fn){return rp.call(V,tx=>{wrap(tx);return fn(tx)})};
 V.patch=function(fn){return pa.call(V,tx=>{wrap(tx);return fn(tx)})};
 const frame=globalThis.frame;let injected=false;
 globalThis.frame=function(b){
  if(scene==='res'&&t===10&&!injected){injected=true;pts+=1234;if(globalThis.__resultRebuild)need=1;}
  const v=frame(b);
  if(scene==='res'&&!need&&R.t[3].__resultText!=='PTS '+pts+'   1 NEXT RACE   R REPLAY')
   throw Error('result HUD stale native points');
  return v;
 };
})();
'''

def harness(out):
 s=(R/'tools/games/test_derby_host.c').read_text()
 s=once(s,'#include <time.h>','#include <time.h>\n#include <unistd.h>')
 s=once(s,'static void present(uint64_t now_us){',r'''
static void result_panel_stream(void){
 static int fd=-2;if(fd==-2){const char *s=getenv("RESULT_PANEL_FD");fd=s?atoi(s):-1;}
 if(fd<0)return;
 const unsigned char *p=(const unsigned char*)panel;size_t left=sizeof panel;
 while(left){ssize_t n=write(fd,p,left);REQ(n>0);p+=n;left-=(size_t)n;}
}
static void present(uint64_t now_us){''')
 s=once(s,'    else pocket_kasane_animations_presented(now_us);','    else pocket_kasane_animations_presented(now_us);\n    result_panel_stream();')
 s=once(s,'    if(normal_race){',r'''
    if(getenv("RESULT_LIFECYCLE")){
        tap("1");run_until("res",4000);
        for(unsigned i=0;i<30;i++)frame(0);
        REQ(!exceptions&&!bad_present&&!framefails);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("RESULT_LIFECYCLE PASS");return 0;
    }
    if(normal_race){''')
 p=out/'verify.c';p.write_text(s);return p

def env(app,csv,case):
 e={**{k:v for k,v in os.environ.items() if not k.startswith(('DERBY_','RESULT_'))},'DERBY_APP_DIR':str(app),'DERBY_TIER':'1','DERBY_CSV':str(csv),'DERBY_HEAP_LIMIT':'524288'}
 if case!='normal':e.update(RESULT_LIFECYCLE='1',DERBY_JS=('globalThis.__resultRebuild='+('true' if case=='rebuild' else 'false')+';'+OBSERVER))
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
   r,w=pipes[mode];e=env(app,out/(mode+'-'+case+'.csv'),case);e['RESULT_PANEL_FD']=str(w)
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
 assert count==14564 if case=='normal' else count>100
 rows={}
 for mode in apps:
  log=(out/(mode+'-'+case+'.log')).read_text();assert ('DERBY_HOST PASS' if case=='normal' else 'RESULT_LIFECYCLE PASS') in log
  rows[mode]={'csv_sha256':hashlib.sha256((out/(mode+'-'+case+'.csv')).read_bytes()).hexdigest(),'events':[l for l in log.splitlines() if 'DERBY ' in l and any(w in l for w in ('SCENE ','FINISH ','PICK ','RESULT ','SAVE ','DEMO '))]}
 assert rows['baseline']==rows['candidate'],case
 return {'case':case,'all_panel_bytes_equal':True,'presents':count,'panel_bytes':64800,'panel_sha256':sha.hexdigest(),'rows':rows}

def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');a=p.parse_args()
 out=R/('.cache/result-hud-verify-m32' if a.m32 else '.cache/result-hud-verify');out.mkdir(parents=True,exist_ok=True)
 apps=protocol.prepare(out);inputs=protocol.manifest(apps)
 binary=run_derby.build(['-O2']+(['-m32','-malign-double'] if a.m32 else []),out/'build',harness=harness(out))
 rows=[compare(binary,apps,out,case) for case in ('normal','rebuild')]
 # One invalidation-contract negative: changing points without rebuilding.
 for mode,app in apps.items():
  proc=subprocess.run([str(binary)],cwd=R,env=env(app,out/(mode+'-negative.csv'),'negative'),capture_output=True,text=True)
  log=proc.stdout+proc.stderr;(out/(mode+'-negative.log')).write_text(log)
  assert (proc.returncode==0 and 'RESULT_LIFECYCLE PASS' in log) if mode=='baseline' else (proc.returncode!=0 and 'result HUD stale native points' in log),mode
 assert protocol.manifest(apps)==inputs
 report={'host_flags':'-O2 -DQJS_DISABLE_DUMPS'+(' -m32 -malign-double' if a.m32 else ''),'host_heap_limit':524288,'product_heap_fit_verified':False,'base':protocol.BASE,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'inputs':inputs,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256((out/'verify.c').read_bytes()).hexdigest(),'rows':rows,'negative_contract_detected':True}
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));print('RESULT_HUD_VERIFY PASS')
if __name__=='__main__':main()
