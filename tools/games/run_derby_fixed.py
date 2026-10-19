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
from derby_fixed_result import parse_trial

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('fixed host source drift: '+old[:80])
    return text.replace(old,new,1)

HELPERS=r'''
#include "derby_fixed_workload.h"
static bool fx_record;static unsigned fx_rep;
static uint64_t fx_input_hash,fx_panel_hash,fx_draws;
uint64_t df_now_us(void){return dm_now_us();}
int df_write(const char *data,size_t size){return fwrite(data,1,size,stdout)==size;}
static void fx_hash(uint64_t *h,const void *p,size_t n){
    const unsigned char *b=p;for(size_t i=0;i<n;i++){*h^=b[i];*h*=1099511628211ull;}
}
'''
DRAW=r'''
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
    df_owner_begin(dm_now_us());
    if(df_measuring()&&!fx_record){
        ++fx_rep;fx_record=true;fx_draws=0;
        fx_input_hash=fx_panel_hash=pixel_hash=1469598103934665603ull;
    }
    if(fx_record)dm_dispatch(0,0);
'''
FRAME_END=r'''
    df_owner_work_end(dm_now_us());df_owner_end(dm_now_us(),true);
    if(fx_record&&df_status()==DF_TAIL){
        printf("DEVICE_WINDOW {\"rep\":%u,\"contexts\":120,\"draws\":%llu,\"input_hash\":\"%016llx\",\"procedural_pixel_hash\":\"%016llx\",\"panel_hash\":\"%016llx\"}\n",
               fx_rep,(unsigned long long)fx_draws,(unsigned long long)fx_input_hash,
               (unsigned long long)pixel_hash,(unsigned long long)fx_panel_hash);
        fx_record=false;
    }
'''
MAIN=r'''
    if(getenv("DERBY_DEVICE_FIXTURE_HOST")){
        unsigned guard=0;
        while(!gnum("__dfExited")){
            frame(0);REQ(++guard<=2200&&df_status()!=DF_ABORT);
        }
        REQ(df_status()==DF_DONE&&fx_rep==3&&!exceptions&&!bad_present&&!framefails&&!ser_bad);
        pocket_input_reset();pocket_kasane_reset();pocket_derby_reset();
        JS_FreeContext(ctx);JS_FreeRuntime(rt);puts("DEVICE_FIXTURE_HOST PASS");return 0;
    }
'''

