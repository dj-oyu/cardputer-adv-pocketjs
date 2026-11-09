// Per-frame breakdown probe: frame_bd.h.
//
// KASANE_PAINT averages render and send over painted frames and the turn over
// all ticks, so it cannot say what one frame() period is made of, nor how
// often a period runs late. This sums, per period: the JS turn, render and
// send (every present in it), the rest of app_tick (pumps, kasane advance,
// end_guest_turn), the rest of the UI loop, and the wait. Whether two cores
// could overlap JS with render+send depends on exactly these numbers.
#include "frame_bd.h"
#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
#include "esp_log.h"

#define WINDOW 60u
static uint32_t cur_js,cur_rn,cur_sd,cur_tick;
static uint64_t s_js,s_rn,s_sd,s_tick,s_held,s_period;
static uint32_t max_period,max_held,max_js,late16,late33,n;

void frame_bd_js(uint32_t us){cur_js+=us;}
void frame_bd_present(uint32_t r,uint32_t s){cur_rn+=r;cur_sd+=s;}
void frame_bd_tick(uint32_t us){cur_tick+=us;}
void frame_bd_period(uint32_t held,uint32_t period){
    s_js+=cur_js;s_rn+=cur_rn;s_sd+=cur_sd;s_tick+=cur_tick;
    s_held+=held;s_period+=period;
    if(period>max_period)max_period=period;
    if(held>max_held)max_held=held;
    if(cur_js>max_js)max_js=cur_js;
    if(period>16700u)late16++;
    if(period>33400u)late33++;
    cur_js=cur_rn=cur_sd=cur_tick=0;
    if(++n<WINDOW)return;
    /* tick_other: app_tick minus js/render/send. loop_other: the UI loop
     * minus app_tick. wait: period minus the loop's work. */
    const double k=1.0/(n*1000.0);
    const double tick_other=(double)((int64_t)s_tick-(int64_t)(s_js+s_rn+s_sd))*k;
    ESP_LOGI("frame","FRAME_BD js=%.2f rn=%.2f sd=%.2f tick_other=%.2f loop_other=%.2f "
             "wait=%.2f period=%.2f max_period=%.2f max_held=%.2f max_js=%.2f "
             "late16=%u late33=%u frames=%u",
             s_js*k,s_rn*k,s_sd*k,tick_other,(double)((int64_t)s_held-(int64_t)s_tick)*k,
             (double)((int64_t)s_period-(int64_t)s_held)*k,s_period*k,max_period/1000.0,
             max_held/1000.0,max_js/1000.0,(unsigned)late16,(unsigned)late33,(unsigned)n);
    s_js=s_rn=s_sd=s_tick=s_held=s_period=0;
    max_period=max_held=max_js=late16=late33=n=0;
}
#endif
