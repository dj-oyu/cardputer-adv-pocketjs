#!/usr/bin/env python3
"""Run the exact diagnostic JS workload through the real host app/renderer."""
import hashlib,json,os,re,shutil,subprocess,tempfile
from pathlib import Path
import run_derby,run_derby_view_norm as norm
ROOT=norm.ROOT
OLD='1e68b268a4b91ec54e88f408118de3e39b8b6bc5'
GOLDEN={
    'oval':('2ecc984362eaf8ce','497.350884362','39a389cdd98565136fb151b3cba655195f282b2b8236471bfb84e449f893fd5c'),
    'straight':('a2bf4bb2bd6ceb54','497.788163384','da62ea7542c8217d4a325a85c0d8a047bce727cc4ad43d6465de3dc749214a29'),
    'pad':('8ca8539d58f721a5','0.000000000','11c34abe3b8abb6148c2f802fcf635b9ec5905695e3920e0ff4723d94cb3a0d4')}
# Test-only step budget. It rejects the previous unbounded callback, without
# changing the product watchdog or asserting that host time predicts device time.
BEFORE='const diagCheckedStep=step;let diagCheckSteps=0,diagCheckMax=0,diagCheckTurns=0;step=(...a)=>{if(++diagCheckSteps>8)throw Error("DIAG_STEP_LIMIT");diagCheckMax=mx(diagCheckMax,diagCheckSteps);return diagCheckedStep(...a);};'
AFTER='const diagCheckedFrame=globalThis.frame;globalThis.frame=b=>{diagCheckSteps=0;++diagCheckTurns;diagCheckedFrame(b);if(diagFrames===130)log("DIAG_STEP_BOUND max="+diagCheckMax+" callbacks="+diagCheckTurns);};'

