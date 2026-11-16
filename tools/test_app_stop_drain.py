"""Compile actual app_stop control flow with bounded host dependency mocks.

No ESP scheduling, physical DMA, or full application integration claim.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / 'main/app_session.c').read_text()
start = text.index('static bool stop_pending,stop_prepared,stop_av_stopped;')
end = text.index('// ------------------------------------------------------ resident suspension', start)
body = text[start:end]
start = text.index('bool app_stop_keep_music(void) {')
end = text.index('\nbool app_can_suspend', start)
body += text[start:end]
pre = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#define KASANE_DMA_OVERLAP 1
#define ESP_LOGI(...) ((void)0)
typedef int ksn_result;
#define KSN_OK 0
#define KSN_IO 1
typedef struct {void *ctx;ksn_result (*finish)(void *);} ksn_display_completion;
typedef struct {unsigned sent_us;} kasane_display_t;
typedef struct {size_t struct_size;} pocketjs_guest_stats_t;
static pocketjs_guest_stats_t final_stats;
static int marker;
static void *guest=&marker;
static bool keep_music_once,overlay_session=true;
static char dormant_id[32]="sleeping";
static unsigned changes,hooks,destroys,heap_ends,drains;
static bool fail_drain=true,fail_reset=true;
static ksn_result kasane_finish(void *ctx){(void)ctx;drains++;return fail_drain?KSN_IO:KSN_OK;}
static ksn_result ksn_runtime_finish_display(const ksn_display_completion *c){return c->finish(c->ctx);}
static bool pocketjs_guest_dormant(void *g){(void)g;return true;}
static void pocketjs_guest_set_dormant(void *g,bool b){(void)g;(void)b;changes++;}
static void pocket_app_set_stop_reason(const char *s){(void)s;changes++;}
static void pocket_app_reset(void){changes++;hooks++;}
static bool pocket_av_reset(void){changes++;return true;}
static bool pocket_kasane_reset(void){return !fail_reset;}
static void pocket_av_output_source_reset(bool b){(void)b;changes++;}
static void pocketjs_guest_stats(void *g,pocketjs_guest_stats_t *s){(void)g;(void)s;}
static void pocketjs_guest_destroy(void *g){assert(g==&marker);destroys++;}
static void shiki_heap_end(void){heap_ends++;}
static void ksn_p0_probe_report(const char *s){(void)s;}
'''
noop = '''pocket_imu_reset pocket_av_detach_background pocket_capture_reset pocket_io_reset
pocket_net_reset pocket_fs_reset pocket_workspace_reset shiki_memstat_report
pocket_derby_reset pocket_clock_reset pocket_av_playback_source_reset pocket_input_reset
pocket_overlay_reset pocket_text_reset pocket_bridge_reset pocket_memory_reset
pocket_shiki_api_reset pocket_api_reset app_report shiki_memstat_reset'''.split()
pre += '\n'.join(f'static void {name}(void){{changes++;}}' for name in noop)
main = r'''
int main(void){
 assert(!app_stop_keep_music()&&app_stop_pending());
 assert(guest==&marker&&!changes&&!hooks&&!destroys&&!heap_ends&&keep_music_once);
 assert(dormant_id[0]=='s');
 assert(!app_stop()&&!changes&&drains==2);
 fail_drain=false;
 assert(!app_stop()&&app_stop_pending());
 assert(guest==&marker&&hooks==1&&!destroys&&!heap_ends);
 unsigned prepared=changes;
 assert(!app_stop()&&changes==prepared&&hooks==1);
 fail_reset=false;
 assert(app_stop()&&!app_stop_pending());
 assert(!guest&&hooks==1&&destroys==1&&heap_ends==1&&!keep_music_once);
 puts("PASS actual app_stop: failed drain no mutations; reset retry one hook; one destruction after success");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='app-stop-') as temp:
    src=Path(temp)/'test.c'; exe=Path(temp)/'test'
    src.write_text(pre+'\n'+body+'\n'+main)
    for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover']]:
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',*flags,str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