def make_harness(out):
    text=(ROOT/'tools/games/test_derby_host.c').read_text()
    text=once(text,'/* ---- the procedural oracle */',HELPERS+'\n/* ---- the procedural oracle */')
    text=once(text,'    spec *s=find(handle);REQ(s);','    spec *s=find(handle);REQ(s);'+DRAW)
    text=once(text,'static void frame(unsigned buttons){','static void frame(unsigned buttons){'+FRAME_START)
    marker='    if(frame_regs>frame_reg_max)frame_reg_max=frame_regs;'
    text=once(text,marker,'    df_dispatch(false,false,buttons);\n'+marker)
    marker='    present((uint64_t)tick*33333u);'
    text=once(text,marker,marker+'\n    if(fx_record){dm_present(1,0);fx_hash(&fx_panel_hash,panel,sizeof panel);}\n    df_present(true,false,false);')
    text=once(text,'    tick++;scene_t++;','    tick++;scene_t++;'+FRAME_END)
    marker='    eval(PRELUDE,strlen(PRELUDE),"prelude.js");'
    text=once(text,marker,marker+'''
    const char *df_host="globalThis.__dfExited=0;app.exit=()=>{globalThis.__dfExited=1;};";
    REQ(eval(df_host,strlen(df_host),"fixture-host.js"));df_reset();
''')
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
        if any(row[f'c{j}']!=rows[0][f'c{j}'] for j in range(10)):raise ValueError('non-deterministic work counters')
        if any(row[f's{j}'].split(':')[1]!=rows[0][f's{j}'].split(':')[1] for j in range(10)):raise ValueError('non-deterministic stage calls')
        counts=[int(row[f'c{j}']) for j in range(10)]
        if counts[:4]!=[0,120,600,7735]:raise ValueError('known clip work differs')
        if variant=='native':
            if counts[4:]!=[120,7735,0,120,0,0]:raise ValueError('cache accounting differs')
        elif variant=='batch':
            if counts[4:]!=[7735,0,0,0,120,960]:raise ValueError('batch accounting differs')
        elif counts[4:]!=[7735,0,0,0,0,0]:raise ValueError('baseline accounting differs')
        if int(row['s1'].split(':')[1])!=(1246 if variant=='batch' else 2086):raise ValueError('bridge boundary calls differ')
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=ROOT/'.cache/derby_fixed_host')
    parser.add_argument('--verify-only',action='store_true')
    parser.add_argument('--m32',action='store_true',help='device-sized heap model; x87 input-bit oracle disabled')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if args.verify_only:
        result=json.loads((out/'result.json').read_text());check_result(result)
        parse_trial((out/'trial.log').read_bytes(),transport_complete=True)
        print('PASS saved actual fixture identity, whole-trial quality and deterministic work counters');return
    native=(ROOT/'main/pocket/pocket_derby.c').read_text()
    variants=re.findall(r'const char \*df_variant\(void\)\{return "(baseline|native|batch)";\}',native)
    if len(variants)!=1:raise ValueError('unique variant required')
    variant=variants[0];generated=out/'fixture';generate(ROOT/'apps/derby',generated)
    app=run_derby.lower(ROOT/'apps/derby',out/'app')
    for name in ('watch','view','play'):shutil.copy2(generated/f'derby_{name}.js',app/f'derby_{name}.js')
    if variant=='batch':
        from derby_runner_batch import generate as batch_generate
        batch_generate(app)
    harness=make_harness(out)
    options={'measure':True,'fixed_workload':True,'harness':harness}
    if variant=='batch':options['runner_batch']=True
    cache=out/('build32-sse2' if args.m32 else 'build');cache.mkdir(exist_ok=True)
    # Existing build() checks source timestamps before reusing these copied objects.
    for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
        old=ROOT/'.cache/derby_host/asan'/f'{name}.o'
        if args.m32:continue
        if old.exists() and not (cache/f'{name}.o').exists():shutil.copy2(old,cache/f'{name}.o')
    flags=['-fsanitize=address,undefined']
    if args.m32:
        flags=['-m32','-malign-double'] if os.environ.get('DERBY_FIXED_SYSTEM_M32')=='1' else subprocess.check_output(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,text=True,env=os.environ).split()
    if args.m32:
        # Match binary64 evaluation rather than i386's default x87 intermediates.
        flags += ['-msse2','-mfpmath=sse']
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
    windows=[json.loads(x[len('DEVICE_WINDOW '):]) for x in process.stdout.splitlines() if x.startswith('DEVICE_WINDOW ')]
    if len(windows)!=3:raise ValueError('three dependent repetitions required')
    for row in windows:
        if any(row[k]!=windows[0][k] for k in ('contexts','draws','input_hash','procedural_pixel_hash','panel_hash')):
            raise ValueError('repetition work/pixels differ')
    result={'variant':variant,'trial':trial,'windows':windows,'hardware_tested':False,
            'device_sized_model':args.m32,'input_bit_oracle_verified':not args.m32,'compiler_flags':flags,
            'guest_heap_limit_bytes':int(env['DERBY_HEAP_LIMIT']),
            'scope':('same-process dependent 3 replays; i386/SSE2 heap model, no sanitizers; ' if args.m32 else 'same-process dependent 3 replays; ASan/UBSan host oracle; ')+'no host pace wait, no target performance claim',
            'harness_sha256':hashlib.sha256(harness.read_bytes()).hexdigest(),
            'generated':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in app.glob('*.js')}}
    # Keep complete diagnostics when the known-answer gate rejects this run.
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    check_result(result)
    print(json.dumps({'variant':variant,'pass':True,'windows':windows,'result':str(out/'result.json')}),flush=True)

if __name__=='__main__':main()