def test_owner(template):
    # Compile the actual patched owner callback against a small host clock stub.
    prefix='''#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
static bool running,done,pending;static int64_t now;static const char *status;
static int64_t esp_timer_get_time(void){return now;}
bool derby_diag_finished(void){return done;}
static bool pocket_kasane_needs_present(void){return pending;}
void derby_diag_finish(const char *s){status=s;}
#define begin_run(...) ((void)diag_prelude)
#define end_run(...) (running=false)
#define printf(...) ((void)0)
'''
    suffix='''
#undef printf
int main(void){
    const struct {bool run,done,pending;int64_t now;const char *want;} cases[]={
        {1,1,0,999,"complete"},{1,1,0,1000,"timeout"},
        {1,0,0,999,NULL},{1,0,0,1000,"timeout"},
        {0,1,0,1001,"app_error"},{1,1,1,999,"present_pending"}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
        running=cases[i].run;done=cases[i].done;pending=cases[i].pending;
        now=cases[i].now;status=NULL;diag_owner_ran=diag_owner_active=true;diag_owner_deadline=1000;
        diag_owner_step();
        if(cases[i].want?(!status||strcmp(status,cases[i].want)):status!=NULL)return 1;
    }
    puts("DIAGNOSTIC_OWNER PASS");return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        p=Path(temp);(p/'owner.c').write_text(prefix+template+suffix)
        subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'owner.c'),'-o',str(p/'owner')],check=True)
        subprocess.run([str(p/'owner')],check=True)

test_owner((ROOT/'tools/games/device_diag/owner.inc').read_text())
out=ROOT/'.cache/device-diag-host';out.mkdir(parents=True,exist_ok=True)
h=norm.harness(out)
s=h.read_text().replace('    norm_lifecycle_test(ctx);','').replace('    norm_boundary_test();','')
s=norm.replace(s,'/* ---- the procedural oracle */',r"""
static unsigned diaghost_n;
static double diaghost_lead;
static JSValue diaghost_mark(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;int32_t op=0;REQ(argc>0 && !JS_ToInt32(c,&op,argv[0]));
    if(op==0 && !diaghost_n)norm_views=norm_oval=norm_ou=norm_sqrt=0;
    if(op==1){REQ(argc>1 && !JS_ToFloat64(c,&diaghost_lead,argv[1]));++diaghost_n;}
    return JS_UNDEFINED;
}
/* ---- the procedural oracle */""")
s=norm.replace(s,'{"__log",js_log,1}', '{"__diagNative",diaghost_mark,2},{"__log",js_log,1}')
pre='pocket.derby._diag=__diagNative;pocket.storage.get=()=>Promise.resolve({value:null,revision:0});pocket.storage.set=()=>Promise.resolve()'
s=norm.replace(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");','    eval(PRELUDE,strlen(PRELUDE),"prelude.js");\n    {const char *s='+json.dumps(pre)+';REQ(eval(s,strlen(s),"diag-pre.js"));}')
s=norm.replace(s,'    REQ(loaded_seen==1);','''    if(getenv("DIAG_HOST")){
        for(unsigned i=0;i<1000 && diaghost_n<120;i++)frame(0);
        REQ(diaghost_n==120 && !exceptions && !bad_present && !framefails);
        printf("DIAG_HOST frames=%u pixels=%016llx lead=%.9f\\n",diaghost_n,(unsigned long long)pixel_hash,diaghost_lead);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("DIAG_HOST PASS");return 0;
    }
    REQ(loaded_seen==1);''')
h.write_text(s)
rows=[]
for mode in ('base','reuse'):
    folder=out/mode;folder.mkdir(exist_ok=True)
    src=subprocess.check_output(['git','show',norm.BASE+':'+norm.SOURCE],cwd=ROOT,text=True) if mode=='base' else (ROOT/norm.SOURCE).read_text()
    p=folder/'pocket_derby.c';p.write_text(norm.instrument(src))
    run_derby.EXTRA=[str(p) if x.endswith('pocket_derby.c') else x for x in run_derby.EXTRA]
    (folder/'build').mkdir(exist_ok=True)
    for obj in (ROOT/'.cache/view-norm-final/base/build').glob('*.o'):shutil.copy(obj,folder/'build'/obj.name)
    binary=run_derby.build(['-O2'],folder/'build',harness=h)
    for profile in ('oval','straight','pad'):
        src=folder/profile/'source';shutil.copytree(ROOT/'apps/derby',src,dirs_exist_ok=True)
        if profile!='pad':
            p=src/'derby_watch.js';p.write_text(norm.replace(p.read_text(),'const OV = [.5,',f"const OV = [{'1' if profile=='oval' else '0'},"))
        p=src/'derby_play.js';original=p.read_text()
        template=(ROOT/'tools/games/device_diag/workload.js').read_text()
        def workload(t):return original+'\n'+BEFORE+'\n'+t.replace('@PROFILE@',profile).replace('@TIER@','1' if profile=='pad' else '2')+'\n'+AFTER
        p.write_text(workload(template))
        app=run_derby.lower(src,folder/profile/'app')
        prefix=folder/profile/'run'
        env=dict(os.environ,DERBY_APP_DIR=str(app),DERBY_HEAP_LIMIT='524288',DIAG_HOST='1',NORM_TIMES=str(prefix)+'.times',NORM_STREAM=str(prefix)+'.draws')
        result=subprocess.run([str(binary)],env=env,cwd=ROOT,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(result.stdout+result.stderr)
        assert result.returncode==0,str(prefix)
        item=dict(mode=mode,profile=profile,summary=re.search(r'DIAG_HOST frames=.*',result.stdout)[0],counts=re.search(r'NORM_COUNTS (.*)',result.stdout)[1].split()[:4],draw_sha256=hashlib.sha256(prefix.with_suffix('.draws').read_bytes()).hexdigest())
        item['step_bound']=re.search(r'DIAG_STEP_BOUND max=([0-9]+) callbacks=([0-9]+)',result.stdout).groups()
        pixel,lead,draw=GOLDEN[profile]
        assert item['summary']==f'DIAG_HOST frames=120 pixels={pixel} lead={lead}'
        assert item['draw_sha256']==draw
        assert int(item['step_bound'][0])==(0 if profile=='pad' else 8)
        rows.append(item);print(json.dumps(item),flush=True)
        if mode=='base' and profile=='oval':
            p.write_text(workload(subprocess.check_output(['git','show',OLD+':tools/games/device_diag/workload.js'],cwd=ROOT,text=True)))
            negative=run_derby.lower(src,folder/profile/'negative-app')
            neg=subprocess.run([str(binary)],env=dict(env,DERBY_APP_DIR=str(negative),NORM_TIMES=str(folder/profile/"negative.times"),NORM_STREAM=str(folder/profile/"negative.draws")),cwd=ROOT,capture_output=True,text=True)
            (folder/profile/'negative.log').write_text(neg.stdout+neg.stderr)
            assert neg.returncode!=0 and 'DIAG_STEP_LIMIT' in neg.stdout+neg.stderr
            p.write_text(workload(template))
            print('OLD_SYNCHRONOUS_LOOP_REJECTED PASS',flush=True)
for profile in ('oval','straight','pad'):
    a,b=[r for r in rows if r['profile']==profile]
    assert a['summary']==b['summary'] and a['draw_sha256']==b['draw_sha256']
    assert a['counts'][:3]==b['counts'][:3]
(out/'result.json').write_text(json.dumps(rows,indent=2)+'\n')
print('DIAGNOSTIC_WORKLOAD PASS')
