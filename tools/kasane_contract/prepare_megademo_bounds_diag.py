#!/usr/bin/env python3
"""Finite product MEGADEMO diagnostic; no device or broker changes."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
PRELUDE="(()=>{let n=0;const log=console.log;console.log=(...a)=>{if(n++<16)log(...a);if(String(a[0]).startsWith('MEGADEMO DEGRADE'))throw Error('MEGA_DIAG_DEGRADE')}})();"
WORKLOAD=r'''
// Diagnostic checkout only: one ordinary app frame per owner callback.
(() => {
  const run=globalThis.frame, mark=pocket.derby._diag;
  let warm=0, measured=0;
  globalThis.frame=function() {
    if(warm++<48) { run(0); return; }
    mark(0);
    run(0);
    mark(1,measured++);
  };
})();
'''
def change(s,a,b):
    assert s.count(a)==1,a
    return s.replace(a,b)
def owner_template(template,head):
    owner=(template/'owner.inc').read_text().replace('DERBY_APP_DIAG','MEGA_APP_DIAG').replace('@WORKLOAD@','megademo-app-twist-120-v1').replace('@SOURCE@',head)
    a=owner.index('static const char diag_prelude[]=');b=owner.index('static void diag_owner_step',a)
    owner=owner[:a]+'static const char diag_prelude[]=\n    '+json.dumps(PRELUDE)+';\n'+owner[b:]
    owner=owner.replace('"local.derby"','"local.megademo"').replace('derby_watch_start','proc_megademo_start').replace('derby_watch_end','proc_megademo_end')
    return owner
def prepare(dst,common):
    assert dst!=ROOT
    assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=dst,text=True)
    template=common.parent/'device_diag'
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=dst,text=True).strip()
    modified=[]
    def write(p,s): (dst/p).write_text(s);modified.append(p)
    runtime=(template/'runtime.inc').read_text().replace('DERBY_APP_DIAG','MEGA_APP_DIAG')
    runtime=runtime.replace(' views=%u ou=%u sqrt=%u','').replace(',diag_views,diag_ou,diag_sqrt);',');')
    runtime=runtime.replace(' lead=%.9f..%.9f',' sample_index=%.0f..%.0f')
    # The same marker in the existing lazy namespace on both sides.
    # This workload never calls or instruments DERBY math.
    s=(dst/'main/pocket/pocket_derby.c').read_text()
    s=change(s,'#include <math.h>','#include <math.h>\n'+runtime)
    s=change(s,'        JS_CFUNC_DEF("course",3,js_course),','        JS_CFUNC_DEF("_diag",2,js_diag),\n        JS_CFUNC_DEF("course",3,js_course),')
    write('main/pocket/pocket_derby.c',s)
    s=(dst/'components/pocketjs_guest/src/guest.c').read_text()
    wrappers='''extern unsigned derby_diag_alloc,derby_diag_bytes,derby_diag_realloc;
static void *diag_malloc(void *p,size_t n){++derby_diag_alloc;derby_diag_bytes+=n;return guest_malloc(p,n);}
static void *diag_calloc(void *p,size_t c,size_t n){++derby_diag_alloc;derby_diag_bytes+=c*n;return guest_calloc(p,c,n);}
static void *diag_realloc(void *p,void *v,size_t n){++derby_diag_realloc;derby_diag_bytes+=n;return guest_realloc(p,v,n);}
'''
    s=change(s,'static const JSMallocFunctions GUEST_ALLOCATOR = {',wrappers+'\nstatic const JSMallocFunctions GUEST_ALLOCATOR = {')
    for name in ('malloc','calloc','realloc'):s=change(s,f'.js_{name} = guest_{name},',f'.js_{name} = diag_{name},')
    write('components/pocketjs_guest/src/guest.c',s)
    owner=owner_template(template,head)
    s=(dst/'main/main.c').read_text()
    s=change(s,'static void ui_task(void *arg) {',owner+'\nstatic void ui_task(void *arg) {')
    s=change(s,'    while(1) {\n        /* App teardown','    while(1) {\n        diag_owner_step();\n        /* App teardown')
    s=change(s,'            tick_run(have,&stroke);','''            const int64_t diag_start=esp_timer_get_time();
            tick_run(diag_owner_active?false:have,&stroke);
            if(diag_owner_active)derby_diag_owner_tick((unsigned)(esp_timer_get_time()-diag_start),app_turn_continued());''')
    write('main/main.c',s)
    write('apps/kasane/proc_megademo.js',(dst/'apps/kasane/proc_megademo.js').read_text()+WORKLOAD)
    diff=subprocess.check_output(['git','diff','--binary'],cwd=dst)
    (dst/'diagnostic.patch').write_bytes(diff)
    manifest=dict(source=head,workload='megademo-app-twist-120-v1',preparation='48 natural NEWS frames, one per normal owner turn; 120 TWIST frames 0..119 at initial HEAVY tier, zoom=0',
        allocator='production GUEST_ALLOCATOR forwarded unchanged; request counters (including failed requests), not host TLSF model',
        timing='native brackets around ordinary frame(0); product owner, scheduler and LCD; 24-second hard deadline checked before success; normal watchdog/runaway guards',
        diagnostic_overhead='same lazy pocket.derby namespace marker, logging cap, counters and sampling arrays on both sides; DEGRADE always fails even beyond log cap; no DERBY workload/math',
        log_budget='16 capped app log calls plus START, 4 summary lines and EXIT; expected <32KiB/256 lines including INFO boot',
        changed_files=modified,sha256={p:hashlib.sha256((dst/p).read_bytes()).hexdigest() for p in modified},patch_sha256=hashlib.sha256(diff).hexdigest())
    (dst/'diagnostic.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--checkout',type=Path,required=True);p.add_argument('--common',type=Path,required=True);a=p.parse_args();prepare(a.checkout.resolve(),a.common.resolve())
