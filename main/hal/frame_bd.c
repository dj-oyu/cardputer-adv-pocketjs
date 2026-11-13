// Per-frame breakdown probe: frame_bd.h.
//
// KASANE_PAINT averages render and send over painted frames and the turn over
// all ticks, so it cannot say what one frame() period is made of, nor how
// often a period runs late. This sums, per period: the JS turn, render and
// send (every present in it), the rest of app_tick (pumps, kasane advance,
// end_guest_turn), the rest of the UI loop, and the wait. Whether two cores
// could overlap JS with render+send depends on exactly these numbers.
//
// It also rotates the display pacing rule every window (frame_bd.h) so the
// three can be compared inside one image, and keeps a 1 ms histogram of the
// periods: an average cannot tell a steady 16.5 ms from 16/17 alternating.
#include "frame_bd.h"
#include <stdatomic.h>
#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
#include "esp_log.h"
#include "usb_console.h"
#include "disp.h"
#include "board.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WINDOW 60u
#define NMODES 3u
#define HIST 11u  /* <15, 15..23, >=24 ms */
static const char *const MODE_NAME[NMODES]={"old","noyield","deadline"};
/* POCKET_FRAME_BD_PRESS_AB: the block cache's pressure as measured, forced on,
 * forced off. POCKET_FRAME_BD_LOG_AB: whole-line log writes on/off. */
static const char *const PRESS_NAME[3]={"asis","on","off"};
static uint32_t cur_js,cur_rn,cur_sd,cur_tick;
static bool cur_press;
static uint64_t s_js,s_rn,s_sd,s_tick,s_held,s_period;
static uint32_t max_period,max_held,max_js,max_log,late16,late33,n,press,yields;
static uint16_t hist[HIST];
#ifdef CONFIG_POCKET_FRAME_BD_PACE_AB
static unsigned mode=FRAME_BD_PACE_OLD;
#else
static unsigned mode=FRAME_BD_PACE_NOYIELD;
#endif
static unsigned window,press_mode,log_whole=1;
static bool disp_task_on=true;
bool frame_bd_disp_on_task(void){return disp_task_on;}
/* P1: DISP_AB rotates inline, task at 5, task at 6 (input_task's priority:
 * equal priority does not preempt, so a poll waits for the task to block). */
static unsigned disp_mode=1;
/* POCKET_FRAME_BD_LCD_DIRECT_AB (BOARD_LCD_DIRECT builds): the LCD send path,
 * flipped every 3 windows so each display mode sees both. Starts direct, as
 * board.c does. */
#if defined(CONFIG_POCKET_FRAME_BD_LCD_DIRECT_AB)&&defined(BOARD_LCD_DIRECT)&&BOARD_LCD_DIRECT
static int lcd_direct=1;
#define LCD_PATH_NAME (lcd_direct?"direct":"queue")
#else
#define LCD_PATH_NAME (BOARD_LCD_DIRECT?"direct":"queue")
#endif
static const char *const DISP_NAME[3]={"inline","task5","task6"};
/* input_task runs on CPU 0 and this on CPU 1: atomics, read once a window. */
static _Atomic uint32_t inp_us,inp_n,inp_max;
void frame_bd_input(uint32_t us){
    atomic_fetch_add(&inp_us,us);atomic_fetch_add(&inp_n,1u);
    uint32_t m=atomic_load(&inp_max);
    while(us>m&&!atomic_compare_exchange_weak(&inp_max,&m,us)){}
}
/* P0: the turn's start, its first join point (0: none yet), the window's
 * sums, turns with no join point, joins per site, ineligible reasons. */
static int64_t turn_t0,turn_join;
static bool in_turn;
static uint64_t s_pre,s_post;
static uint32_t turns,no_join,site_n[FRAME_BD_JOIN_SITES],presents,eligible,why_n[6];
static uint32_t stack_min=UINT32_MAX;

