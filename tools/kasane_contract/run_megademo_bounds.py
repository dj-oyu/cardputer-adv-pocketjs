#!/usr/bin/env python3
"""Pinned-main A/B: exact endpoint bounds reuse; host real app, no USB."""
import argparse, csv, hashlib, json, os, random, statistics, subprocess, time
from pathlib import Path
import run_megademo_app_host as host
ROOT=host.ROOT
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
SOURCE='apps/kasane/proc_megademo.js'
def replace(s,a,b):
    assert s.count(a)==1,a
    return s.replace(a,b)

def harness(out):
    s=(ROOT/'tools/kasane_contract/test_megademo_app_host.c').read_text()
    s=replace(s,'static size_t cur_bytes,peak_bytes;', 'static size_t cur_bytes,peak_bytes,n_alloc,b_alloc;')
    s=replace(s,'h->n=n;pk_count(tlsf_len(n),0);','h->n=n;n_alloc++;b_alloc+=tlsf_len(n);pk_count(tlsf_len(n),0);')
    s=replace(s,'q->n=n;pk_count(tlsf_len(n),tlsf_len(old));','q->n=n;n_alloc++;b_alloc+=tlsf_len(n);pk_count(tlsf_len(n),tlsf_len(old));')
    s=replace(s,'static JSRuntime *rt;',r'''static unsigned long long now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
static char current_scene[64]="init";
static FILE *capture;
static JSRuntime *rt;''')
    s=replace(s,'size_t n=sp?(size_t)(sp-name):strlen(name);','size_t n=sp?(size_t)(sp-name):strlen(name);snprintf(current_scene,sizeof current_scene,"%.*s",(int)n,name);')
    s=replace(s,'    else pocket_kasane_animations_presented(now_us);','    else pocket_kasane_animations_presented(now_us);\n    if(capture)fwrite(panel,1,sizeof panel,capture);')
    start=s.index('static const char PRELUDE[]=');end=s.index('static JSValue js_now',start)
    s=s[:start]+r'''static const char PRELUDE[]=
 "globalThis.console={log:globalThis.__log};"
 "globalThis.pocket={kasane:globalThis.kasane,memory:{info(){return {internalFreeBytes:31000-1000*__mega.live}}}};"
 "(()=>{const P=pocket.kasane.procedural,R=P.register,U=P.unregister;"
 "const M=globalThis.__mega={live:0,peak:0,total:0,frameReg:0,unreg:0,frameUnreg:0};"
 "P.register=function(){const h=R.apply(P,arguments);M.live++;M.total++;M.frameReg++;if(M.live>M.peak)M.peak=M.live;return h};"
 "P.unregister=function(h){U.call(P,h);M.live--;M.unreg++;M.frameUnreg++};})();";
'''+s[end:]
    s=replace(s,'    const char *ppm_dir=getenv("MEGA_PPM");',r'''    const char *ppm_dir=getenv("MEGA_PPM");
    if(getenv("BOUNDS_CAPTURE")){capture=fopen(getenv("BOUNDS_CAPTURE"),"wb");if(!capture)return 3;}
    FILE *csv=fopen(getenv("BOUNDS_CSV"),"w");if(!csv)return 3;
    fprintf(csv,"frame,buttons,scene,zoom,js_ns,owner_ns,present_ns,alloc,bytes,reg,unreg,live,heap_raw,heap_live\n");''')
    s=replace(s,'    for(unsigned t=0;ok&&t<frames;t++){',r'''    JSValue global=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,global,"frame"),meta=JS_GetPropertyStr(ctx,global,"__mega"),probe=JS_GetPropertyStr(ctx,global,"__boundsProbe");
    JS_FreeValue(ctx,global);int zoom=0,target=0;
    for(unsigned t=0;ok&&t<frames;t++){''')
    old='''        char call[160];snprintf(call,sizeof call,"__mega.frameReg=0;__mega.frameUnreg=0;__mega.frameRegNs=0;__mega.frameDrawNs=0;frame(%u)",b);
        eval(call,strlen(call),"frame.js");
        present((uint64_t)t*33333u);'''
    s=replace(s,old,r'''        if(b&0x4000)target=target?0:44;
        zoom+=zoom<target?1:zoom>target?-1:0;
        JS_SetPropertyStr(ctx,meta,"frameReg",JS_NewInt32(ctx,0));
        JS_SetPropertyStr(ctx,meta,"frameUnreg",JS_NewInt32(ctx,0));
        JSValue arg=JS_NewInt32(ctx,b);
        size_t alloc0=n_alloc,bytes0=b_alloc;
        unsigned long long start=now_ns();
        JSValue ret=JS_Call(ctx,fn,JS_UNDEFINED,1,&arg);
        unsigned long long js_ns=now_ns()-start;
        size_t alloc_delta=n_alloc-alloc0,bytes_delta=b_alloc-bytes0;
        if(JS_IsException(ret)){exceptions++;ok=false;JSValue e=JS_GetException(ctx);const char *msg=JS_ToCString(ctx,e);printf("frame %u: %s\n",t,msg?msg:"?");JS_FreeCString(ctx,msg);JS_FreeValue(ctx,e);}
        JS_FreeValue(ctx,ret);JS_FreeValue(ctx,arg);
        JSContext *pending;while(JS_ExecutePendingJob(rt,&pending)>0){}
        pocket_kasane_end_turn();
        unsigned long long owner_ns=now_ns()-start;
        start=now_ns();present((uint64_t)t*33333u);
        unsigned long long present_ns=now_ns()-start;
        if(JS_IsFunction(ctx,probe)){JSValue v=JS_Call(ctx,probe,JS_UNDEFINED,0,NULL);if(JS_IsException(v)){exceptions++;ok=false;}JS_FreeValue(ctx,v);}
        unsigned reg_now=(unsigned)num("__mega.frameReg"),unreg_now=(unsigned)num("__mega.frameUnreg"),live_now=(unsigned)num("__mega.live");
        size_t raw_now=cur_bytes;
''')
    s=replace(s,'        if(getenv("MEGA_HEAPTRACE")&&!(t%8))',r'''        fprintf(csv,"%u,%u,%s,%d,%llu,%llu,%llu,%zu,%zu,%u,%u,%u,%zu,%zu\n",t,b,current_scene,zoom,js_ns,owner_ns,present_ns,alloc_delta,bytes_delta,reg_now,unreg_now,live_now,raw_now,cur_bytes);
        if(getenv("MEGA_HEAPTRACE")&&!(t%8))''')
    s=replace(s,'    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);','    fclose(csv);if(capture)fclose(capture);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,meta);JS_FreeValue(ctx,probe);pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);')
    s=s.replace('num("__mega.frameRegNs||0")','0').replace('num("__mega.frameDrawNs")','0')
    s=replace(s,'        JS_RunGC(rt);','        if(!getenv("BOUNDS_AUTO_GC"))JS_RunGC(rt);')
    s=replace(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");',r'''    eval(PRELUDE,strlen(PRELUDE),"prelude.js");
    if(getenv("BOUNDS_DIAG")){
      const char *stub="globalThis.__diagMarks=0;globalThis.__diagLast=-1;let opened=false;pocket.derby={_diag(op,n){if(op===0){if(opened)throw Error('double start');opened=true;}else{if(!opened||n!==__diagMarks)throw Error('bad end');opened=false;++__diagMarks;__diagLast=n;}}};";
      ok_unused:
      if(!eval(stub,strlen(stub),"diagnostic-stub.js"))return 4;
    }'''.replace('      ok_unused:\n',''))
    s=replace(s,'ok=ok&&peak<=32&&total>32&&coexist_frames>0&&!degrades;','ok=ok&&peak<=32&&(total>32||getenv("BOUNDS_DIAG"))&&coexist_frames>0&&!degrades;')
    s=replace(s,'    ok=ok&&!exceptions&&!present_failures;',r'''    ok=ok&&!exceptions&&!present_failures;
    if(getenv("BOUNDS_DIAG")){
        unsigned count=(unsigned)num("__diagMarks"),want=(unsigned)atoi(getenv("BOUNDS_DIAG"));
        int last=(int)num("__diagLast");
        printf("MEGA_DIAG_HOST marks=%u last=%d\n",count,last);
        ok=ok&&count==want&&last==(int)want-1;
    }''')
    p=out/'harness.c';p.write_text(s);return p

def build(out,sanitize=False):
    flags=['-O2']+(['-fsanitize=address,undefined','-fno-pie','-no-pie'] if sanitize else [])
    h=harness(out)
    recipe=(ROOT/'tools/kasane_contract/run_megademo_app_host.py').read_text()
    recipe=recipe.replace('"tools/kasane_contract/test_megademo_app_host.c", *kasane_sources()',repr(str(h))+', *kasane_sources()')
    env={'__file__':str(ROOT/'tools/kasane_contract/run_megademo_app_host.py'),'__name__':'bounds_build'}
    exec(compile(recipe,'host_build','exec'),env)
    return env['build'](flags,out/'build')

def source_variants(out):
    sources={'base':subprocess.check_output(['git','show',BASE+':'+SOURCE],cwd=ROOT,text=True),'fast':(ROOT/SOURCE).read_text()}
    probe=r'''    Object.freeze(FULL);Object.freeze(MON);
    globalThis.__boundsProbe=()=>{
      if(FULL.join(',')!=='0,0,240,135'||MON.join(',')!=='64,20,176,83')throw Error('bounds mutated');
      for(const step of [-1,-0,...Array.from({length:46},(_,i)=>i),NaN,Infinity,-Infinity]){
        const u=step/ZOOM,e=u*u*(3-2*u),want=FULL.map((v,j)=>rnd(v+(MON[j]-v)*e)),got=boundsAt(step);
        if(got.length!==4||!want.every((v,j)=>Object.is(v,got[j])))throw Error('bounds mismatch '+step);
      }
      return true;
    };
'''
    for mode,s in sources.items():
        (out/(mode+'.js')).write_text(s)
        (out/(mode+'-frozen.js')).write_text(replace(s,'    function build(tx) {',probe+'    function build(tx) {'))
    (out/'negative.js').write_text((out/'fast-frozen.js').read_text().replace("globalThis.__boundsProbe=()=>{","globalThis.__boundsProbe=()=>{ (()=>{ 'use strict'; FULL[0]=99; })();"))
    return sources

def summary(rows):
    rng=random.Random(42);result={}
    for case in sorted({r['case'] for r in rows}):
      result[case]={}
      for metric in ['js_total','owner_present_total','present_total','wall_s']:
        pairs=[]
        for pair in sorted({r['pair'] for r in rows if r['case']==case and r['pair']>0}):
          a,b=sorted([r for r in rows if r['case']==case and r['pair']==pair],key=lambda x:x['mode'])
          pairs.append((a[metric],b[metric]))
        ratios=[b/a for a,b in pairs]
        boot=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(10000))
        result[case][metric]={'ratio':statistics.median(ratios),'bootstrap95':[boot[249],boot[9749]],'base':statistics.median(a for a,b in pairs),'fast':statistics.median(b for a,b in pairs)}
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--pairs',type=int,default=9);p.add_argument('--sanitize',action='store_true');p.add_argument('--verify-only',action='store_true');a=p.parse_args()
    out=a.out.resolve();assert not (out/'result.json').exists(),'use a fresh output directory';out.mkdir(exist_ok=True,parents=True)
    if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    sources=source_variants(out);binary=build(out,a.sanitize);rows=[]
    if a.verify_only:
      captures=[]
      for mode in ['base','fast','base-frozen','fast-frozen','negative']:
        stem=out/mode
        env=dict(os.environ,MEGA_FRAMES='1136',MEGA_LOG='1',BOUNDS_CSV=str(stem)+'.csv',BOUNDS_CAPTURE=str(stem)+'.panels',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        r=subprocess.run([str(binary),str(stem)+'.js'],cwd=ROOT,env=env,capture_output=True,text=True)
        stem.with_suffix('.log').write_text(r.stdout+r.stderr)
        if mode=='negative':
          assert r.returncode==1 and 'exceptions=1' in r.stdout and 'AddressSanitizer' not in r.stderr and 'runtime error:' not in r.stderr
        else:
          assert r.returncode==0 and 'MEGADEMO_HOST PASS' in r.stdout,(mode,r.stdout,r.stderr)
          captures.append(stem.with_suffix('.panels').read_bytes())
      assert all(c==captures[0] for c in captures)
      report={'base':BASE,'frames':1136,'panels':1137,'bytes':len(captures[0]),'sha256':hashlib.sha256(captures[0]).hexdigest(),'frozen_pass':True,'negative_mutation_rejected':True,'sanitize':a.sanitize}
      (out/'result.json').write_text(json.dumps(report,indent=2)+chr(10));print(json.dumps(report));return
    for case,frames in [('natural',736),('controls',1136)]:
      for pair in range(a.pairs+1):
        captures=[];both=[]
        for mode in (('base','fast') if pair%2==0 else ('fast','base')):
          stem=out/f'{case}-{pair}-{mode}'
          env=dict(os.environ,MEGA_FRAMES=str(frames),MEGA_LOG='1',BOUNDS_CSV=str(stem)+'.csv',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
          if pair==0:env['BOUNDS_CAPTURE']=str(stem)+'.panels'
          start=time.perf_counter();r=subprocess.run([str(binary),str(out/(mode+'.js'))],env=env,cwd=ROOT,text=True,capture_output=True);wall=time.perf_counter()-start
          stem.with_suffix('.log').write_text(r.stdout+r.stderr);assert r.returncode==0 and 'MEGADEMO_HOST PASS' in r.stdout,str(stem)
          frames_csv=list(csv.DictReader(stem.with_suffix('.csv').open()));both.append(frames_csv)
          row=dict(case=case,pair=pair,mode=mode,wall_s=wall,js_total=sum(int(f['js_ns']) for f in frames_csv),owner_present_total=sum(int(f['owner_ns'])+int(f['present_ns']) for f in frames_csv),present_total=sum(int(f['present_ns']) for f in frames_csv))
          rows.append(row);print(json.dumps(row),flush=True)
          if pair==0:captures.append(stem.with_suffix('.panels').read_bytes())
        for x,y in zip(*both):
          for k in ['frame','buttons','scene','zoom','reg','unreg','live']:assert x[k]==y[k],(case,pair,k)
        if pair==0:
          assert captures[0]==captures[1]
          fbase=list(csv.DictReader((out/f'{case}-0-base.csv').open()))
          ffast=list(csv.DictReader((out/f'{case}-0-fast.csv').open()))
          for x,y in zip(fbase,ffast):
            delta=(int(y['alloc'])-int(x['alloc']),int(y['bytes'])-int(x['bytes']))
            assert delta in ([(-13,-648),(-26,-1296)] if int(x['zoom']) in (0,44) else [(0,0)]),(case,x['frame'],delta)
          logs=[(out/f'{case}-0-{m}.log').read_text().splitlines() for m in ['base','fast']]
          assert [x for x in logs[0] if x.startswith('  MEGADEMO')]==[x for x in logs[1] if x.startswith('  MEGADEMO')]
    report={'base':BASE,'automatic_gc':bool(os.getenv('BOUNDS_AUTO_GC')),'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'flags':['-O2'],'source_sha256':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in sources.items()},'rows':rows,'summary':summary(rows)}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('BOUNDS_APP PASS')
if __name__=='__main__':main()
