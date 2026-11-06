"""Exercise the exact diagnostic wrapper source with host ESP/JS boundary stubs.
This is not app_session/FreeRTOS execution or a target timing test.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[3]
source=(ROOT/'main/pocket/pocket_shiki.c').read_text()
block=source[source.index('/* One native step per ordinary guest frame'):source.index('\n#endif\n\nesp_err_t pocket_shiki_debug_fill')]
prefix=r'''
#include "ui/kasane/ksn_mesh_q7_preflight.h"
#include "ui/kasane/ksn_mesh_q7_workspace.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int JSValue;typedef int JSValueConst;typedef int JSContext;
enum {MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2};
static bool done,ok,oom;static unsigned active;static size_t available=100000;
static unsigned char *base,*raw_pointer;static int64_t now,increment=10;
static JSValue JS_NewObject(JSContext*c){(void)c;done=false;ok=false;return 1;}
static bool JS_IsException(JSValue v){(void)v;return false;}
static JSValue JS_NewBool(JSContext*c,bool v){(void)c;return v;}
static void JS_SetPropertyStr(JSContext*c,JSValue o,const char*k,JSValue v){(void)c;(void)o;if(!strcmp(k,"done"))done=v;if(!strcmp(k,"ok"))ok=v;}
static void q7_num(JSContext*c,JSValue o,const char*k,double n){(void)c;(void)o;(void)k;(void)n;}
static void q7_heap(JSContext*c,JSValue o){(void)c;(void)o;}
static void q7_stats(JSContext*c,JSValue o,const ksn_mesh_q7_stats*s){(void)c;(void)o;(void)s;}
static size_t heap_caps_get_free_size(unsigned c){(void)c;return available;}
static size_t heap_caps_get_largest_free_block(unsigned c){(void)c;return available;}
static unsigned uxTaskGetStackHighWaterMark(void*p){(void)p;return 16000;}
static void *heap_caps_malloc(size_t n,unsigned c){(void)c;if(oom)return NULL;assert(!active);base=malloc(n+8);assert(base);raw_pointer=base+4;active++;return raw_pointer;}
static void heap_caps_free(void*p){if(!p)return;assert(active==1&&p==raw_pointer);free(base);active--;base=raw_pointer=NULL;}
static int64_t esp_timer_get_time(void){now+=increment;return now;}
'''
suffix=r'''
static void call(void){assert(js_mesh_q7_preflight(NULL,0,0,NULL)==1);}
int main(void){
 for(unsigned i=0;i<3376;i++){
  call();assert(ok);assert(done==(i==3375));
  if(!done){assert(active==1);assert((uintptr_t)q7_preflight_state.workspace%ksn_mesh_q7_preflight_alignment()==0);}
 }
 assert(!active&&q7_preflight_state.report.checkpoints==3376);call();assert(done&&ok&&!active);
 pocket_shiki_debug_reset();assert(!active);
 for(unsigned i=0;i<8;i++){call();assert(!done&&ok&&active==1);}pocket_shiki_debug_reset();assert(!active);
 available=0;call();assert(done&&!ok&&!active);pocket_shiki_debug_reset();available=100000;
 oom=true;call();assert(done&&!ok&&!active);pocket_shiki_debug_reset();oom=false;
 call();assert(!done&&active);now+=180000001;call();assert(done&&!ok&&!active);pocket_shiki_debug_reset();
 increment=30000;call();assert(done&&!ok&&!active);pocket_shiki_debug_reset();
 puts("Q7_DEBUG_LIFECYCLE_PASS deliberately4aligned allocations,3376turns,reset,OOM,admission,total/step stop; host boundary stubs only");
}
'''
with tempfile.TemporaryDirectory(prefix='q7-debug-') as d:
 p=Path(d)/'test.c';p.write_text(prefix+block+suffix);binary=Path(d)/'test'
 sources=['main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_mesh_depth.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_prepared.c','main/ui/kasane/ksn_mesh_q7_certified.c','main/ui/kasane/ksn_mesh_q7_preflight.c']
 subprocess.run(['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fno-fast-math','-ffp-contract=off','-frounding-math','-fsanitize=address,undefined','-I'+str(ROOT/'main'),str(p),*[str(ROOT/s) for s in sources],'-lm','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
