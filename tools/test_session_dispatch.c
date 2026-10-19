// Production app_tick/run_pumps/dispatch_guest with deterministic host ports.
// Keep production optional scopes compiled out with their actual OFF contract.
#include "../main/pocket/derby_measure.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef TEST_DERBY_DETAIL_EXIT
#define POCKET_DERBY_DETAIL_MEASURE
#define DD_RESUME() ((void)0)
#define DD_SUSPEND() ((void)0)
#include "../main/pocket/derby_fixed_workload.h"
static unsigned fixture_phase;
unsigned df_status(void){return fixture_phase;}
#endif
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=-2,KSN_INPUT_APP,KSN_INPUT_BLOCKED};
typedef int ksn_result;
enum { KSN_OK, KSN_BUSY };
typedef struct {size_t struct_size;uint32_t buttons,analog;const uint32_t *touches;
    const int32_t *touch_hits;size_t touch_count;} pocketjs_guest_frame_t;
typedef struct {int id;} sys_notice;
static void *guest;
static bool turn_continued,pending,remain,activate,need_present,submission;
static bool runaway,exit_requested,stopped,cont_submits;
static bool proc_commits,proc_pending;
static ksn_result presenter_result;
static atomic_bool stop_requested;
static unsigned continuation_turns,ticks;
static uint32_t deferred_buttons,delivered,armed;
static int scope,guest_error;
static int64_t now,last_present_us;
#ifdef TEST_DERBY_DETAIL_EXIT
static int64_t deadline,frame_elapsed,continuation_elapsed;
#endif
static double turn_sum;
#define VM_DISPLAY_PERIOD_MS 33
static char calls[1024];
static void call(char c){size_t n=strlen(calls);assert(n+1<sizeof(calls));calls[n]=c;calls[n+1]=0;}
static int64_t esp_timer_get_time(void){return now;}
static void arm_turn(uint32_t b){armed=b;call('A');
#ifdef TEST_DERBY_DETAIL_EXIT
    deadline=now+250000;
#endif
}
static bool pocketjs_guest_work_pending(void *g){(void)g;return pending;}
/* cont_submits: the continuation finishes a frame() the VM parked mid-call,
 * and that frame() submits its picture (backlog R3a). jobs_left: as in
 * guest.c, whether work remains after a resume is JS_IsJobPending(), read
 * whether or not the resumed frame() threw -- a failed continuation can
 * still leave jobs queued. */
static bool jobs_left;
static int pocketjs_guest_continue(void *g){
    (void)g;call('C');pending=remain||jobs_left;
    const bool finished=!remain&&!guest_error;
#ifdef TEST_DERBY_DETAIL_EXIT
    now+=continuation_elapsed;
#endif
    if(proc_commits&&finished)proc_pending=true;
    if(cont_submits&&finished)submission=need_present=true;
    return guest_error;
}
static bool frame_parks;
#ifdef TEST_DERBY_DETAIL_EXIT
static bool finish_in_frame;
#endif
/* What frame()'s first beginFrame()/patch() would meet: BUSY if the finished
 * frame's pending procedural frame or ticket was not presented first. */
static bool frame_met_pending;
/* A procedural commit() marks its frame pending; present_frame() clears it. */
static bool pocket_proc_pending(void){return proc_pending;}
static int pocketjs_guest_frame(void *g,const pocketjs_guest_frame_t *f){
    (void)g;frame_met_pending=proc_pending||submission;if(frame_parks)pending=true;
    assert(f->struct_size==sizeof(*f)&&f->analog==0x8080);
    assert(!f->touch_count&&!f->touches&&!f->touch_hits);
    delivered=f->buttons;call('F');if(activate)submission=need_present=true;
#ifdef TEST_DERBY_DETAIL_EXIT
    if(finish_in_frame){fixture_phase=DF_DONE;exit_requested=true;}
    now+=frame_elapsed;
    if(atomic_load(&stop_requested))return ESP_FAIL;
#endif
    return guest_error;
}
static void *sys_device_notifications(void){return NULL;}
static bool sys_notify_active(void *n,sys_notice *out){(void)n;(void)out;return false;}
static uint16_t pet_hub_selected(void){return 0;}
static int pocket_kasane_update_notice(const sys_notice *n,uint16_t pet){(void)n;(void)pet;return KSN_OK;}
static bool pocket_kasane_needs_present(void){return need_present;}
static bool pocket_kasane_has_submission(void){return submission;}
static void pocket_kasane_set_animation_time(uint64_t now){(void)now;}
static bool native_animation,system_pending;
// Actual scope reset/claim functions, without printing protocol text in tests.
#define printf(...) ((void)0)
#include "capture_scope_impl.inc"
#undef printf
static bool pocket_kasane_animation_pending(void){return native_animation;}
static bool pocket_kasane_system_pending(void){return system_pending;}
static int pocket_kasane_input_scope(bool b){(void)b;return scope;}
static void pocket_kasane_end_turn(void){call('E');}
/* 'Y': the host turn ended with the guest's turn still open (parked frame()
 * or a cut drain), so the procedural frame under construction is kept. */
