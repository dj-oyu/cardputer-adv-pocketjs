#include "shiki_heap.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
static void *guest,*owner,*finalizer_block;
static bool keep_music_once,overlay_session;
static char dormant_id[8];
static int stage,destroyed,output_calls;
static bool av_ok,kasane_ok;
typedef struct {size_t struct_size;} pocketjs_guest_stats_t;
static pocketjs_guest_stats_t final_stats;
#define ESP_LOGI(...) ((void)0)
#define NOOP(name) static void name(void){}
NOOP(pocket_imu_reset) NOOP(pocket_av_detach_background)
NOOP(pocket_capture_reset) NOOP(pocket_io_reset) NOOP(pocket_net_reset)
NOOP(pocket_fs_reset) NOOP(pocket_workspace_reset) NOOP(shiki_memstat_report)
NOOP(pocket_derby_reset) NOOP(pocket_clock_reset) NOOP(pocket_av_playback_source_reset)
NOOP(pocket_input_reset) NOOP(pocket_overlay_reset) NOOP(pocket_text_reset)
NOOP(pocket_bridge_reset) NOOP(pocket_memory_reset) NOOP(app_report)
NOOP(shiki_memstat_reset)
static bool pocketjs_guest_dormant(void *p){(void)p;return false;}
static void pocketjs_guest_set_dormant(void *p,bool b){(void)p;(void)b;}
static void pocket_app_set_stop_reason(const char *s){(void)s;}
static void pocket_app_reset(void){assert(stage==0);stage=1;}
static bool pocket_av_reset(void){return av_ok;}
static bool pocket_kasane_reset(void){return kasane_ok;}
static void pocket_av_output_source_reset(bool b){assert(kasane_ok && b==av_ok);output_calls++;}
static void pocket_shiki_api_reset(void){assert(stage==1);if(owner)shiki_heap_free(owner);owner=NULL;stage=2;}
static void pocket_api_reset(void){assert(stage==2);stage=3;}
static void pocketjs_guest_stats(void *g,pocketjs_guest_stats_t *s){assert(g&&s&&stage==3);}
static void pocketjs_guest_destroy(void *g){assert(g&&stage==3&&shiki_heap_active());if(finalizer_block)shiki_heap_free(finalizer_block);finalizer_block=NULL;destroyed++;}
static void ksn_p0_probe_report(const char *s){(void)s;}
#include "stop.inc"
static void stop(void){stage=0;app_stop();assert(!guest);}
int main(void){
 for(int av=0;av<2;av++)for(int kasane=0;kasane<2;kasane++){
  av_ok=av;kasane_ok=kasane;output_calls=0;
  assert(shiki_heap_begin(49152));
  owner=shiki_heap_malloc(480);assert(owner);
  guest=(void*)1; finalizer_block=shiki_heap_malloc(96);assert(finalizer_block);
  void *pending=(!av_ok||!kasane_ok)?shiki_heap_malloc(32):NULL;
  if(!av_ok||!kasane_ok)assert(pending);
  stop();assert(!shiki_heap_active());assert(output_calls==kasane);
  if(pending){
   assert(!shiki_heap_begin(49152));
   // Repeated stop must not release an outstanding external owner.
   stop();assert(!shiki_heap_begin(49152));
   shiki_heap_free(pending);
  }
  assert(shiki_heap_begin(49152));
  unsigned b,u,l;shiki_heap_info(&b,&u,&l);assert(b==49152&&u==0&&l==49152);
  shiki_heap_end();stop();
 }
 assert(destroyed==4);
 puts("reset-return matrix: AV/Kasane 4 combinations, held owner, repeated stop, restart: PASS");
}
