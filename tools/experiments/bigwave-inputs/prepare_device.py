"""Finite BIG WAVE diagnostic in a disposable clean checkout; never accesses hardware."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];TEMPLATE=Path(__file__).with_name('device_diag');WORKLOAD='bigwave-inputs-ride-120-v1'
def change(s,a,b):
 assert s.count(a)==1,a
 return s.replace(a,b)
def input_source(s):
 s=change(s,'    keystate_snapshot(&now);','    keystate_snapshot(&now);\n    memset(&now,0,sizeof now); /* diagnostic empty physical input */')
 s=change(s,'// held/pressed/released share everything', (TEMPLATE/'runtime.inc').read_text()+'\n// held/pressed/released share everything')
 return change(s,'    (void)user;\n    JS_DefinePropertyValueStr(ctx,ns,"onAction",','    (void)user;\n    JS_DefinePropertyValueStr(ctx,ns,"_diag",JS_NewCFunction(ctx,js_inputs_diag,"_diag",3),0);\n    JS_DefinePropertyValueStr(ctx,ns,"onAction",')
def prepare(dst):
 assert dst!=ROOT
 assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=dst,text=True)
 head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=dst,text=True).strip();modified=[]
 def read(p):return (dst/p).read_text()
 def write(p,s):(dst/p).write_text(s);modified.append(p)
 write('main/pocket/pocket_input.c',input_source(read('main/pocket/pocket_input.c')))
 s=read('components/pocketjs_guest/src/guest.c');wrappers='''extern unsigned inputs_diag_alloc,inputs_diag_bytes,inputs_diag_realloc;
static void *diag_malloc(void *p,size_t n){++inputs_diag_alloc;inputs_diag_bytes+=n;return guest_malloc(p,n);}
static void *diag_calloc(void *p,size_t c,size_t n){++inputs_diag_alloc;inputs_diag_bytes+=c*n;return guest_calloc(p,c,n);}
static void *diag_realloc(void *p,void *v,size_t n){++inputs_diag_realloc;inputs_diag_bytes+=n;return guest_realloc(p,v,n);}
'''
 s=change(s,'static const JSMallocFunctions GUEST_ALLOCATOR = {',wrappers+'\nstatic const JSMallocFunctions GUEST_ALLOCATOR = {')
 for n in ('malloc','calloc','realloc'):s=change(s,f'.js_{n} = guest_{n},',f'.js_{n} = diag_{n},')
 write('components/pocketjs_guest/src/guest.c',s)
 owner=(TEMPLATE/'owner.inc').read_text().replace('@WORKLOAD@',WORKLOAD).replace('@SOURCE@',head)
 s=change(read('main/main.c'),'static void ui_task(void *arg) {',owner+'\nstatic void ui_task(void *arg) {')
 s=change(s,'    while(1) {\n        /* App teardown','    while(1) {\n        diag_owner_step();\n        /* App teardown')
 s=change(s,'            tick_run(have,&stroke);','''            const int64_t diag_start=esp_timer_get_time();
            tick_run(diag_owner_active?false:have,&stroke);
            if(diag_owner_active)inputs_diag_owner_tick((unsigned)(esp_timer_get_time()-diag_start),app_turn_continued());''')
 write('main/main.c',s)
 write('apps/bigwave/big_wave.js',read('apps/bigwave/big_wave.js')+'\n'+(TEMPLATE/'workload.js').read_text())
 diff=subprocess.check_output(['git','diff','--binary'],cwd=dst);(dst/'diagnostic.patch').write_bytes(diff)
 manifest=dict(source=head,workload=WORKLOAD,setup_frames=82,measured_frames=120,storage='app does not use storage; no diagnostic storage hooks',allocator='production GUEST_ALLOCATOR delegated unchanged via request counters',changed_files=modified,sha256={p:hashlib.sha256((dst/p).read_bytes()).hexdigest() for p in modified},patch_sha256=hashlib.sha256(diff).hexdigest())
 (dst/'diagnostic.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--checkout',type=Path,required=True);a=p.parse_args();prepare(a.checkout.resolve())
