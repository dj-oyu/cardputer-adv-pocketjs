#!/usr/bin/env python3
"""Actual finite device fixture/controller on real QuickJS + renderer.

Host oracle/sanitizer elapsed timings are functional coverage, not target speed.
There is no host pace wait. Run test_derby_fixed_workload.py for direct-wait tests.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import run_derby

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from derby_fixed_workload_chunks import generate
from derby_fixed_result import parse_trial,verify_work
from derby_fixed_collector_check import check_collector

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('fixed host source drift: '+old[:80])
    return text.replace(old,new,1)

HELPERS=r'''
#include "derby_fixed_workload.h"
static bool fx_record;static unsigned fx_rep,fx_ready;
static unsigned fx_tail_steps,fx_tail_rep,fx_tail_total[3],fx_tail_turns[3],fx_tail_max[3],fx_settles[3],fx_tail_draws;
static double eval_deadline;
static double now_s(void);
static JSValue fx_tail_step(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;uint32_t rep=0;
    if(argc!=1||JS_ToUint32(c,&rep,argv[0])<0||rep<1||rep>3)return JS_ThrowInternalError(c,"tail probe identity");
    fx_tail_rep=rep;++fx_tail_steps;++fx_tail_total[rep-1];return JS_UNDEFINED;
}
static JSValue fx_tail_settle(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;uint32_t rep=0;
    if(argc!=1||JS_ToUint32(c,&rep,argv[0])<0||rep<1||rep>3)return JS_ThrowInternalError(c,"settle probe identity");
    ++fx_settles[rep-1];return JS_UNDEFINED;
}
static uint64_t fx_input_hash,fx_panel_hash,fx_draws,fx_pixels,fx_equal,fx_strips,fx_equal_strips;
uint64_t df_now_us(void){return dm_now_us();}
bool derby_runtime_resources_deadline_ok(uint32_t reserve_us){return eval_deadline>now_s()+reserve_us/1000000.0;}
/* Format-only host stub; cannot qualify runtime resource acceptance. */
bool derby_runtime_resources_read(df_resources *out){
    *out=(df_resources){8192,100000,80000,100000,163840,true};return df_status()==DF_TAIL;
}
int df_write(const char *data,size_t size){return fwrite(data,1,size,stdout)==size;}
static void fx_hash(uint64_t *h,const void *p,size_t n){
    const unsigned char *b=p;for(size_t i=0;i<n;i++){*h^=b[i];*h*=1099511628211ull;}
}
'''
DRAW=r'''
    if(df_status()==DF_TAIL)++fx_tail_draws;
    if(fx_record){
        ++fx_draws;fx_hash(&fx_input_hash,&s->program.count,sizeof s->program.count);
        for(unsigned i=0;i<s->program.count;i++){
            const ksn_proc_inst *q=&s->code[i];
            fx_hash(&fx_input_hash,&q->op,1);fx_hash(&fx_input_hash,&q->dst,1);
            fx_hash(&fx_input_hash,&q->a,1);fx_hash(&fx_input_hash,&q->b,1);
            fx_hash(&fx_input_hash,&q->value,sizeof q->value);fx_hash(&fx_input_hash,&q->color,sizeof q->color);
        }
        fx_hash(&fx_input_hash,&ni,sizeof ni);fx_hash(&fx_input_hash,in64,ni*sizeof *in64);
    }
'''
FRAME_START=r'''
    fx_tail_steps=0;eval_deadline=now_s()+.25;
    df_owner_begin(dm_now_us());
    if(df_measuring()&&!fx_record){
        ++fx_rep;fx_record=true;fx_draws=0;fx_pixels=fx_equal=fx_strips=fx_equal_strips=0;
        fx_input_hash=fx_panel_hash=pixel_hash=1469598103934665603ull;
    }
    if(fx_record)dm_dispatch(0,0);
'''
FRAME_END=r'''
    eval_deadline=0;
    if(fx_tail_steps){
        ++fx_tail_turns[fx_tail_rep-1];
        if(fx_tail_steps>fx_tail_max[fx_tail_rep-1])fx_tail_max[fx_tail_rep-1]=fx_tail_steps;
    }
    df_owner_work_end(dm_now_us());df_owner_end(dm_now_us(),true);
    if(fx_record&&df_status()==DF_TAIL){
        printf("DEVICE_WINDOW {\"rep\":%u,\"contexts\":120,\"draws\":%llu,\"input_hash\":\"%016llx\",\"procedural_pixel_hash\":\"%016llx\",\"panel_hash\":\"%016llx\"}\n",
               fx_rep,(unsigned long long)fx_draws,(unsigned long long)fx_input_hash,
               (unsigned long long)pixel_hash,(unsigned long long)fx_panel_hash);
        printf("DEVICE_TRANSFER {\"rep\":%u,\"pixels\":%llu,\"equal_previous_pixels\":%llu,\"strips\":%llu,\"equal_previous_strips\":%llu}\n",
               fx_rep,(unsigned long long)fx_pixels,(unsigned long long)fx_equal,
               (unsigned long long)fx_strips,(unsigned long long)fx_equal_strips);
        fx_record=false;
    }
'''
MAIN=r'''
    if(getenv("DERBY_DEVICE_FIXTURE_HOST")){
        REQ(fx_ready==1&&df_status()==DF_READY&&tick==0);
        const char *tail_probe="const __dfStep=step,__dfSettle=settle;"
            "step=function(f,s,dt){if(dfMode===3)__dfTailStep(dfRep);return __dfStep(f,s,dt)};"
            "settle=function(){__dfTailSettle(dfRep);return __dfSettle()};";
        REQ(eval(tail_probe,strlen(tail_probe),"tail-probe.js"));
        unsigned guard=0;
        while(!gnum("__dfExited")){
            frame(0);REQ(++guard<=2200&&df_status()!=DF_ABORT&&!exceptions);
        }
        REQ(df_status()==DF_DONE&&fx_rep==3&&!exceptions&&!bad_present&&!framefails&&!ser_bad);
        REQ(!fx_tail_draws);
        for(unsigned i=0;i<3;i++)
            printf("DEVICE_TAIL {\"rep\":%u,\"steps\":%u,\"turns\":%u,\"max_steps\":%u,\"settles\":%u,\"guest_draws\":%u}\n",
                   i+1,fx_tail_total[i],fx_tail_turns[i],fx_tail_max[i],fx_settles[i],fx_tail_draws);
        REQ(df_fixed_exit_record());
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();
        puts("APP_STOPPED");puts("HOME_READY");
        JS_FreeContext(ctx);JS_FreeRuntime(rt);puts("DEVICE_FIXTURE_HOST PASS");return 0;
    }
'''

def make_harness(out):
    text=(ROOT/'tools/games/test_derby_host.c').read_text()
    text=once(text,'static double eval_deadline;','')
    # The old harness cleared its only deadline after app initialization. Arm
    # the production 250ms turn backstop for frame calls too. A deterministic
    # 32-step stress budget also exercises fatal QuickJS interruption on fast
    # hosts; it is a test model, not an estimate of S3 execution time.
    text=once(text,'return eval_deadline>0&&now_s()>eval_deadline;',
              'return fx_tail_steps>32||(eval_deadline>0&&now_s()>eval_deadline);')
    text=once(text,'{"__log",js_log,1},',
              '{"__dfTailStep",fx_tail_step,1},{"__dfTailSettle",fx_tail_settle,1},{"__log",js_log,1},')
    # A memory-only shell model supplies identity and post-cleanup stop/home;
    # READY and all fixed summaries must come from the generated app/native C.
    text=once(text,'    printf("  [%5u] %s\\n",tick,s);',
              '    if(!strncmp(s,"DERBY READY",11)){REQ(tick==0&&df_status()==DF_READY&&++fx_ready==1);}\n    printf("%s\\n",s);')
    text=once(text,'/* ---- the procedural oracle */',HELPERS+'\n/* ---- the procedural oracle */')
    text=once(text,'    spec *s=find(handle);REQ(s);','    spec *s=find(handle);REQ(s);'+DRAW)
    text=once(text,'    (void)o;memcpy(panel+y*240,p,rows*240*sizeof *p);return KSN_OK;',r"""
    if(fx_record){
        uint64_t equal=0;size_t count=(size_t)rows*240;
        for(size_t i=0;i<count;i++)equal+=panel[(size_t)y*240+i]==p[i];
        fx_pixels+=count;fx_equal+=equal;fx_strips++;fx_equal_strips+=equal==count;
    }
    (void)o;memcpy(panel+y*240,p,rows*240*sizeof *p);return KSN_OK;""")
    text=once(text,'static void frame(unsigned buttons){' ,'static void frame(unsigned buttons){'+FRAME_START)
    marker='    if(frame_regs>frame_reg_max)frame_reg_max=frame_regs;'
    text=once(text,marker,'    df_dispatch(false,false,buttons);\n'+marker)
    marker='    present((uint64_t)tick*33333u);'
    text=once(text,marker,marker+'\n    if(fx_record){dm_present(1,0);fx_hash(&fx_panel_hash,panel,sizeof panel);}\n    df_present(true,false,false);')
    text=once(text,'    tick++;scene_t++;','    tick++;scene_t++;'+FRAME_END)
    marker='    eval(PRELUDE,strlen(PRELUDE),"prelude.js");'
    text=once(text,marker,marker+'''
    const char *df_host="globalThis.__dfExited=0;app.exit=()=>{globalThis.__dfExited=1;};";
    REQ(eval(df_host,strlen(df_host),"fixture-host.js"));df_reset();
    if(getenv("DERBY_FIXED_NO_TONES")){
        const char *silent="pocket.capabilities.get=()=>({available:false})";
        REQ(eval(silent,strlen(silent),"no-tones.js"));
    }
    if(getenv("DERBY_FIXED_INIT_FAIL")){
        const char *fault="pocket.kasane.procedural.resource=()=>{throw Error('DF_INIT_TEST')};";
        REQ(eval(fault,strlen(fault),"init-fault.js"));
    }
''')
    text=once(text,'    bool ok=eval(src,n,path);','    puts("APP_ID local.derby");\n    bool ok=eval(src,n,path);')
    text=once(text,'    REQ(loaded_seen==1);',MAIN+'\n    REQ(loaded_seen==1);')
    path=out/'test_derby_fixed_host.c';path.write_text(text);return path

def check_result(result):
    windows=result['windows'];rows=result['trial']['repetitions'];variant=result['variant']
    expected={'contexts':120,'draws':4975,'input_hash':'986b2e6e027af00d',
              'procedural_pixel_hash':'c1b0fdbde748d0de','panel_hash':'cec4b765a2794b14'}
    if result.get('device_sized_model'):expected.pop('input_hash')
    if len(windows)!=3 or len(rows)!=3:raise ValueError('three dependent repetitions required')
    for i,(window,row) in enumerate(zip(windows,rows,strict=True),1):
        mismatches={k:{'expected':v,'actual':window[k]} for k,v in expected.items() if window[k]!=v}
        if window['rep']!=i or mismatches:raise ValueError('known fixture work/pixels differ: '+json.dumps({'rep':window['rep'],'mismatches':mismatches},sort_keys=True))
        # This no-continuation host harness also checks all model-local calls.
        # The device gate only freezes fixed work scopes; continuation/render
        # scopes have their own invariants in the shared verifier.
        if any(row[f's{j}'].split(':')[1]!=rows[0][f's{j}'].split(':')[1] for j in range(10)):raise ValueError('non-deterministic host stage calls')
    verify_work(rows,variant)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=ROOT/'.cache/derby_fixed_host')
    parser.add_argument('--verify-only',action='store_true')
    parser.add_argument('--detail',action='store_true',help='owner-task detail and logical copy counters')
    parser.add_argument('--detail-fast',action='store_true',help='optimized functional overhead control without sanitizers')
    parser.add_argument('--m32',action='store_true',help='device-sized heap model; x87 input-bit oracle disabled')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.verify_only:
        result=json.loads((out/'result.json').read_text());check_result(result)
        parse_trial((out/'trial.log').read_bytes(),transport_complete=True)
        check_collector((out/'trial.log').read_bytes())
        from derby_detail_result import parse_detail
        if parse_detail((out/'trial.log').read_text(),expected=result.get('detail_enabled',False))!=result.get('detail',[]):
            raise ValueError('saved detailed result/log mismatch')
        print('PASS saved actual fixture identity, whole-trial quality and deterministic work counters');return
    native=(ROOT/'main/pocket/pocket_derby.c').read_text()
    variants=re.findall(r'const char \*df_variant\(void\)\{return "(baseline|native|batch)";\}',native)
    if len(variants)!=1:raise ValueError('unique variant required')
    variant=variants[0];generated=out/'fixture';generate(ROOT/'apps/derby',generated,args.detail)
    app=run_derby.lower(ROOT/'apps/derby',out/'app')
    for name in ('watch','view','play','fixed'):shutil.copy2(generated/f'derby_{name}.js',app/f'derby_{name}.js')
    chunks=app/'chunks.txt';chunks.write_text(chunks.read_text()+'\nfixed derby_fixed.js\n')
    if args.detail:shutil.copy2(generated/'derby_pan.js',app/'derby_pan.js')
    if variant=='batch':
        from derby_runner_batch import generate as batch_generate
        batch_generate(app)
    harness=make_harness(out)
    options={'measure':True,'fixed_workload':True,'harness':harness}
    if variant=='batch':options['runner_batch']=True
    cache=out/(('build32-sse2' if args.m32 else 'build')+('-fast' if args.detail_fast else '-asan')+('-detail' if args.detail else '-off'));cache.mkdir(exist_ok=True)
    # Existing build() checks source timestamps before reusing these copied objects.
    for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
        old=ROOT/'.cache/derby_host/asan'/f'{name}.o'
        if args.m32 or args.detail_fast:continue
        if old.exists() and not (cache/f'{name}.o').exists():shutil.copy2(old,cache/f'{name}.o')
    flags=['-fsanitize=address,undefined']
    if args.m32:
        flags=['-m32','-malign-double'] if os.environ.get('DERBY_FIXED_SYSTEM_M32')=='1' else subprocess.check_output(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,text=True,env=os.environ).split()
    if args.m32:
        # Match binary64 evaluation rather than i386's default x87 intermediates.
        flags += ['-msse2','-mfpmath=sse']
    if args.detail_fast:
        flags=[*flags,'-O2'] if args.m32 else ['-O2']
    if args.detail:flags += ['-DPOCKET_DERBY_DETAIL_MEASURE']
    binary=run_derby.build(flags,cache,**options)
    env=dict(os.environ,DERBY_APP_DIR=str(app),DERBY_HEAP_LIMIT='524288',DERBY_HW='0x2545F491',
             DERBY_DEVICE_FIXTURE_HOST='1',DERBY_SER_ORACLE='1',DERBY_SER_FUZZ='4000',
             ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.m32:
        env['DERBY_HEAP_LIMIT']='163840';env.pop('DERBY_SER_ORACLE');env.pop('DERBY_SER_FUZZ')
    if variant=='batch' and not args.m32:
        view=(ROOT/'apps/derby/derby_view.js').read_text();a=view.index('const rin = ')+len('const rin = ')
        oracle=out/'runner_oracle.js'
        oracle.write_text((ROOT/'tools/games/derby_runner_batch_oracle.js').read_text().replace('__REFERENCE_RIN__',view[a:view.index(';',a)]))
        env['DERBY_RUNNER_BATCH_ORACLE']=str(oracle)
    process=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
    log=out/'trial.log';log.write_text(process.stdout+process.stderr)
    if process.returncode:raise RuntimeError('finite fixture failed; inspect '+str(log))
    trial=parse_trial(log.read_bytes(),transport_complete=True)
    from derby_detail_result import parse_detail
    detail=parse_detail(log.read_text(),expected=args.detail)
    windows=[json.loads(x[len('DEVICE_WINDOW '):]) for x in process.stdout.splitlines() if x.startswith('DEVICE_WINDOW ')]
    if len(windows)!=3:raise ValueError('three dependent repetitions required')
    for row in windows:
        if any(row[k]!=windows[0][k] for k in ('contexts','draws','input_hash','procedural_pixel_hash','panel_hash')):
            raise ValueError('repetition work/pixels differ')
    transfers=[json.loads(x[len('DEVICE_TRANSFER '):]) for x in process.stdout.splitlines() if x.startswith('DEVICE_TRANSFER ')]
    if len(transfers)!=3 or any(t!={'rep':i,'pixels':3888000,'equal_previous_pixels':3499706,'strips':2040,'equal_previous_strips':449} for i,t in enumerate(transfers,1)):
        raise ValueError('host previous-pixel/strip observation changed')
    result={'transfers':transfers,'detail_enabled':args.detail,'detail':detail,'variant':variant,'trial':trial,'windows':windows,'hardware_tested':False,
            'device_sized_model':args.m32,'input_bit_oracle_verified':not args.m32,'compiler_flags':flags,
            'guest_heap_limit_bytes':int(env['DERBY_HEAP_LIMIT']),
            'scope':('same-process dependent 3 replays; i386/SSE2 heap model, no sanitizers; ' if args.m32 else ('same-process dependent 3 replays; optimized host oracle, no sanitizers; ' if args.detail_fast else 'same-process dependent 3 replays; ASan/UBSan host oracle; '))+'no host pace wait, no target performance claim',
            'harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),
            'generated':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in app.glob('*.js')}}
    # Keep complete diagnostics when the known-answer gate rejects this run.
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    check_result(result)
    reference=json.loads(subprocess.check_output(['node',str(ROOT/'tools/games/derby_fixed_tail_oracle.js'),
                         str(generated/'derby_watch.js'),str(generated/'fixture.json')],text=True))
    tails=[json.loads(line[len('DEVICE_TAIL '):]) for line in process.stdout.splitlines() if line.startswith('DEVICE_TAIL ')]
    if len(tails)!=3:raise ValueError('three tail records required')
    for rep,tail in enumerate(tails,1):
        if tail!={'rep':rep,'steps':reference['tail_steps'],'turns':92,'max_steps':8,'settles':1,'guest_draws':0}:
            raise ValueError('tail work/return/settlement mismatch: '+json.dumps(tail))
    # Restore just the previous synchronous tail in a copy of the actual
    # generated app. The same binary must now reject it through QuickJS's fatal
    # interrupt path, not quietly qualify the old fixture again on a fast host.
    old=out/'old-tail-app';shutil.copytree(app,old,dirs_exist_ok=True)
    path=old/'derby_fixed.js';text=path.read_text()
    a=text.index('      // DF bounded tail:');b=text.index('      settle();',a)
    path.write_text(text[:a]+text[b:])
    bad=subprocess.run([str(binary)],cwd=ROOT,env=env|{'DERBY_APP_DIR':str(old)},capture_output=True,text=True)
    bad_raw=(bad.stdout+bad.stderr).encode();(out/'old-tail-rejected.log').write_bytes(bad_raw)
    if bad.returncode==0 or b'InternalError: interrupted' not in bad_raw or b'at settle' not in bad_raw or b'DERBY_FIXED v=' in bad_raw:
        raise ValueError('old synchronous tail escaped watchdog regression')
    result['tail_contract']={'reference':reference,'actual':tails,'old_tail_rejected':True,
        'old_tail_exit_code':bad.returncode,'old_tail_raw_sha256':hashlib.sha256(bad_raw).hexdigest(),
        'scope':'250ms host frame backstop plus deterministic 32-step stress interrupt; not target timing proof'}
    # This checks the actual app's initialization marker rather than inventing
    # READY in a synthetic transport fixture (the previous host smoke missed it).
    startup={}
    for name,extra in [('no-tones',{'DERBY_FIXED_NO_TONES':'1','DERBY_EVAL_ONLY':'1'}),
                       ('init-failure',{'DERBY_FIXED_INIT_FAIL':'1'})]:
        probe=subprocess.run([str(binary)],cwd=ROOT,env=env|extra,capture_output=True,text=True)
        raw=(probe.stdout+probe.stderr).encode();(out/(name+'.log')).write_bytes(raw)
        ready=[line for line in raw.splitlines() if line.startswith(b'DERBY READY')]
        if name=='no-tones':
            if probe.returncode or ready!=[b'DERBY READY tones=false']:raise ValueError('actual no-tone initialization marker invalid')
        elif probe.returncode==0 or ready or b'DF_INIT_TEST' not in raw:
            raise ValueError('failed actual initialization emitted READY or did not fail')
        startup[name]={'exit_code':probe.returncode,'ready_count':len(ready),'sha256':hashlib.sha256(raw).hexdigest()}
    # The nested controller load is part of startup, never an optional feature.
    # A missing/invalid/throwing chunk must escape play before enter/READY.
    for name,code,expected in [
            ('controller-missing',None,b"no chunk 'fixed'"),
            ('controller-syntax','(',b"chunk 'fixed' did not compile"),
            ('controller-throw',"throw Error('DF_CONTROLLER_TEST');",b'DF_CONTROLLER_TEST'),
            ('controller-strict',"__dfUndeclared=1;",b'__dfUndeclared')]:
        probe_app=out/name;shutil.copytree(app,probe_app,dirs_exist_ok=True)
        if code is None:
            path=probe_app/'chunks.txt'
            path.write_text('\n'.join(line for line in path.read_text().splitlines()
                                       if not line.startswith('fixed '))+'\n')
        else:
            path=probe_app/'derby_fixed.js';text=path.read_text()
            directive="'use strict';";assert text.startswith(directive)
            path.write_text(directive+'\n'+code+'\n'+text[len(directive):])
        probe=subprocess.run([str(binary)],cwd=ROOT,
                             env=env|{'DERBY_APP_DIR':str(probe_app),'DERBY_EVAL_ONLY':'1'},
                             capture_output=True,text=True)
        raw=(probe.stdout+probe.stderr).encode();(out/(name+'.log')).write_bytes(raw)
        if (probe.returncode==0 or expected not in raw or b'DERBY READY' in raw
                or b'DERBY_FIXED v=' in raw):
            raise ValueError('controller startup failure swallowed: '+name)
        startup[name]={'exit_code':probe.returncode,'ready_count':0,
                       'sha256':hashlib.sha256(raw).hexdigest()}
    result['collector_contract']=check_collector(log.read_bytes(),detail_enabled=args.detail)
    result['startup_probes']=startup
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'variant':variant,'pass':True,'windows':windows,'result':str(out/'result.json')}),flush=True)

if __name__=='__main__':main()
