"""Long natural-state replay and one zoom/tier/phase matrix; final panels compared directly."""
import argparse,hashlib,json,os,subprocess
import bench,common
R=common.R
OBSERVER=r'''
static int32_t zst[14];static unsigned zframes;
static JSValue zobserve(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;if(argc!=14)return JS_ThrowInternalError(c,"observer shape");
 for(int i=0;i<14;i++)if(JS_ToInt32(c,&zst[i],argv[i]))return JS_EXCEPTION;
 static FILE *f;if(!f)f=fopen(getenv("ZENITH_STATE"),"wb");if(!f)abort();
 if(fwrite(zst,sizeof zst,1,f)!=1)abort();
 zframes++;return JS_UNDEFINED;
}
'''
SCHEDULE=r'''
static int zstep(unsigned b){return mp_tick(false,b);}
static int zpress(unsigned b){return zstep(b)||zstep(0);}
static int zscene(void){for(unsigned n=0;zst[0]!=4&&n<7;n++)if(zpress(0x20))return 1;return zst[0]!=4;}
static int ztier(int tier){
 while(zst[2]>tier)if(zpress(0x40))return 1;
 while(zst[2]<tier)if(zpress(0x10))return 1;
 return 0;
}
static int zscenario(void){
 unsigned natural=getenv("ZENITH_NEGATIVE")?1:10;
 for(unsigned i=0;i<mp_loop*natural;i++)if(zstep(0))return 8;
 for(int tier=0;tier<3;tier++)for(int phase=0;phase<4;phase++){
  if(zscene()||ztier(tier)||zpress(0x20)||zpress(0x80))return 9;
  for(unsigned n=0;((zst[1]+1)&3)!=phase&&n<4;n++)if(zstep(0))return 10;
  if(zpress(0x4000))return 11;
  // Rebuild during the zoom transition, then again at full-screen zoom.
  unsigned away=tier<2?0x10:0x40,back=tier<2?0x40:0x10;
  if(zpress(away)||zpress(back))return 12;
  for(unsigned n=0;zst[3]!=44&&n<44;n++)if(zstep(0))return 13;
  if(zst[0]!=4||zst[3]!=44||zpress(away)||zpress(back))return 14;
  if(zpress(0x4000))return 15;
  for(unsigned n=0;zst[3]!=0&&n<44;n++)if(zstep(0))return 16;
  if(zst[0]!=4||zst[3]||zst[4])return 17;
  for(unsigned n=0;n<8;n++)if(zstep(0))return 18;
  if(getenv("ZENITH_NEGATIVE"))goto done;
 }
 done:printf("ZENITH_STATE frames=%u natural_loops=%u matrix_tiers=%u phases=%u\n",zframes,natural,getenv("ZENITH_NEGATIVE")?1:3,getenv("ZENITH_NEGATIVE")?1:4);
 return 0;
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');a=p.parse_args();out=R/('.cache/zenith-verify-m32' if a.m32 else '.cache/zenith-verify')
 old=bench.HELPER;at=old.index('static int mp_run(');old=old[:at]+SCHEDULE+'\n'+old[at:]
 start=old.index(' for(unsigned i=0;i<mp_loop;i++)if(mp_tick(false,0))');end=old.index(' size_t window_peak=',start)
 old=old[:start]+''' JS_RunGC(rt);size_t before=cur_bytes,prior_peak=peak_bytes;peak_bytes=cur_bytes;JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);
 if(zscenario())return 19;
'''+old[end:];bench.HELPER=old
 def transform(s):
  s=common.once(s,'static int mp_tick(bool record,unsigned buttons){',OBSERVER+'\nstatic int mp_tick(bool record,unsigned buttons){')
  return common.once(s,'    JSValue g=JS_GetGlobalObject(ctx);','    JSValue g=JS_GetGlobalObject(ctx);\n    JS_SetPropertyStr(ctx,g,"__zenObserve",JS_NewCFunction(ctx,zobserve,"__zenObserve",14));')
 apps,binary,h=bench.prepare(out,a.m32,transform)

 for app in apps.values():app.write_text(common.once(app.read_text(),'      ++t; ++tick;','      __zenObserve(scene,t,tier,zoom,target,live.length,pend.length,next?next.s:-1,next?next.h.length:0,built,vis,shown,held,tick);\n      ++t; ++tick;'))
 mutant=out/'mutant.js';mutant.write_text(common.once(apps['candidate'].read_text(),'zoom === 1 || ',''))
 def compare(case,other,negative=False):
  inputs={'baseline':apps['baseline'],'candidate':other};pipes={m:os.pipe() for m in inputs};children={};logs={};count=0;different=[];sha=hashlib.sha256()
  def read(fd):
   data=b''
   while len(data)<64800:
    part=os.read(fd,64800-len(data))
    if not part:break
    data+=part
   assert len(data) in (0,64800),'partial composed panel'
   return data
  try:
   for mode,app in inputs.items():
    r,w=pipes[mode];logs[mode]=(out/(case+'-'+mode+'.log')).open('w');env={**{k:v for k,v in os.environ.items() if not k.startswith(('MEGA_','ZENITH_'))},'MEGA_HEAP_LIMIT':'163840' if a.m32 else '524288','ZENITH_PANEL_FD':str(w),'ZENITH_STATE':str(out/(case+'-'+mode+'.state'))}
    if negative:env['ZENITH_NEGATIVE']='1'
    children[mode]=subprocess.Popen([str(binary),str(app)],cwd=R,env=env,stdout=logs[mode],stderr=subprocess.STDOUT,pass_fds=(w,));os.close(w)
   while True:
    x,y=[read(pipes[m][0]) for m in inputs]
    assert bool(x)==bool(y),'frame count differs'
    if not x:break
    if x!=y:different.append(count)
    if not negative:assert x==y,('panel mismatch',count)
    sha.update(x);count+=1
   for c in children.values():assert c.wait()==0,'app/heap/path failed'
  finally:
   for c in children.values():
    if c.poll() is None:c.terminate();c.wait()
   for r,w in pipes.values():os.close(r)
   for f in logs.values():f.close()
  state={m:(out/(case+'-'+m+'.state')).read_bytes() for m in inputs};assert state['baseline']==state['candidate'],'long app state/loader/input differs'
  rows={}
  for m in inputs:
   log=(out/(case+'-'+m+'.log')).read_text();rows[m]=json.loads(next(l.removeprefix('ZENITH_RESULT ') for l in log.splitlines() if l.startswith('ZENITH_RESULT ')))
  assert rows['baseline']['log']==rows['candidate']['log']
  assert len(state['baseline'])==count*56
  if negative:assert different,'visibility mutant escaped'
  else:assert count>4500,count
  return dict(panels=count,all_panels_equal=not different,different_frames=different[:20],state_bytes=len(state['baseline']),state_sha256=hashlib.sha256(state['baseline']).hexdigest(),panel_sha256=sha.hexdigest(),logs_equal=True)
 report=dict(heap_limit=163840 if a.m32 else 524288,positive=compare('long',apps['candidate']),negative=compare('negative',mutant,True))
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
