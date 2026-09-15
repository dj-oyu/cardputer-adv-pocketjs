// Production app_tick/run_pumps/dispatch_guest with deterministic host ports.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=-2,KSN_INPUT_APP,KSN_INPUT_BLOCKED};
typedef struct {size_t struct_size;uint32_t buttons,analog;const uint32_t *touches;
    const int32_t *touch_hits;size_t touch_count;} pocketjs_guest_frame_t;
typedef struct {size_t struct_size;} pocketjs_ui_frame_view_t;
static void *guest,*core;
static bool turn_continued,pending,remain,active,activate,need_present,submission;
static bool runaway,exit_requested,stopped;
static bool kasane_session;
static unsigned continuation_turns,ticks;
static uint32_t deferred_buttons,delivered,armed;
static int scope,guest_error,draw_error;
static int64_t now,last_present_us;
static double turn_sum;
#define VM_DISPLAY_PERIOD_MS 33
static char calls[1024];
static void call(char c){size_t n=strlen(calls);assert(n+1<sizeof(calls));calls[n]=c;calls[n+1]=0;}
static int64_t esp_timer_get_time(void){return now;}
static void arm_turn(uint32_t b){armed=b;call('A');}
static bool pocketjs_guest_jobs_pending(void *g){(void)g;return pending;}
static int pocketjs_guest_continue(void *g){(void)g;call('C');pending=remain;return guest_error;}
static int pocketjs_guest_frame(void *g,const pocketjs_guest_frame_t *f){
    (void)g;assert(f->struct_size==sizeof(*f)&&f->analog==0x8080);
    assert(!f->touch_count&&!f->touches&&!f->touch_hits);
    delivered=f->buttons;call('F');if(activate)active=true;return guest_error;
}
static void pocketjs_ui_core_tick(void *c){(void)c;call('T');}
static int pocketjs_ui_core_draw(void *c,pocketjs_ui_frame_view_t *f){
    (void)c;assert(f->struct_size==sizeof(*f));call('D');return draw_error;
}
static bool pocket_kasane_active(void){return active;}
static bool pocket_kasane_needs_present(void){return need_present;}
static bool pocket_kasane_has_submission(void){return submission;}
static int pocket_kasane_input_scope(bool b){(void)b;return scope;}
static void pocket_kasane_end_turn(void){call('E');}
static void report_oom_if_any(void){call('O');}
static bool drain_runaway(void){return runaway;}
static bool pocket_app_exit_requested(void){return exit_requested;}
static void app_request_stop(void){stopped=true;call('S');}
static int present_frame(pocketjs_ui_frame_view_t *f){(void)f;call('P');need_present=false;return 0;}
#define PUMP(name,c) static void name(void){call(c);}
PUMP(pocket_app_pump,'a') PUMP(pocket_text_pump,'t') PUMP(pocket_imu_pump,'i')
PUMP(pocket_io_pump,'o') PUMP(pocket_bridge_pump,'b') PUMP(pocket_net_pump,'n')
PUMP(pocket_capture_pump,'c') PUMP(pocket_api_pump,'p') PUMP(pocket_fs_pump,'f')
PUMP(pocket_av_pump,'v') PUMP(pocket_ui_pump,'u')
static void pocket_input_pump(uint32_t b){delivered=b;call('k');}
#include "session_dispatch_impl.inc"
static void reset(void){
    calls[0]=0;pending=remain=active=activate=need_present=submission=false;
    kasane_session=false;
    runaway=exit_requested=stopped=turn_continued=false;scope=KSN_INPUT_APP;
    deferred_buttons=delivered=armed=continuation_turns=ticks=0;
    guest_error=draw_error=0;now=40000;last_present_us=0;turn_sum=0;
}
int main(void){
    reset();assert(app_tick(0x4000)==0);assert(delivered==0x4000&&armed==0x4000);
    assert(!strcmp(calls,"AatiobncpfvukEFTDEOP"));
    reset();active=true;assert(app_tick(0)==0);assert(!strchr(calls,'T')&&!strchr(calls,'D'));
    reset();kasane_session=true;assert(app_tick(0)==0);assert(!active&&!strchr(calls,'T')&&!strchr(calls,'D'));
    reset();activate=true;assert(app_tick(0)==0);assert(active&&!strchr(calls,'T'));
    reset();guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(strstr(calls,"FEO")&&!strchr(calls,'T')&&!strchr(calls,'P'));
    reset();draw_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(strstr(calls,"FTDEO")&&!strchr(calls,'P'));
    reset();pending=remain=true;assert(app_tick(0x20)==0);
    assert(turn_continued&&!strchr(calls,'F')&&strstr(calls,"ACTDEO"));
#ifdef CONFIG_POCKET_VM_FAIR
    assert(strstr(calls,"atiobncpfvukE")&&delivered==0x20);
#else
    assert(!strchr(calls,'a')&&deferred_buttons==0x20);
#endif
    calls[0]=0;remain=false;assert(app_tick(0)==0);
    assert(strchr(calls,'C')<strchr(calls,'a')&&strchr(calls,'a')<strchr(calls,'F'));
#ifndef CONFIG_POCKET_VM_FAIR
    assert(delivered==0x20&&!deferred_buttons);
#endif
    reset();pending=remain=true;exit_requested=true;assert(app_tick(0)==0);assert(!stopped);
    reset();pending=remain=true;assert(app_tick(0x2000)==0);
    assert(delivered==0x2000&&strchr(calls,'F')&&!turn_continued);
    reset();pending=remain=true;runaway=true;assert(app_tick(0)==ESP_ERR_TIMEOUT);
    assert(!strchr(calls,'F')&&!strchr(calls,'P'));
    reset();pending=true;guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(!strcmp(calls,"ACEO"));
    reset();active=need_present=submission=true;assert(app_tick(0)==0);assert(!strcmp(calls,"P"));
    reset();active=need_present=true;assert(app_tick(0)==0);assert(calls[0]=='P'&&strchr(calls,'F'));
    reset();active=need_present=submission=true;assert(app_tick(0x2000)==0);assert(delivered==0x2000);
    reset();active=true;scope=KSN_INPUT_BLOCKED;assert(app_tick(0x4000)==0);assert(!delivered);
    reset();pending=remain=true;now=1000;assert(app_tick(0)==0);assert(!strchr(calls,'P'));
    puts("session dispatch PASS: ordering, cleanup, Back, continuation, backend, watchdog");
}
