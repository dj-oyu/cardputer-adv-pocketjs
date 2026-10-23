"""Reuse the full key/bot game scenario; compare every composed panel and native call."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
import common
R=common.R;once=common.once
STREAM=r'''
static void bw_panel_stream(void){
 static int fd=-2;if(fd==-2){const char *s=getenv("BW_PANEL_FD");fd=s?atoi(s):-1;}
 if(fd<0)return;
 const unsigned char *p=(const unsigned char*)panel;size_t left=sizeof panel;
 while(left){ssize_t n=write(fd,p,left);if(n<=0)abort();left-=(size_t)n;p+=n;}
}
static void bw_trace(unsigned kind,const rec_draw *d){
 static FILE *f;static bool opened;
 if(!opened){opened=true;const char *s=getenv("BW_NATIVE_TRACE");if(s)f=fopen(s,"wb");if(s&&!f)abort();}
 if(!f)return;
 uint32_t row[4+KSN_PROC_INPUTS]={kind,frame_no,0,0};
 if(d){row[2]=(uint32_t)d->h;row[3]=d->n;memcpy(row+4,d->in,sizeof d->in);}
 if(fwrite(row,sizeof row,1,f)!=1)abort();
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');p.add_argument('--sanitize',action='store_true');a=p.parse_args();assert not(a.m32 and a.sanitize)
 out=R/('.cache/bigwave-rock-verify'+('-m32' if a.m32 else '-asan' if a.sanitize else ''));apps=common.inputs(out)
 s=(R/'tools/games/test_big_wave_host.c').read_text();s=once(s,'#include <math.h>','#include <math.h>\n#include <unistd.h>')
 # rec_draw is declared later than present, so split the two helpers.
 panel,trace=STREAM.split('static void bw_trace',1)
 s=once(s,'static void present(void){',panel+'\nstatic void present(void){')
 s=once(s,'    else pocket_kasane_animations_presented(now);','    else pocket_kasane_animations_presented(now);\n    bw_panel_stream();')
 s=once(s,'static JSValue js_begin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){','static void bw_trace'+trace+'\nstatic JSValue js_begin(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){')
 s=once(s,'JS_ToFloat64(c,&rec_bg,argv[0]);rec_n=0;','JS_ToFloat64(c,&rec_bg,argv[0]);rec_n=0;bw_trace(1,NULL);')
 s=once(s,'    return JS_UNDEFINED;\n}\nstatic JSValue js_commit','    bw_trace(2,d);\n    return JS_UNDEFINED;\n}\nstatic JSValue js_commit')
 s=once(s,'rec_committed=true;return JS_UNDEFINED;','rec_committed=true;bw_trace(3,NULL);return JS_UNDEFINED;')
 h=out/'verify.c';h.write_text(s);binary=common.build(out/'build',h,a.m32,a.sanitize)
 pipes={m:os.pipe() for m in apps};children={};logs={};count=0;sha=hashlib.sha256()
 def read(fd):
  data=b''
  while len(data)<64800:
   chunk=os.read(fd,64800-len(data))
   if not chunk:break
   data+=chunk
  assert len(data) in (0,64800),'partial panel'
  return data
 try:
  for mode,app in apps.items():
   log=out/(mode+'.log');logs[mode]=log.open('w');r,w=pipes[mode]
   env={**{k:v for k,v in os.environ.items() if not k.startswith(('BW_','BIGWAVE_ROCK_'))},'BW_HEAP_LIMIT':'163840' if a.m32 else '524288','BW_PANEL_FD':str(w),'BW_NATIVE_TRACE':str(out/(mode+'.native')),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
   children[mode]=subprocess.Popen([str(binary),str(app)],cwd=R,env=env,stdout=logs[mode],stderr=subprocess.STDOUT,pass_fds=(w,));os.close(w)
  while True:
   a0,b0=[read(pipes[m][0]) for m in apps];assert a0==b0,f'panel mismatch at{count}'
   if not a0:break
   sha.update(a0);count+=1
  for p0 in children.values():assert p0.wait()==0,'full native game scenario failed'
 finally:
  for p0 in children.values():
   if p0.poll() is None:p0.terminate();p0.wait()
  for r,_ in pipes.values():os.close(r)
  for f in logs.values():f.close()
 rows={}
 for mode in apps:
  text=(out/(mode+'.log')).read_text();assert 'BIGWAVE_HOST PASS' in text
  rows[mode]={'native_sha256':hashlib.sha256((out/(mode+'.native')).read_bytes()).hexdigest(),'native_bytes':(out/(mode+'.native')).stat().st_size,'semantic_events':[l for l in text.splitlines() if '] BIGWAVE ' in l],'checks':[l for l in text.splitlines() if l.startswith(('PASS ','FAIL '))]}
 assert rows['baseline']==rows['candidate'],'native calls or state transitions differ'
 assert (out/'baseline.native').read_bytes()==(out/'candidate.native').read_bytes()
 assert count>5000,count
 report={'base':common.BASE,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(h.read_bytes()).hexdigest(),'panels':count,'panel_bytes':64800,'all_panel_bytes_equal':True,'panel_sha256':sha.hexdigest(),'rows':rows}
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));print('BIGWAVE_ROCK_VERIFY PASS')
if __name__=='__main__':main()
