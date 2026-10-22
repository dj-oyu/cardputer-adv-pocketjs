/* Generated diagnostic only; the app and allocator remain the product paths. */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
unsigned derby_diag_alloc,derby_diag_bytes,derby_diag_realloc;
unsigned derby_diag_views,derby_diag_ou,derby_diag_sqrt;
static unsigned diag_n,diag_ticks,diag_cont,diag_free_min=~0u;
static unsigned diag_us[120],diag_alloc,diag_bytes,diag_realloc;
static unsigned diag_a0,diag_b0,diag_r0,diag_v0,diag_o0,diag_s0;
static int64_t diag_begin,diag_window,diag_last,diag_tick_sum;
static bool diag_sampling,diag_done;
static double diag_lead_first,diag_lead_last;
static unsigned diag_views,diag_ou,diag_sqrt;
static int diag_cmp(const void *a,const void *b){unsigned x=*(const unsigned*)a,y=*(const unsigned*)b;return (x>y)-(x<y);}
bool derby_diag_finished(void){return diag_done;}
void derby_diag_owner_tick(unsigned us,bool continued){
    if(diag_sampling){diag_ticks++;diag_tick_sum+=us;diag_cont+=continued;}
}
void derby_diag_finish(const char *status){
    unsigned free_bytes=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    qsort(diag_us,diag_n,sizeof(*diag_us),diag_cmp);
    printf("DERBY_APP_DIAG counts frames=%u owner_ticks=%u continuations=%u views=%u ou=%u sqrt=%u\n",diag_n,diag_ticks,diag_cont,diag_views,diag_ou,diag_sqrt);
    printf("DERBY_APP_DIAG frame_wall_us median=%u p95=%u max=%u window_us=%lld owner_tick_us=%lld\n",diag_n?diag_us[diag_n/2]:0,diag_n?diag_us[(diag_n-1)*95/100]:0,diag_n?diag_us[diag_n-1]:0,(long long)(diag_last-diag_window),(long long)diag_tick_sum);
    printf("DERBY_APP_DIAG guest_requests=%u requested_bytes=%u realloc_requests=%u boundary_free_min=%u free_end=%u lead=%.9f..%.9f\n",diag_alloc,diag_bytes,diag_realloc,diag_free_min,free_bytes,diag_lead_first,diag_lead_last);
    printf("DERBY_APP_DIAG END status=%s frames=%u\n",status,diag_n);
    diag_sampling=false;
}
static JSValue js_diag(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;int32_t op=0;double lead=0;
    if(argc<1||JS_ToInt32(ctx,&op,argv[0]))return JS_EXCEPTION;
    if(op==0){
        if(diag_n>=120||diag_done)return JS_ThrowInternalError(ctx,"diagnostic frame limit");
        unsigned free_bytes=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(free_bytes<diag_free_min)diag_free_min=free_bytes;
        diag_a0=derby_diag_alloc;diag_b0=derby_diag_bytes;diag_r0=derby_diag_realloc;
        diag_v0=derby_diag_views;diag_o0=derby_diag_ou;diag_s0=derby_diag_sqrt;
        diag_begin=esp_timer_get_time();if(!diag_n)diag_window=diag_begin;
        diag_sampling=true;
    }else if(op==1){
        const int64_t now=esp_timer_get_time();
        if(!diag_sampling||diag_n>=120)return JS_ThrowInternalError(ctx,"diagnostic sequence");
        diag_us[diag_n]=(unsigned)(now-diag_begin);diag_last=now;
        diag_alloc+=derby_diag_alloc-diag_a0;diag_bytes+=derby_diag_bytes-diag_b0;diag_realloc+=derby_diag_realloc-diag_r0;
        diag_views+=derby_diag_views-diag_v0;diag_ou+=derby_diag_ou-diag_o0;diag_sqrt+=derby_diag_sqrt-diag_s0;
        if(argc<2||JS_ToFloat64(ctx,&lead,argv[1]))return JS_EXCEPTION;
        if(!diag_n)diag_lead_first=lead;
        diag_lead_last=lead;++diag_n;
        if(diag_n==120)diag_done=true;
    }else return JS_ThrowInternalError(ctx,"diagnostic operation");
    return JS_UNDEFINED;
}
