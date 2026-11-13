/* What the guest pays when an app first reads pocket.kasane
 * (docs/kasane/api-reorg.md sec.6 P0 item 4, the first row of sec.3.6).
 *
 * On the device pocket.kasane is a lazy namespace: the first read runs
 * build_kasane (main/pocket/pocket_kasane.c), which registers the classes,
 * the functions, cache, and the grid, video and pixel members (grid also puts
 * gridFold on the global object), and kasane.shiki when the session has a
 * Shiki heap. The host's pocket_api_lazy (tools/hostshim/pocket_api_stub.c)
 * runs that same build at install, so the guest heap just before and just
 * after pocket_kasane_install is the cost of the first read, without the
 * lazy getter itself. Function-list entries that QuickJS materialises on
 * first use are not built yet in either case.
 *
 * The heap is charged as the device's TLSF would be (request rounded up to
 * 4 B, 12 B minimum: the model of tools/games/test_lcd_catch_host.c), after a
 * GC on both sides. Then grid (with the global gridFold), video and pixel are
 * each installed alone on a fresh object in a fresh session: their share.
 * kasane.shiki's is the difference between the two sessions.
 *
 * Build and run (WSL/Linux; M32=1 for the device's 8 B JSValue and 4 B
 * pointers, the figure to quote):
 *   bash tools/build_kasane_ns_cost_test.sh && /tmp/test-kasane-ns-cost
 *   M32=1 bash tools/build_kasane_ns_cost_test.sh && /tmp/test-kasane-ns-cost-m32
 */
#include "pocket_kasane.h"
#include "pocket_av.h"
#include "pocket_video_sd_stream.h"
#include "shiki_heap.h"
#include "ksn_shiki.h"
#include "pocket_grid.h"
#include "pocket_video.h"
#include "pocket_pixel.h"
#include "system/sys_device.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr){__real_free(ptr);}
void *__wrap_calloc(size_t count,size_t size){return __real_calloc(count,size);}
/* pocket.kasane.video's SD stream: never started here. */
pocket_video_sd_stream *pocket_video_sd_stream_start(const char *p,uint16_t w,uint16_t h,const char **code){
    (void)p;(void)w;(void)h;if(code)*code="UNSUPPORTED";return NULL;}
void pocket_video_sd_stream_clock(pocket_video_sd_stream *s,uint64_t c){(void)s;(void)c;}
void pocket_video_sd_stream_pause(pocket_video_sd_stream *s,bool p){(void)s;(void)p;}
bool pocket_video_sd_stream_borrow(pocket_video_sd_stream *s,const uint8_t **p,uint32_t *b,uint64_t *t){
    (void)s;(void)p;(void)b;(void)t;return false;}
void pocket_video_sd_stream_release(pocket_video_sd_stream *s){(void)s;}
int pocket_video_sd_stream_state(const pocket_video_sd_stream *s){(void)s;return -1;}
bool pocket_video_sd_stream_stop(pocket_video_sd_stream *s){(void)s;return true;}
void pocket_video_sd_stream_reap(void){}
/* gridFold is a lazy global evaluated from apps/kasane/grid_fold.js when
 * first read, so its source is not part of the first read of pocket.kasane
 * (as tools/kasane_contract/test_megademo_app_host.c stands it in). */
const char grid_fold_js_start[] asm("_binary_grid_fold_js_start") = "globalThis.gridFold={};";
const char grid_fold_js_end[] asm("_binary_grid_fold_js_end") = "";
/* No built-in Shiki programs: register() is not called here. */
const ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *count){*count=0;return NULL;}

typedef struct { size_t n; size_t pad[3]; } pk_hdr;
static size_t cur_bytes,cur_blocks;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;cur_bytes+=tlsf_len(n);cur_blocks++;return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;cur_bytes-=tlsf_len(h->n);cur_blocks--;free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    q->n=n;cur_bytes+=tlsf_len(n);cur_bytes-=tlsf_len(old);return q+1;}
static size_t pk_usable(const void *p){return p?tlsf_len(((const pk_hdr *)p-1)->n):0;}
static const JSMallocFunctions PEAK_MF={pk_calloc,pk_malloc,pk_free,pk_realloc,pk_usable};

typedef struct { size_t bytes,blocks; int64_t obj,prop,shape,atom,cfunc; } sample;
static sample take(JSRuntime *rt){
    JS_RunGC(rt);
    JSMemoryUsage u;JS_ComputeMemoryUsage(rt,&u);
    return (sample){cur_bytes,cur_blocks,u.obj_count,u.prop_count,u.shape_count,u.atom_count,u.c_func_count};
}
static void print(const char *what,sample a,sample b){
    printf("%-34s %+6ld B in %+4ld blocks  (objects %+ld, properties %+ld, shapes %+ld, atoms %+ld, C functions %+ld)\n",
           what,(long)(b.bytes-a.bytes),(long)(b.blocks-a.blocks),(long)(b.obj-a.obj),(long)(b.prop-a.prop),
           (long)(b.shape-a.shape),(long)(b.atom-a.atom),(long)(b.cfunc-a.cfunc));
}

static unsigned fails;
static void session(bool shiki){
    if(shiki&&!shiki_heap_begin(0)){printf("FAIL: shiki_heap_begin\n");fails++;return;}
    JSRuntime *rt=JS_NewRuntime2(&PEAK_MF,NULL);
    JSContext *ctx=JS_NewContext(rt);
    if(!ctx||JS_PocketHarden(ctx)<0){printf("FAIL: context\n");fails++;return;}
    host_capabilities_clear();
    const sample s0=take(rt);
    if(pocket_kasane_install(ctx,NULL)!=ESP_OK){printf("FAIL: install\n");fails++;}
    const sample s1=take(rt);
    printf("%s\n",shiki?"session with a Shiki heap (the manifest names kasane.shiki):":"session without a Shiki heap:");
    print("  pocket.kasane, first read",s0,s1);
    pocket_kasane_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(shiki)shiki_heap_end();
}

/* One member alone, installed on a fresh object of a fresh session: what it
 * adds to every first read (grid also defines the lazy global gridFold). */
static void member(const char *name,esp_err_t (*install)(JSContext *,JSValueConst)){
    JSRuntime *rt=JS_NewRuntime2(&PEAK_MF,NULL);
    JSContext *ctx=JS_NewContext(rt);
    if(!ctx||JS_PocketHarden(ctx)<0){printf("FAIL: context\n");fails++;return;}
    host_capabilities_clear();
    JSValue ns=JS_NewObject(ctx);
    const sample s0=take(rt);
    if(install(ctx,ns)!=ESP_OK){printf("FAIL: %s install\n",name);fails++;}
    const sample s1=take(rt);
    char what[64];snprintf(what,sizeof what,"  of which %s",name);
    print(what,s0,s1);
    JS_FreeValue(ctx,ns);
    pocket_grid_reset();pocket_video_reset();pocket_pixel_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
}

int main(void){
    printf("guest heap, TLSF model, %u-byte pointers, %u-byte JSValue\n",(unsigned)sizeof(void *),(unsigned)sizeof(JSValue));
    session(false);
    session(true);
    printf("the members, each alone:\n");
    member("grid (and gridFold)",pocket_grid_install);
    member("video",pocket_video_install);
    member("pixel",pocket_pixel_install);
    printf("%s\n",fails?"KASANE_NS_COST FAIL":"KASANE_NS_COST PASS");
    return fails?1:0;
}
