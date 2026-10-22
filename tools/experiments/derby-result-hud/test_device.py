"""Execute the exact device diagnostic JS and native input counters on host.
This checks fixed workload/120-frame completion/path reach and output equality;
it does not emulate the product guest allocator, scheduler or LCD.
"""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys
R=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(R/'tools/games'))
import run_derby
import prepare_device as device
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'

def once(s,old,new):
 assert s.count(old)==1,old
 return s.replace(old,new)

HELPER=r'''
unsigned derby_diag_down,derby_diag_queries;
static unsigned diag_frames,diag_d0,diag_q0,diag_down,diag_queries;
static size_t diag_a0,diag_b0,diag_requests,diag_bytes;
static JSValue diag_mark(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
 (void)self;int32_t op=0;REQ(argc>0&&!JS_ToInt32(c,&op,argv[0]));
 if(op==2)return JS_UNDEFINED;
 if(op==0){result_diag_measuring=true;diag_d0=derby_diag_down;diag_q0=derby_diag_queries;diag_a0=n_alloc;diag_b0=b_alloc;}
 else{REQ(op==1&&diag_frames<120);diag_down+=derby_diag_down-diag_d0;diag_queries+=derby_diag_queries-diag_q0;diag_requests+=n_alloc-diag_a0;diag_bytes+=b_alloc-diag_b0;++diag_frames;}
 return JS_UNDEFINED;
}
'''

def owner_boundary(out):
 # Compile the exact generated owner. One table covers deadline and completion
 # rejection at the boundary; it does not emulate the production owner loop.
 pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
static bool running,done,valid,pending;
static int64_t fake_now;
static unsigned clocks;
static const char *status;
static const char blob[2]={0};
static const char *derby_watch_start=blob,*derby_watch_end=blob+2;
static int64_t esp_timer_get_time(void){++clocks;return fake_now;}
bool derby_diag_finished(void){return done;}
bool derby_diag_valid(void){return valid;}
void derby_diag_owner_tick(unsigned us,bool c){(void)us;(void)c;}
void derby_diag_finish(const char *s){status=s;}
static bool pocket_kasane_needs_present(void){return pending;}
static void begin_run(const char *a,const char *b,size_t c,const char *d,size_t e){(void)a;(void)b;(void)c;(void)d;(void)e;}
static void end_run(int e){(void)e;running=false;}
'''
 post=r'''
