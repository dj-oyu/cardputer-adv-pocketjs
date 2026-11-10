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
#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
#include "esp_log.h"
#include "usb_console.h"
#include "disp.h"
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
             "heap_free_min=%u heap_largest_min=%u disp=%s disp_stack_free=%u",
             window,turns?s_pre/(turns*1000.0):0.0,turns?s_post/(turns*1000.0):0.0,
             (unsigned)turns,(unsigned)no_join,(unsigned)site_n[0],(unsigned)site_n[1],
             (unsigned)eligible,(unsigned)presents,(unsigned)why_n[0],(unsigned)why_n[1],
             (unsigned)why_n[2],(unsigned)why_n[3],(unsigned)why_n[4],(unsigned)why_n[5],
             (unsigned)stack_min,(unsigned)free_min,(unsigned)largest_min,
             disp_task_on?"task":"inline",(unsigned)disp_stack_free());
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
    disp_task_on=!disp_task_on;
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
