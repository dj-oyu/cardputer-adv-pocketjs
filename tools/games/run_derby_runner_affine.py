#!/usr/bin/env python3
"""Bounded Q14 runner comparison. Host timings never gate or imply S3 speed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import zlib
import run_derby

ROOT=Path(__file__).resolve().parents[2]
SOURCES=['main/pocket/derby_runner_affine.c','main/ui/kasane/ksn_proc_plan.c',
         'main/ui/kasane/ksn_proc_analysis.c','main/ui/kasane/ksn_procedural.c',
         'main/ui/kasane/ksn_proc_points.c','main/ui/kasane/ksn_proc_points_pie.c',
         'main/ui/kasane/ksn_proc_points_dispatch.c']

def run(cmd,**kw):
    return subprocess.run(cmd,cwd=ROOT,check=True,text=True,**kw)

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('affine harness source drift: '+old[:100])
    return text.replace(old,new,1)

def harness(out):
    text=(ROOT/'tools/games/test_derby_host.c').read_text()
    text=once(text,'#include "pocket_proc.h"','#include "pocket_proc.h"\n#include "derby_runner_affine.h"')
    text=once(text,'    int handle;','    int handle;\n    bool runner_affine;float runner_colors[3];')
    text=once(text,'        n=e->count;','''        s->runner_affine=!strcmp(e->name,"derby.runner");
        if(s->runner_affine){REQ(given==3);memcpy(s->runner_colors,arg,3*sizeof(float));}
        n=e->count;''')
    text=once(text,'    append(&cand_plan,&f_plan);append(&cand_vm,&f_vm);','''
    if(s->runner_affine){
        ksn_proc_frame alternate;
        if(derby_runner_affine(in,s->runner_colors,NULL,&alternate,NULL)){
            REQ(alternate.count==f_plan.count);
            for(unsigned i=0;i<alternate.count;i++){
                const ksn_proc_segment *a=&alternate.segments[i],*b=&f_plan.segments[i];
                REQ(a->color==b->color&&abs(a->x0-b->x0)<=1&&abs(a->x1-b->x1)<=1&&
                    abs(a->y0-b->y0)<=1&&abs(a->y1-b->y1)<=1);
            }
            f_plan=alternate;f_vm=alternate;
        }
    }
    append(&cand_plan,&f_plan);append(&cand_vm,&f_vm);''')
    text+='''
__attribute__((destructor)) static void affine_counts(void){
    DerbyRunnerAffineCounts c;pocket_proc_runner_affine_counts(&c,false);
    printf("AFFINE_PIPELINE %u %u %u %u %u\\n",c.attempts,c.accepted,c.fallback,c.scalar_batches,c.pie_batches);
}
'''
    path=out/'test_derby_affine_pipeline.c';path.write_text(text);return path

def png_from_ppm(path):
    raw=path.read_bytes();header,pixels=raw.split(b'\n255\n',1)
    w,h=map(int,header.splitlines()[1].split());assert len(pixels)==w*h*3
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
    path.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'.cache/derby_runner_affine')
    p.add_argument('--full',action='store_true',help='real QuickJS/adapter/full renderer at all three tiers')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    table=run_derby.rom_table(out)
    fixtures=out/'inputs.txt'
    fixture=run(['node','tools/games/export_derby_affine_inputs.mjs',str(fixtures)],capture_output=True)
    print(fixture.stdout,end='')
    metrics={}
    for label,flags in [('optimized',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=out/label
        run(['cc','-std=c11',*flags,'-Wall','-Wextra','-Werror','-ffp-contract=off','-DKSN_PROC_POINTS_PIE_MODEL',
             '-I','main','-I','main/pocket','-I','main/ui/kasane','-I','tools/hostshim',
             'tools/games/test_derby_runner_affine.c',*SOURCES,str(table),'-lm','-o',str(binary)])
        result=subprocess.run([str(binary),str(fixtures),str(out/'worst.ppm')],cwd=ROOT,capture_output=True,text=True,
                              env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode();print(result.stdout,end='')
        metrics[label]=json.loads(re.search(r'^AFFINE_RESULT (.+)$',result.stdout,re.M)[1])
        metrics[label]['input_range']=json.loads(re.search(r'^AFFINE_INPUT_RANGE (.+)$',result.stdout,re.M)[1])
    png_from_ppm(out/'worst.ppm')
    pipeline=[]
    if args.full:
        cache=out/'pipeline-build';cache.mkdir(exist_ok=True)
        # Reuse only compatible ASan QuickJS objects when available. build()
        # still checks source mtimes; candidate flags affect no QuickJS code.
        for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
            old=ROOT/'.cache/derby_host/asan'/f'{name}.o'
            if old.exists() and not (cache/f'{name}.o').exists():shutil.copy2(old,cache/f'{name}.o')
        binary=run_derby.build(['-fsanitize=address,undefined'],cache,runner_affine=True,harness=harness(out))
        app=run_derby.lower(ROOT/'apps/derby',out/'app')
        for tier in (0,1,2):
            env=dict(os.environ,DERBY_APP_DIR=str(app),DERBY_HEAP_LIMIT='524288',DERBY_TIER=str(tier),
                     ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            result=subprocess.run([str(binary)],cwd=ROOT,env=env,capture_output=True,text=True)
            (out/f'pipeline-tier{tier}.log').write_text(result.stdout+result.stderr)
            result.check_returncode()
            found=re.search(r'AFFINE_PIPELINE (\d+) (\d+) (\d+) (\d+) (\d+)',result.stdout)
            if not found:raise ValueError('pipeline counts missing')
            attempts,accepted,fallback,scalar,pie=map(int,found.groups())
            assert accepted>0 and attempts==accepted+fallback and scalar==accepted and pie==0
            pipeline.append(dict(tier=tier,attempts=attempts,accepted=accepted,fallback=fallback,scalar_batches=scalar,pie_batches=pie))
            print('AFFINE_PIPELINE_PASS',pipeline[-1],flush=True)
    report=dict(scope='host functional/numerical/render comparison; no target cycles/speed or device acceptance',
                fixture=json.loads(fixture.stdout),metrics=metrics,pipeline=pipeline,
                source_sha256={path:hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in SOURCES},
                artifacts=dict(worst='worst.png',layout='float VM | opt-in integer | differing pixels (magenta)'))
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS bounded affine, actual runner VM reference, PIE arithmetic model, temporal and raster comparison',flush=True)

if __name__=='__main__':main()