void frame_bd_turn_begin(void){turn_t0=esp_timer_get_time();turn_join=0;in_turn=true;}
void frame_bd_join_point(unsigned site){
    if(!in_turn||site>=FRAME_BD_JOIN_SITES)return;
    site_n[site]++;
    if(!turn_join)turn_join=esp_timer_get_time();
}
void frame_bd_turn_end(void){
    if(!in_turn)return;
    in_turn=false;
    const int64_t end=esp_timer_get_time();
    turns++;
    if(!turn_join){no_join++;s_pre+=(uint64_t)(end-turn_t0);return;}
    s_pre+=(uint64_t)(turn_join-turn_t0);
    s_post+=(uint64_t)(end-turn_join);
}
void frame_bd_eligible(unsigned why){
    presents++;
    if(!why)eligible++;
    for(unsigned i=0;i<6;i++)if(why&(1u<<i))why_n[i]++;
}
void frame_bd_js(uint32_t us){cur_js+=us;}
void frame_bd_present(uint32_t r,uint32_t s){cur_rn+=r;cur_sd+=s;}
void frame_bd_tick(uint32_t us){cur_tick+=us;}
void frame_bd_log(uint32_t us){if(us>max_log)max_log=us;}
static uint32_t free_min=UINT32_MAX,largest_min=UINT32_MAX;
void frame_bd_pressure(bool on,bool native,uint32_t free_bytes,uint32_t largest){
    cur_press|=on;
    if(native){
        if(free_bytes<free_min)free_min=free_bytes;
        if(largest<largest_min)largest_min=largest;
    }
}
unsigned frame_bd_pace_mode(void){return mode;}
bool frame_bd_pressure_override(bool on){
    return press_mode==1?true:press_mode==2?false:on;
}
void frame_bd_yielded(void){yields++;}
static unsigned defer_n,hook_n,settle_n;
static uint64_t hook_us,settle_us;
void frame_bd_deferred(void){defer_n++;}
#define DEFER_WHY 17u
static unsigned defer_why[DEFER_WHY];
void frame_bd_defer_why(unsigned bits){
    for(unsigned i=0;i<DEFER_WHY;i++)if(bits&(1u<<i))defer_why[i]++;
}
void frame_bd_join_wait(bool hook,uint32_t us){
    if(hook){hook_n++;hook_us+=us;}else{settle_n++;settle_us+=us;}
}
void frame_bd_period(uint32_t held,uint32_t period){
    s_js+=cur_js;s_rn+=cur_rn;s_sd+=cur_sd;s_tick+=cur_tick;
    s_held+=held;s_period+=period;
    if(period>max_period)max_period=period;
    if(held>max_held)max_held=held;
    if(cur_js>max_js)max_js=cur_js;
    if(period>16700u)late16++;
    if(period>33400u)late33++;
    if(cur_press)press++;
    {
        const uint32_t ms=period/1000u;
        hist[ms<15u?0u:ms>=24u?HIST-1u:ms-14u]++;
    }
    cur_js=cur_rn=cur_sd=cur_tick=0;cur_press=false;
    if(++n<WINDOW)return;
    /* tick_other: app_tick minus js/render/send. loop_other: the UI loop
     * minus app_tick. wait: period minus the loop's work. */
    const double k=1.0/(n*1000.0);
    const double tick_other=(double)((int64_t)s_tick-(int64_t)(s_js+s_rn+s_sd))*k;
    ESP_LOGI("frame","FRAME_BD win=%u pace=%s press_mode=%s log_whole=%u js=%.2f rn=%.2f sd=%.2f tick_other=%.2f "
             "loop_other=%.2f wait=%.2f period=%.3f max_period=%.2f max_held=%.2f "
             "max_js=%.2f max_log=%.2f late16=%u late33=%u yields=%u press=%u "
             "hist=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u frames=%u",
             window,MODE_NAME[mode],PRESS_NAME[press_mode],log_whole,s_js*k,s_rn*k,s_sd*k,tick_other,
             (double)((int64_t)s_held-(int64_t)s_tick)*k,
             (double)((int64_t)s_period-(int64_t)s_held)*k,s_period*k,
             max_period/1000.0,max_held/1000.0,max_js/1000.0,max_log/1000.0,
             (unsigned)late16,(unsigned)late33,(unsigned)yields,(unsigned)press,
             hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],hist[6],hist[7],
             hist[8],hist[9],hist[10],(unsigned)n);
    /* P0: js_pre/js_post per turn (ms), joins per window by site, presents
     * that could overlap, ineligible reasons (cont/capture/text/notice/
     * resource/sound capture), and the UI task's minimum free stack. */
    /* A watermark since the task started, so once a window is enough: the
     * scan of the unused stack cost ~1 ms when it ran every frame. Words on
     * ESP-IDF's FreeRTOS are bytes. */
    stack_min=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("frame","FRAME_P0 win=%u js_pre=%.2f js_post=%.2f turns=%u no_join=%u "
             "commit=%u tx=%u eligible=%u/%u why=%u,%u,%u,%u,%u,%u stack_free=%u "
             "heap_free_min=%u heap_largest_min=%u disp=%s disp_stack_free=%u inp_ms=%.2f inp_n=%u inp_max=%u lcd_tx=%u lcd=%s "
             "defer=%u hook=%u hook_ms=%.2f settle=%u settle_ms=%.2f "
             "nodefer=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
             window,turns?s_pre/(turns*1000.0):0.0,turns?s_post/(turns*1000.0):0.0,
             (unsigned)turns,(unsigned)no_join,(unsigned)site_n[0],(unsigned)site_n[1],
             (unsigned)eligible,(unsigned)presents,(unsigned)why_n[0],(unsigned)why_n[1],
             (unsigned)why_n[2],(unsigned)why_n[3],(unsigned)why_n[4],(unsigned)why_n[5],
             (unsigned)stack_min,(unsigned)free_min,(unsigned)largest_min,
             DISP_NAME[disp_task_on?disp_mode:0u],(unsigned)disp_stack_free(),
             atomic_exchange(&inp_us,0u)/1000.0,(unsigned)atomic_exchange(&inp_n,0u),
             (unsigned)atomic_exchange(&inp_max,0u),(unsigned)board_lcd_tx_take(),LCD_PATH_NAME,
             defer_n,hook_n,hook_us/1000.0,settle_n,settle_us/1000.0,
             defer_why[0],defer_why[1],defer_why[2],defer_why[3],defer_why[4],defer_why[5],
             defer_why[6],defer_why[7],defer_why[8],defer_why[9],defer_why[10],defer_why[11],
             defer_why[12],defer_why[13],defer_why[14],defer_why[15],defer_why[16]);
    defer_n=hook_n=settle_n=0;hook_us=settle_us=0;
    for(unsigned i=0;i<DEFER_WHY;i++)defer_why[i]=0;
    free_min=largest_min=UINT32_MAX;
    s_pre=s_post=0;turns=no_join=presents=eligible=0;
    for(unsigned i=0;i<FRAME_BD_JOIN_SITES;i++)site_n[i]=0;
    for(unsigned i=0;i<6;i++)why_n[i]=0;
    stack_min=UINT32_MAX;
    s_js=s_rn=s_sd=s_tick=s_held=s_period=0;
    max_period=max_held=max_js=max_log=late16=late33=n=press=yields=0;
    for(unsigned i=0;i<HIST;i++)hist[i]=0;
    window++;
#ifdef CONFIG_POCKET_FRAME_BD_PACE_AB
    mode=(mode+1u)%NMODES;
#endif
#ifdef CONFIG_POCKET_FRAME_BD_DISP_AB
    disp_mode=(disp_mode+1u)%3u;
    disp_task_on=disp_mode!=0u;
    if(disp_task_on)disp_set_priority(disp_mode==1u?5u:6u);
#endif
#if defined(CONFIG_POCKET_FRAME_BD_LCD_DIRECT_AB)&&defined(BOARD_LCD_DIRECT)&&BOARD_LCD_DIRECT
    /* Between frames on the UI task; P1 joins the display task at once, so
     * nothing is in flight that board_lcd_direct_set would not reap. */
    if(window%3u==0u)lcd_direct=board_lcd_direct_set(!lcd_direct);
#endif
#ifdef CONFIG_POCKET_FRAME_BD_PRESS_AB
    press_mode=(press_mode+1u)%3u;
#endif
#ifdef CONFIG_POCKET_FRAME_BD_LOG_AB
    /* Flipped every 3 windows so each press mode sees both. */
    if(window%3u==0u){log_whole^=1u;usb_console_log_whole(log_whole!=0);}
#endif
}
#endif
