/* Diagnostic checkout only; no additions to normal firmware. */
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned lcd_diag_alloc,lcd_diag_bytes,lcd_diag_realloc;
static unsigned diag_n,diag_tick_n,diag_ticks,diag_cont,diag_free_min=~0u;
static unsigned diag_us[120],diag_alloc,diag_bytes,diag_realloc,diag_modes[5];
static unsigned diag_a0,diag_b0,diag_r0;
static int64_t diag_begin,diag_window,diag_last,diag_tick_sum;
static bool diag_sampling,diag_done;
static int diag_cmp(const void *a,const void *b){unsigned x=*(const unsigned*)a,y=*(const unsigned*)b;return (x>y)-(x<y);}
bool lcd_diag_finished(void){return diag_done;}
bool lcd_diag_valid(void){return diag_n==120&&diag_modes[2]==84&&diag_modes[3]==36;}
void lcd_diag_owner_tick(unsigned us,bool continued){if(diag_sampling||diag_n>diag_tick_n){diag_ticks++;diag_tick_sum+=us;diag_cont+=continued;diag_tick_n=diag_n;}}
void lcd_diag_finish(const char *status){
    unsigned free_bytes=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    qsort(diag_us,diag_n,sizeof(*diag_us),diag_cmp);
    printf("LCD7_DIAG counts frames=%u owner_ticks=%u continuations=%u play=%u miss=%u pointer_bytes=%zu\n",diag_n,diag_ticks,diag_cont,diag_modes[2],diag_modes[3],sizeof(void*));
    printf("LCD7_DIAG js_frame_wall_us median_x2=%u p95_nearest_rank=%u max=%u window_us=%lld owner_tick_us=%lld\n",diag_n?diag_us[(diag_n-1)/2]+diag_us[diag_n/2]:0,diag_n?diag_us[(95*diag_n+99)/100-1]:0,diag_n?diag_us[diag_n-1]:0,(long long)(diag_last-diag_window),(long long)diag_tick_sum);
    printf("LCD7_DIAG guest_requests=%u requested_bytes=%u realloc_requests=%u boundary_free_min=%u free_end=%u\n",diag_alloc,diag_bytes,diag_realloc,diag_free_min,free_bytes);
    printf("LCD7_DIAG END status=%s frames=%u complete:%s\n",status,diag_n,!strcmp(status,"complete")?"true":"false");diag_sampling=false;
}
static JSValue js_lcd_diag(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;int32_t op=0,mode=0,score=0;
    if(argc<1||JS_ToInt32(ctx,&op,argv[0]))return JS_EXCEPTION;
    if(op==2){key_set(key_pressed,keymap_key_index("1"));return JS_UNDEFINED;}
    if(op==0){
        if(diag_n>=120||diag_done||diag_sampling)return JS_ThrowInternalError(ctx,"diagnostic frame limit");
        unsigned free_bytes=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);if(free_bytes<diag_free_min)diag_free_min=free_bytes;
        diag_a0=lcd_diag_alloc;diag_b0=lcd_diag_bytes;diag_r0=lcd_diag_realloc;
        diag_begin=esp_timer_get_time();if(!diag_n)diag_window=diag_begin;diag_sampling=true;
    }else if(op==1){
        const int64_t now=esp_timer_get_time();
        if(!diag_sampling||diag_n>=120)return JS_ThrowInternalError(ctx,"diagnostic sequence");
        if(argc<3||JS_ToInt32(ctx,&mode,argv[1])||JS_ToInt32(ctx,&score,argv[2]))return JS_EXCEPTION;
        if((mode!=2&&mode!=3)||score!=0)return JS_ThrowInternalError(ctx,"diagnostic input mismatch");
        diag_us[diag_n]=(unsigned)(now-diag_begin);diag_last=now;
        diag_alloc+=lcd_diag_alloc-diag_a0;diag_bytes+=lcd_diag_bytes-diag_b0;diag_realloc+=lcd_diag_realloc-diag_r0;
        ++diag_modes[mode];++diag_n;diag_sampling=false;if(diag_n==120)diag_done=true;
    }else return JS_ThrowInternalError(ctx,"diagnostic operation");return JS_UNDEFINED;
}