int main(void){
 const struct {int64_t now;bool done,valid,pending;const char *want;} cases[]={
  {23999999,true,true,false,"complete"},
  {24000000,true,true,false,"timeout"},
  {24000001,true,true,false,"timeout"},
  {24000000,false,true,false,"timeout"},
  {23999999,true,false,false,"input_mismatch"},
  {23999999,true,true,true,"present_pending"},
 };
 for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
  running=true;diag_owner_ran=true;diag_owner_active=true;diag_owner_deadline=24000000;
  fake_now=cases[i].now;done=cases[i].done;valid=cases[i].valid;pending=cases[i].pending;clocks=0;status=NULL;
  diag_owner_step();
  assert(clocks==1&&status&&!strcmp(status,cases[i].want)&&!running&&!diag_owner_active);
 }
 puts("RESULT_HUD_OWNER_BOUNDARY PASS");
}
'''
 source=out/'owner-boundary.c'
 owner=(device.TEMPLATE/'owner.inc').read_text().replace('@SOURCE@','test').replace('@WORKLOAD@',device.WORKLOAD)
 source.write_text(pre+owner+post)
 binary=out/'owner-boundary'
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)

def main():
 out=R/'.cache/result-hud-device-host';out.mkdir(exist_ok=True)
 owner_boundary(out)
 s=(R/'tools/games/test_derby_host.c').read_text()
 s=once(s,'static const char PRELUDE[]=',HELPER+'\nstatic const char PRELUDE[]=')
 s=once(s,'static void present(uint64_t now_us){','static uint64_t result_diag_panel_hash=1469598103934665603ull;\nstatic bool result_diag_measuring;\nstatic void present(uint64_t now_us){')
 s=once(s,'    else pocket_kasane_animations_presented(now_us);','    else pocket_kasane_animations_presented(now_us);\n    if(result_diag_measuring)for(size_t i=0;i<sizeof panel;i++){result_diag_panel_hash^=((unsigned char*)panel)[i];result_diag_panel_hash*=1099511628211ull;}')
 s=once(s,'{"__log",js_log,1}','{"__attractDiag",diag_mark,2},{"__log",js_log,1}')
 pre='pocket.derby._diag=__attractDiag;pocket.random.seed=()=>0x2545f491;pocket.storage.get=()=>Promise.resolve({value:null,revision:0});pocket.storage.set=()=>Promise.resolve()'
 s=once(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");','    eval(PRELUDE,strlen(PRELUDE),"prelude.js");\n    {const char *s='+json.dumps(pre)+';REQ(eval(s,strlen(s),"diag-pre.js"));}')
 s=once(s,'    REQ(loaded_seen==1);',r'''
    for(unsigned i=0;i<250&&diag_frames<120;i++)frame(0);
    REQ(diag_frames==120&&!exceptions&&!bad_present&&!framefails);
    printf("RESULT_HUD_DEVICE_HOST {\"frames\":%u,\"pixels\":\"%016llx\",\"panel_hash\":\"%016llx\",\"down\":%u,\"queries\":%u,\"requests\":%zu,\"charged_bytes\":%zu}\n",diag_frames,(unsigned long long)pixel_hash,(unsigned long long)result_diag_panel_hash,diag_down,diag_queries,diag_requests,diag_bytes);
    pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    return 0;
    REQ(loaded_seen==1);''')
 h=out/'harness.c';h.write_text(s)
 native=out/'pocket_input.c';native.write_text(device.input_source((R/'main/pocket/pocket_input.c').read_text()))
 run_derby.EXTRA=[str(native) if x=='main/pocket/pocket_input.c' else x for x in run_derby.EXTRA]
 (out/'build').mkdir(exist_ok=True)
 for n in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
  obj=R/'.cache/attract-deep/build'/(n+'.o')
  if obj.exists():shutil.copy2(obj,out/'build'/obj.name)
 binary=run_derby.build(['-O2'],out/'build',harness=h)
 rows={};workload_hashes={}
 for mode in ('baseline','candidate'):
  src=out/mode/'source'
  if src.exists():shutil.rmtree(src)
  shutil.copytree(R/'apps/derby',src)
  p=src/'derby_scene.js'
  if mode=='baseline':p.write_bytes(subprocess.check_output(['git','show',BASE+':apps/derby/derby_scene.js'],cwd=R))
  p=src/'derby_play.js'
  workload=device.workload()
  workload_hashes[mode]=hashlib.sha256(workload.encode()).hexdigest()
  p.write_text(p.read_text()+'\n'+workload)
  p=src/'derby_watch.js';p.write_text(once(p.read_text(),'const OV = [.5,','const OV = [1,'))
  app=run_derby.lower(src,out/mode/'app')
  csv=out/mode/'frames.csv'
  env={**{k:v for k,v in os.environ.items() if not k.startswith(('DERBY_','RESULT_HUD_'))},'DERBY_APP_DIR':str(app),'DERBY_TIER':'2','DERBY_CSV':str(csv),'DERBY_HEAP_LIMIT':'524288'}
  proc=subprocess.run([str(binary)],cwd=R,env=env,capture_output=True,text=True)
  (out/mode/'run.log').write_text(proc.stdout+proc.stderr)
  assert proc.returncode==0,proc.stdout[-2000:]+proc.stderr[-2000:]
  row=json.loads(next(l.removeprefix('RESULT_HUD_DEVICE_HOST ') for l in proc.stdout.splitlines() if l.startswith('RESULT_HUD_DEVICE_HOST ')))
  row['csv_sha256']=hashlib.sha256(csv.read_bytes()).hexdigest();rows[mode]=row
 a,b=rows.values()
 assert all(a[k]==b[k] for k in ('frames','pixels','panel_hash','csv_sha256'))
 assert (a['down'],b['down'],a['queries'],b['queries'])==(120,120,1440,1440),rows
 # Fixed seed1/no-storage diagnostic result differs from the seed3 host race.
 assert a['requests']-b['requests']==32*120 and a['charged_bytes']-b['charged_bytes']==1672*120,rows
 report={'workload':device.WORKLOAD,'base':BASE,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'workload_sha256':workload_hashes,'rows':rows}
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));print('RESULT_HUD_DEVICE_WORKLOAD PASS')
if __name__=='__main__':main()