static void pocket_kasane_park_turn(void){call('Y');}
static void report_oom_if_any(void){call('O');}
static bool drain_runaway(void){return runaway;}
static bool pocket_app_exit_requested(void){return exit_requested;}
static void app_request_stop(void){stopped=true;atomic_store(&stop_requested,true);call('S');}
static int present_frame(void){call('P');need_present=false;submission=false;proc_pending=false;return 0;}
/* Silent: the call strings below predate these, and none of them decides an
 * ordering this test checks. The presenter step can be made to fail. */
static void sample_memory_pressure(void){}
static ksn_result pocket_kasane_presenter_step(bool *blocked){*blocked=false;return presenter_result;}
#define ESP_LOGE(tag,...) ((void)(tag))
enum {KSN_P0_APP_TURN};
static void ksn_p0_probe_sample(int which,uint32_t us){(void)which;(void)us;}
static void pocket_memory_pump(bool leaving){(void)leaving;}
#define PUMP(name,c) static void name(void){call(c);}
PUMP(pocket_app_pump,'a') PUMP(pocket_text_pump,'t') PUMP(pocket_imu_pump,'i')
PUMP(pocket_io_pump,'o') PUMP(pocket_bridge_pump,'b') PUMP(pocket_net_pump,'n')
PUMP(pocket_capture_pump,'c') PUMP(pocket_api_pump,'p') PUMP(pocket_fs_pump,'f')
PUMP(pocket_av_pump,'v')
/* Silent, like the pumps added after the call strings were fixed. */
static void pocket_video_sd_stream_reap(void){}
static void pocket_input_pump(uint32_t b){delivered=b;call('k');}
#ifdef TEST_DERBY_DETAIL_EXIT
#include "detail_interrupt_impl.inc"
#endif
#include "session_dispatch_impl.inc"
#ifdef TEST_DERBY_DETAIL_EXIT
// Actual owner tick_run(), with UI/I/O ports quiet and teardown observable.
typedef int board_key_t;
enum {SCREEN_HOME=0,KEY_NONE=0,KEY_BACK,KEY_ENTER,KEY_UP,KEY_RIGHT,KEY_DOWN,KEY_LEFT};
typedef struct {board_key_t nav;} keystroke_t;
static int owner;
static atomic_bool owner_capture;
static bool ended,running;static int ended_result,run_started;
static bool exit_write_ok=true;
static bool df_detail_exit_record(void){call('R');return exit_write_ok;}
static const char *home_error;
typedef struct {void (*run_restore)(void);void (*ended)(esp_err_t,const char*);} screen_ops_t;
static const screen_ops_t SCREENS[1]={{0}};
static void *keys;
static void xQueueReset(void *p){(void)p;}
static void app_stop(void){ended=true;guest=NULL;call('T');}
static const char *app_error(void){return "APP EXITED";}
static void take_pending_run(void){}
static void sound_play(int i){(void)i;}
static void home_ready(const char *tag){assert(!strcmp(tag,"shell"));call('H');}
#define ESP_LOGI(tag,...) home_ready(tag)
#include "detail_end_run_impl.inc"
static void observed_end_run(esp_err_t e){ended_result=e;call('X');end_run(e);}
#define end_run observed_end_run
#define FALSE_PORT(name) static bool name(void){return false;}
FALSE_PORT(pocket_workspace_modal) FALSE_PORT(sd_picker_modal) FALSE_PORT(file_picker_modal)
FALSE_PORT(pocket_workspace_modal_dirty) FALSE_PORT(sd_picker_modal_dirty) FALSE_PORT(file_picker_modal_dirty)
FALSE_PORT(pocket_text_active) FALSE_PORT(app_can_suspend) FALSE_PORT(pocket_capture_take_dirty)
FALSE_PORT(pocket_workspace_run_requested)
#define QUIET_PORT(name) static void name(void){}
QUIET_PORT(pocket_input_keys_withhold) QUIET_PORT(pocket_workspace_modal_draw)
QUIET_PORT(sd_picker_modal_draw) QUIET_PORT(file_picker_modal_draw)
QUIET_PORT(app_force_redraw) QUIET_PORT(suspend_run)
#define KEY_PORT(name) static void name(const keystroke_t *s){(void)s;}
KEY_PORT(pocket_workspace_modal_key) KEY_PORT(sd_picker_modal_key)
KEY_PORT(file_picker_modal_key) KEY_PORT(pocket_text_key)
static void app_force_redraw_bands(uint32_t b){(void)b;}
static uint32_t pocket_text_take_dirty(void){return 0;}
static esp_err_t app_suspend(void){return ESP_FAIL;}
typedef struct {const char *id;} registry_t;
static const registry_t registry={"local.derby"};
static const registry_t *app_registry_current(void){return &registry;}
#define APP_ID_WORK "local.work"
#define capture owner_capture
#include "detail_owner_impl.inc"
#undef end_run
#undef capture
#endif
static void reset(void){
    guest=(void*)1;atomic_store(&stop_requested,false);
#ifdef TEST_DERBY_DETAIL_EXIT
    fixture_phase=DF_OFF;finish_in_frame=ended=false;
    deadline=frame_elapsed=continuation_elapsed=0;
    ended_result=0;atomic_store(&owner_capture,false);owner=SCREEN_HOME;
    running=true;run_started=ESP_OK;home_error=NULL;
    exit_write_ok=true;
#endif
    calls[0]=0;pending=remain=activate=need_present=submission=false;
    native_animation=system_pending=false;board_capture(false);
    runaway=exit_requested=stopped=turn_continued=cont_submits=frame_parks=proc_commits=proc_pending=false;scope=KSN_INPUT_APP;
    jobs_left=frame_met_pending=false;
    presenter_result=KSN_OK;
    deferred_buttons=delivered=armed=continuation_turns=ticks=0;
    guest_error=0;now=40000;last_present_us=0;turn_sum=0;
}
int main(void){
    // An ordinary turn: pumps, frame(), then the Kasane present. There is no
    // UI core tick or draw between frame() and the present any more.
    reset();assert(app_tick(0x4000)==0);assert(delivered==0x4000&&armed==0x4000);
    assert(!strcmp(calls,"AatiobncpfvkEFEOP"));
    reset();activate=true;assert(app_tick(0)==0);assert(submission||strchr(calls,'P'));
    reset();guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(strstr(calls,"FEO")&&!strchr(calls,'P'));
    reset();pending=remain=true;assert(app_tick(0x20)==0);
    assert(turn_continued&&!strchr(calls,'F')&&strstr(calls,"ACYO"));
    assert(!strchr(calls,'a')&&deferred_buttons==0x20);
    calls[0]=0;remain=false;assert(app_tick(0)==0);
    assert(strchr(calls,'C')<strchr(calls,'a')&&strchr(calls,'a')<strchr(calls,'F'));
    assert(delivered==0x20&&!deferred_buttons);
    reset();pending=remain=true;exit_requested=true;assert(app_tick(0)==0);assert(!stopped);
    reset();pending=remain=true;assert(app_tick(0x2000)==0);
    assert(delivered==0x2000&&strchr(calls,'F')&&!turn_continued);
    reset();pending=remain=true;runaway=true;assert(app_tick(0)==ESP_ERR_TIMEOUT);
    assert(!strchr(calls,'F')&&!strchr(calls,'P'));
    reset();pending=true;guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(!strcmp(calls,"ACEO"));
    // A pending guest submission gets its own display turn.
    reset();need_present=submission=true;assert(app_tick(0)==0);assert(!strcmp(calls,"P"));
    reset();need_present=submission=native_animation=true;assert(app_tick(0)==0);assert(calls[0]=='P'&&strchr(calls,'F'));
    reset();need_present=true;assert(app_tick(0)==0);assert(calls[0]=='P'&&strchr(calls,'F'));
    // A diagnostic capture forces an owner redraw. Do not run JS and present
    // its next picture inside the same CAPTURE_BEGIN/END interval: that emits
    // row 0 again after row 134, invalidating the one-frame serial protocol.
    reset();board_capture(true);need_present=activate=true;assert(board_capture_active());
    assert(app_tick(0)==0);assert(!strcmp(calls,"P"));
    // Finishing capture restores ordinary dispatch on the very next turn.
    board_capture(false);calls[0]=0;assert(app_tick(0)==0);
    assert(strchr(calls,'F')&&strchr(calls,'P'));
    // main.c can send a press and its release within one capture scope.
    // Keep capture active across BOTH calls, as the real wrapper does.
    reset();board_capture(true);need_present=activate=true;
    assert(app_tick(0x20)==0);assert(app_tick(0)==0);
    assert(!strcmp(calls,"P"));
    // Even new invalidation/native work cannot spend a second turn in this
    // scope. Only closing and starting the NEXT scope replenishes its turn.
    need_present=native_animation=true;assert(app_tick(0)==0);
    assert(!strcmp(calls,"P"));
    board_capture(false);board_capture(true);assert(app_tick(0)==0);
    assert(!strcmp(calls,"PP"));
    // Back still reaches its final save frame despite a capture flag.
    reset();board_capture(true);need_present=true;assert(app_tick(0x2000)==0);
    assert(delivered==0x2000&&strchr(calls,'F'));
    reset();need_present=submission=true;assert(app_tick(0x2000)==0);assert(delivered==0x2000);
    reset();scope=KSN_INPUT_BLOCKED;assert(app_tick(0x4000)==0);assert(!delivered);
    reset();pending=remain=true;now=1000;assert(app_tick(0)==0);assert(!strchr(calls,'P'));
    // A failed presenter step ends the turn before anything reaches JS.
    reset();presenter_result=KSN_BUSY;assert(app_tick(0x4000)==ESP_FAIL);assert(!strcmp(calls,""));
    // R3a: the continuation finished a parked frame() that submitted. That
    // ticket is presented before any new frame(), whose patch would otherwise
    // find it unconsumed; the keys wait for the next turn.
    reset();pending=true;cont_submits=true;assert(app_tick(0x20)==0);
    assert(!strcmp(calls,"ACEOP")&&!strchr(calls,'F')&&deferred_buttons==0x20);
    calls[0]=0;cont_submits=false;assert(app_tick(0)==0);
    assert(strchr(calls,'F')&&delivered==0x20&&!deferred_buttons);
    // Back is the last save turn: it is not held back for that present, but
    // the present comes first, in the same turn, so the save frame() does not
    // meet the unconsumed ticket.
    reset();pending=true;cont_submits=true;assert(app_tick(0x2000)==0);
    assert(strchr(calls,'F')&&delivered==0x2000&&!frame_met_pending);
    assert(strchr(calls,'P')<strchr(calls,'F'));
    // The same with the parked frame's procedural commit(): the save frame's
    // beginFrame() finds nothing pending (it was BUSY, EXECUTION FAILED).
    reset();pending=true;proc_commits=true;cont_submits=true;assert(app_tick(0x2000)==0);
    assert(!strcmp(calls,"ACEOPatiobncpfvkEFEOP")&&delivered==0x2000&&!frame_met_pending);
    // Back with nothing parked: no extra present before the save frame.
    reset();assert(app_tick(0x2000)==0);assert(!strcmp(calls,"AatiobncpfvkEFEOP"));
    // Back whose continuation finished the frame but left jobs (the leave
    // budget cut the drain): still presented first, and the turn stays open
    // around the save frame (park, not end) because work remains.
    reset();pending=true;proc_commits=true;jobs_left=true;assert(app_tick(0x2000)==0);
    assert(!strcmp(calls,"ACYOPatiobncpfvkYFYOP")&&!frame_met_pending);
    // A frame() the budget parks keeps its procedural frame open ('Y', not
    // 'E'); the continuation that finishes it ends the turn, and only then.
    reset();frame_parks=true;assert(app_tick(0)==0);
    assert(!strcmp(calls,"AatiobncpfvkEFYOP"));
    calls[0]=0;frame_parks=false;remain=true;now+=1000;assert(app_tick(0)==0);
    assert(!strncmp(calls,"ACYO",4)&&!strchr(calls,'E')&&!strchr(calls,'F')&&turn_continued);
    calls[0]=0;remain=false;assert(app_tick(0)==0);
    assert(!strncmp(calls,"ACEO",4)&&!strchr(calls,'Y'));
    // The continuation finishes the parked frame() and its procedural
    // commit(): that frame is presented before the next frame(), whose
    // beginFrame() would otherwise meet it pending (BUSY).
    reset();pending=true;proc_commits=true;assert(app_tick(0x20)==0);
    assert(!strcmp(calls,"ACEOP")&&!strchr(calls,'F')&&deferred_buttons==0x20&&!proc_pending);
    // A continuation that throws out of the parked frame is the end of it.
    reset();pending=true;guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(!strcmp(calls,"ACEO"));
    // Unless it left jobs queued (guest.c): the turn then only parks, and
    // the half-built frame is closed by app_vm_prepare_stop() when the
    // session ends on this error -- no frame() runs in between.
    reset();pending=true;jobs_left=true;guest_error=ESP_FAIL;assert(app_tick(0)==ESP_FAIL);
    assert(!strcmp(calls,"ACYO"));
    puts("session dispatch PASS: ordering, cleanup, Back, continuation, display turn, watchdog, parked-frame present, Back present before the save frame");
#ifdef TEST_DERBY_DETAIL_EXIT
    reset();fixture_phase=DF_DONE;exit_requested=true;
    assert(app_derby_detail_finished());pending=true;assert(!app_derby_detail_finished());
    pending=false;atomic_store(&stop_requested,true);assert(!app_derby_detail_finished());
    atomic_store(&stop_requested,false);guest=NULL;assert(!app_derby_detail_finished());
    reset();fixture_phase=DF_ABORT;exit_requested=true;assert(!app_derby_detail_finished());
    reset();fixture_phase=DF_DONE;assert(!app_derby_detail_finished());
    // The frame that emits END requests exit. Owner ends it before a second
    // VM entry (including the release entry for a key press).
    reset();finish_in_frame=true;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_OK&&!stopped&&!home_error&&!running);
    assert(strchr(calls,'F')&&strchr(calls,'X')&&!strchr(calls,'S'));
    assert(strchr(calls,'T')<strchr(calls,'H')&&strchr(calls,'H')==strrchr(calls,'H'));
    assert(strchr(calls,'R')<strchr(calls,'T'));
    reset();finish_in_frame=true;keystroke_t enter={KEY_ENTER};tick_run(true,&enter);
    assert(ended&&ended_result==ESP_OK&&!stopped);
    assert(strchr(calls,'F')==strrchr(calls,'F'));
    // DONE alone never turns a genuine failing final frame into success.
    reset();finish_in_frame=true;guest_error=ESP_FAIL;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_FAIL&&home_error);
    assert(!strchr(calls,'R'));
    reset();finish_in_frame=true;exit_write_ok=false;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_FAIL&&home_error&&strchr(calls,'R'));
    // The real interrupt predicate catches a late successful VM return. It
    // fails in this owner turn, instead of rearming and accepting next tick.
    reset();finish_in_frame=true;frame_elapsed=250001;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_ERR_TIMEOUT&&home_error&&!strchr(calls,'R'));
    reset();fixture_phase=DF_DONE;exit_requested=true;pending=true;
    continuation_elapsed=250001;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_ERR_TIMEOUT&&!strchr(calls,'R'));
    // Draining jobs after END is required; neither exit nor DONE skips them.
    reset();fixture_phase=DF_DONE;exit_requested=true;pending=remain=true;
    tick_run(false,NULL);assert(!ended&&!stopped&&strchr(calls,'C'));
    calls[0]=0;remain=false;tick_run(false,NULL);
    assert(ended&&ended_result==ESP_OK&&!strchr(calls,'F')&&!strchr(calls,'S'));
    reset();fixture_phase=DF_DONE;exit_requested=true;pending=true;guest_error=ESP_FAIL;
    tick_run(false,NULL);assert(ended&&ended_result==ESP_FAIL);
    reset();fixture_phase=DF_DONE;exit_requested=true;pending=remain=runaway=true;
    tick_run(false,NULL);assert(ended&&ended_result==ESP_ERR_TIMEOUT);
    reset();fixture_phase=DF_DONE;exit_requested=true;app_request_stop();
    tick_run(false,NULL);assert(ended&&ended_result==ESP_FAIL&&home_error);
    reset();finish_in_frame=true;keystroke_t back={KEY_BACK};tick_run(true,&back);
    assert(ended&&stopped&&delivered==0x2000);
    puts("detailed owner exit PASS: final frame, release guard, full drain, errors/watchdog preserved");
#endif
}
