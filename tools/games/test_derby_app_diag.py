#!/usr/bin/env python3
"""Run the exact diagnostic JS workload through the real host app/renderer."""
import hashlib,json,os,re,shutil,subprocess
from pathlib import Path
import run_derby,run_derby_view_norm as norm
ROOT=norm.ROOT
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
        for(unsigned i=0;i<250 && diaghost_n<120;i++)frame(0);
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
    for obj in (ROOT/'.cache/view-norm-final/base/build').glob('*.o'):shutil.copy2(obj,folder/'build'/obj.name)
    binary=run_derby.build(['-O2'],folder/'build',harness=h)
    for profile in ('oval','straight','pad'):
        src=folder/profile/'source';shutil.copytree(ROOT/'apps/derby',src,dirs_exist_ok=True)
        if profile!='pad':
            p=src/'derby_watch.js';p.write_text(norm.replace(p.read_text(),'const OV = [.5,',f"const OV = [{'1' if profile=='oval' else '0'},"))
        p=src/'derby_play.js';p.write_text(p.read_text()+'\n'+(ROOT/'tools/games/device_diag/workload.js').read_text().replace('@PROFILE@',profile).replace('@TIER@','1' if profile=='pad' else '2'))
        app=run_derby.lower(src,folder/profile/'app')
        prefix=folder/profile/'run'
        env=dict(os.environ,DERBY_APP_DIR=str(app),DERBY_HEAP_LIMIT='524288',DIAG_HOST='1',NORM_TIMES=str(prefix)+'.times',NORM_STREAM=str(prefix)+'.draws')
        result=subprocess.run([str(binary)],env=env,cwd=ROOT,capture_output=True,text=True)
        prefix.with_suffix('.log').write_text(result.stdout+result.stderr)
        assert result.returncode==0,str(prefix)
        item=dict(mode=mode,profile=profile,summary=re.search(r'DIAG_HOST frames=.*',result.stdout)[0],counts=re.search(r'NORM_COUNTS (.*)',result.stdout)[1].split()[:4],draw_sha256=hashlib.sha256(prefix.with_suffix('.draws').read_bytes()).hexdigest())
        rows.append(item);print(json.dumps(item),flush=True)
for profile in ('oval','straight','pad'):
    a,b=[r for r in rows if r['profile']==profile]
    assert a['summary']==b['summary'] and a['draw_sha256']==b['draw_sha256']
    assert a['counts'][:3]==b['counts'][:3]
(out/'result.json').write_text(json.dumps(rows,indent=2)+'\n')
print('DIAGNOSTIC_WORKLOAD PASS')
