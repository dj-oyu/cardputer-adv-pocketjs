#!/usr/bin/env python3
"""Prepare a disposable checkout for a finite real-app device diagnostic.
Never opens hardware. Does not modify the caller's checkout or shared config.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TEMPLATE=ROOT/'tools/games/device_diag'
def change(s,old,new):
    assert s.count(old)==1,old
    return s.replace(old,new)
def prepare(dst,profile):
    assert dst!=ROOT,'use a separate clean checkout'
    status=subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=dst,text=True)
    assert not status,'diagnostic target must be clean'
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=dst,text=True).strip()
    modified=[]
    def write(name,s):
        (dst/name).write_text(s);modified.append(name)
    def read(name):return (dst/name).read_text()
    runtime=(TEMPLATE/'runtime.inc').read_text()
    s=read('main/pocket/pocket_derby.c')
    s=change(s,'#include <math.h>','#include <math.h>\n'+runtime)
    s=change(s,'static bool ou(unsigned j,double w,double m,double zf){','static bool ou(unsigned j,double w,double m,double zf){\n    ++derby_diag_ou;')
    s=change(s,'sqrt(f*f+25600)','(++derby_diag_sqrt,sqrt(f*f+25600))')
    s=change(s,'    course->in_view=0;','    ++derby_diag_views;\n    course->in_view=0;')
    s=change(s,'        JS_CFUNC_DEF("course",3,js_course),','        JS_CFUNC_DEF("_diag",2,js_diag),\n        JS_CFUNC_DEF("course",3,js_course),')
    write('main/pocket/pocket_derby.c',s)
    s=read('components/pocketjs_guest/src/guest.c')
    wrappers='''extern unsigned derby_diag_alloc,derby_diag_bytes,derby_diag_realloc;
static void *diag_malloc(void *p,size_t n){++derby_diag_alloc;derby_diag_bytes+=n;return guest_malloc(p,n);}
static void *diag_calloc(void *p,size_t c,size_t n){++derby_diag_alloc;derby_diag_bytes+=c*n;return guest_calloc(p,c,n);}
static void *diag_realloc(void *p,void *v,size_t n){++derby_diag_realloc;derby_diag_bytes+=n;return guest_realloc(p,v,n);}
'''
    s=change(s,'static const JSMallocFunctions GUEST_ALLOCATOR = {',wrappers+'\nstatic const JSMallocFunctions GUEST_ALLOCATOR = {')
    for n in ('malloc','calloc','realloc'):s=change(s,f'.js_{n} = guest_{n},',f'.js_{n} = diag_{n},')
    write('components/pocketjs_guest/src/guest.c',s)
    owner=(TEMPLATE/'owner.inc').read_text().replace('@WORKLOAD@',f'derby-app-{profile}-120-v1').replace('@SOURCE@',head)
    s=read('main/main.c')
    s=change(s,'static void ui_task(void *arg) {',owner+'\nstatic void ui_task(void *arg) {')
    s=change(s,'    while(1) {\n        /* App teardown','    while(1) {\n        diag_owner_step();\n        /* App teardown')
    s=change(s,'            tick_run(have,&stroke);','''            const int64_t diag_start=esp_timer_get_time();
            tick_run(diag_owner_active?false:have,&stroke);
            if(diag_owner_active)derby_diag_owner_tick((unsigned)(esp_timer_get_time()-diag_start),app_turn_continued());''')
    write('main/main.c',s)
    if profile!='pad':
        s=read('apps/derby/derby_watch.js')
        s=change(s,'const OV = [.5,',f"const OV = [{'1' if profile=='oval' else '0'},")
        write('apps/derby/derby_watch.js',s)
    s=(TEMPLATE/'workload.js').read_text().replace('@PROFILE@',profile).replace('@TIER@','1' if profile=='pad' else '2')
    write('apps/derby/derby_play.js',read('apps/derby/derby_play.js')+'\n'+s)
    diff=subprocess.check_output(['git','diff','--binary'],cwd=dst)
    (dst/'diagnostic.patch').write_bytes(diff)
    manifest=dict(source=head,workload=f'derby-app-{profile}-120-v1',allocator='production GUEST_ALLOCATOR delegated unchanged via request counters',changed_files=modified,
                  sha256={name:hashlib.sha256((dst/name).read_bytes()).hexdigest() for name in modified},patch_sha256=hashlib.sha256(diff).hexdigest())
    (dst/'diagnostic.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--checkout',type=Path,required=True);p.add_argument('--profile',choices=['oval','straight','pad'],required=True)
    a=p.parse_args();prepare(a.checkout.resolve(),a.profile)
