"""Real LCD CATCH game/storage replay and one compact invalidation boundary table."""
import argparse,hashlib,json,os,subprocess
import common
R=common.R
HELPER=r'''
static void boundary(void){
 snprintf(kv,sizeof kv,"[-1,-2147483648]");kv_has=true;open_session();frames(30);
 CHECK(num("lcdCatch.mode")==1,"saved negative HI after lamp");
 tap("s");tap("s");
 const int values[]={0,9,10,99,100,999,0};
 for(unsigned i=0;i<sizeof values/sizeof values[0];i++){
  char s[200];snprintf(s,sizeof s,"Object.assign(lcdCatch,{mode:2,paused:true,score:%d})",values[i]);eval(s,strlen(s),"boundary.js");frame();frame();
 }
 eval("lcdCatch.mode=0;lcdCatch.wait=100",strlen("lcdCatch.mode=0;lcdCatch.wait=100"),"lamp.js");frame();
 eval("lcdCatch.mode=1;lcdCatch.hi=[-1,-1]",strlen("lcdCatch.mode=1;lcdCatch.hi=[-1,-1]"),"hi.js");frame();tap("s");
 eval("globalThis.__aborts=0;globalThis.__patch=pocket.kasane.patch;pocket.kasane.patch=function(f){return __patch(function(tx){f(tx);__aborts++;throw Error('intentional abort')})};lcdCatch.mode=2;lcdCatch.score=100",strlen("globalThis.__aborts=0;globalThis.__patch=pocket.kasane.patch;pocket.kasane.patch=function(f){return __patch(function(tx){f(tx);__aborts++;throw Error('intentional abort')})};lcdCatch.mode=2;lcdCatch.score=100"),"abort.js");frame();
 CHECK(num("__aborts")==1,"one actual transaction aborted after callback");
 eval("pocket.kasane.patch=__patch",strlen("pocket.kasane.patch=__patch"),"restore.js");frame();frame();
 close_session();printf("LCD7_BOUNDARY %s\n",fails||exceptions||bad_present?"FAIL":"PASS");
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');a=p.parse_args();root=R/('.cache/lcd7-m32' if a.m32 else '.cache/lcd7');out=root/'verify';apps=common.inputs(out);s=(R/'tools/games/test_lcd_catch_host.c').read_text()
 s=common.once(s,'static void present(void){','static FILE *panels;\nstatic void present(void){')
 s=common.once(s,'    frames_run++;','    if(panels){fwrite(panel,1,sizeof panel,panels);}\n    frames_run++;')
 s=common.once(s,'int main(void){',HELPER+'\nint main(int argc,char **argv){')
 s=common.once(s,'FILE *f=fopen("apps/lcdcatch/lcd_catch.js","rb");','FILE *f=fopen(argc>1?argv[1]:"apps/lcdcatch/lcd_catch.js","rb");')
 s=common.once(s,'    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");','    panels=fopen(getenv("LCD7_PANELS"),"wb");\n    if(getenv("LCD7_BOUNDARY")){boundary();fclose(panels);return fails||exceptions||bad_present;}\n    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");')
 h=out/'verify.c';h.write_text(s);binary=common.build(root/'build',h,m32=a.m32);report={}
 for case in ('replay','boundary'):
  logs={};data={}
  for mode,app in apps.items():
   dest=out/(case+'-'+mode+'.panels');env={**{k:v for k,v in os.environ.items() if not k.startswith(('LCD_','LCD7_'))},'LCD7_PANELS':str(dest)}
   if case=='boundary':env['LCD7_BOUNDARY']='1'
   text=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True);(out/(case+'-'+mode+'.log')).write_text(text)
   logs[mode]='\n'.join(l for l in text.splitlines() if not l.startswith(('source ','guest heap ')))
   data[mode]=dest.read_bytes()
  assert logs['baseline']==logs['candidate'],(case,logs)
  assert data['baseline']==data['candidate'],case
  report[case]={'frames':len(data['baseline'])//64800,'panel_sha256':hashlib.sha256(data['baseline']).hexdigest(),'logs_equal':True}
  mutant=out/'rejected-sentinel.js';mutant.write_text(apps['candidate'].read_text().replace('let digits;','let digits = -1;').replace('digits = undefined','digits = -1'))
 dest=out/'rejected-sentinel.panels';env={**{k:v for k,v in os.environ.items() if not k.startswith(('LCD_','LCD7_'))},'LCD7_PANELS':str(dest),'LCD7_BOUNDARY':'1'}
 subprocess.run([str(binary),str(mutant)],cwd=R,env=env,check=True,stdout=subprocess.DEVNULL)
 assert dest.read_bytes()!=(out/'boundary-baseline.panels').read_bytes(),'negative sentinel mutant escaped'
 report['negative_sentinel_rejected']=True
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
