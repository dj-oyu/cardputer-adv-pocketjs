"""Host fault tests extracted from actual UI transition functions, not hardware proof."""
from pathlib import Path
import subprocess
import tempfile
import os
temp = tempfile.TemporaryDirectory(prefix="ui-stop-")
tmp = Path(temp.name)
root=Path(__file__).resolve().parents[1]
o=(root/'main/ui/overlay.c').read_text()
seg=o[o.index('/* A failed drain retains'):o.index('// ------------------------------------------------------- room')]
preamble='''#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define OVERLAY_APPS 2
#define ESP_LOGW(...) ((void)0)
typedef enum {OVERLAY_OFF,OVERLAY_RUNNING,OVERLAY_STARTING,OVERLAY_BLOCKED,OVERLAY_REFUSED,OVERLAY_STOPPED} overlay_state_t;
static overlay_state_t state=OVERLAY_RUNNING;
static unsigned choice=1,budget_guest_us,budget_composite_us;
static bool flag_stored,proven,session_up=true,budget_frame_pending,allowed;
static unsigned reset_count,stop_calls;
static bool app_stop(void){stop_calls++;return allowed;}
static bool app_stop_keep_music(void){return app_stop();}
static void pocket_overlay_reset(void){reset_count++;}
static void say(const char*s){(void)s;}
static void announce(const char*s){(void)s;}
static void label_reset(unsigned n){(void)n;}
static void flag_set(bool v){flag_stored=v;}
bool overlay_release(void);
'''
tests='''int main(void){
 assert(!overlay_release());assert(session_up&&state==OVERLAY_RUNNING&&reset_count==0);
 assert(!overlay_retry_stop());assert(session_up&&reset_count==0);
 allowed=true;assert(overlay_retry_stop());assert(!session_up&&state==OVERLAY_STARTING&&reset_count==1);
 session_up=true;state=OVERLAY_RUNNING;allowed=false;
 stop_with("FAULTED",OVERLAY_STOPPED,false);assert(session_up&&state==OVERLAY_RUNNING);
 assert(!overlay_retry_stop());allowed=true;assert(overlay_retry_stop());assert(!session_up&&state==OVERLAY_STOPPED);
 session_up=true;state=OVERLAY_RUNNING;choice=1;allowed=false;
 overlay_armed_set(2);assert(choice==1&&session_up);assert(!overlay_retry_stop());
 allowed=true;assert(overlay_retry_stop());assert(choice==2&&!session_up&&state==OVERLAY_STARTING);
 session_up=true;state=OVERLAY_RUNNING;allowed=false;overlay_armed_set(0);
 assert(choice==2&&session_up);allowed=true;assert(overlay_retry_stop());assert(choice==0&&state==OVERLAY_OFF&&!session_up);
 puts("actual overlay transition snippets: repeated drain failure, retained release/stop/choice PASS");
}
'''
p=(tmp/'ui_overlay_test.c');p.write_text(preamble+seg+tests)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(tmp/'ui_overlay_test')],check=True)
subprocess.run([str(tmp/'ui_overlay_test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
m=(root/'main/main.c').read_text()
def fun(sig):
 start=m.index(sig+' {');a=m.index('{',start);n=1;i=a+1
 # functions here have braces in comments but balanced ones only
 while n:
  n+=(m[i]=='{')-(m[i]=='}');i+=1
 return m[start:i]+'\n'
state=m[m.index('/* Owner-only deferred transitions.'):m.index('static void enter(screen_id_t next) {')]
pre='''#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;typedef int screen_id_t;
#define SCREEN_HOME 0
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 2
#define ESP_LOGI(...) ((void)0)
#define atomic_store(p,v) (*(p)=(v))
static bool allow,app_pending,overlay_pending,running=true;
static int close_count,open_count,release_count,ended_count,restore_count,queue_count,start_count;
static int text_screen,keys,screen,owner,run_started,native_generation;
static const char *home_error;
static bool app_stop_pending(void){return app_pending;}
static bool overlay_stop_pending(void){return overlay_pending;}
static bool app_stop(void){app_pending=!allow;return allow;}
static bool overlay_release(void){overlay_pending=!allow;return allow;}
static bool overlay_retry_stop(void){if(!overlay_pending)return true;return overlay_release();}
static void close_screen(void){close_count++;}
static void open_screen(void){open_count++;}
static void restore(void){restore_count++;}
static void ended(int a,const char*b){(void)a;(void)b;ended_count++;}
typedef struct {void(*close)(void);void(*open)(void);bool takes_text;const char*tag,*ready;void(*run_restore)(void);void(*ended)(int,const char*);} screen_ops_t;
static screen_ops_t SCREENS[2]={{close_screen,open_screen,false,"home","ready",restore,ended},{close_screen,open_screen,true,"editor","ready",restore,ended}};
static void shell_release_background_frames(void){release_count++;}
static void net_autosync_yield(const char*s){(void)s;}
static void xQueueReset(int k){(void)k;queue_count++;}
static const char*app_error(void){return "fault";}
static bool app_frameless(void){return false;}
static void sound_play(int i){(void)i;}
static void take_pending_run(void){}
static bool app_guest_present(void){return app_pending;}
static int app_guest_generation(void){return native_generation;}
static char got_id[32],got_src[32];
static void begin_run(const char*i,const char*p,size_t pn,const char*s,size_t sn){(void)p;(void)pn;start_count++;strcpy(got_id,i);memcpy(got_src,s,sn);got_src[sn]=0;}
'''
body=state+fun('static void enter(screen_id_t next)')+fun('static void end_run(esp_err_t tick_err)')+fun('static bool native_stop(void)')+fun('static bool retry_transitions(void)')
test='''int main(void){
 enter(1);assert(enter_pending&&screen==0&&close_count==0&&release_count==0);
 assert(!retry_transitions());assert(close_count==0);
 allow=true;assert(retry_transitions());assert(screen==1&&close_count==1&&open_count==1&&release_count==1);
 allow=false;end_run(7);assert(end_pending&&running&&ended_count==0&&queue_count==0);
 assert(!retry_transitions());allow=true;assert(retry_transitions());assert(!running&&ended_count==1&&restore_count==1&&queue_count==1);
 char id[]="local.demo",src[]="test source";assert(defer_start(id,NULL,0,src,11));id[0]='X';src[0]='X';
 assert(retry_transitions());assert(start_count==1&&!strcmp(got_id,"local.demo")&&!strcmp(got_src,"test source"));
 assert(defer_start("x",NULL,0,"y",1));cancel_deferred_start();assert(!pending_start.active&&!pending_start.storage);
 puts("actual main transition snippets: deferred screen/end-run/source ownership PASS");
}
'''
p=(tmp/'ui_main_test.c');p.write_text(pre+body+test)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(tmp/'ui_main_test')],check=True)
subprocess.run([str(tmp/'ui_main_test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

# Source-level entry guard checks supplement the transition execution tests.
for signature in ('static uint8_t native_tap(uint8_t action)',
                  'static uint8_t native_suspend(void)'):
    assert fun(signature).split('{',1)[1].lstrip().startswith(
        'if(transition_pending())return AC_BUSY;')
assert fun('static void tick_run(bool have, const keystroke_t *stroke)').split('{',1)[1].lstrip().startswith(
    'if(transition_pending())return;')
suspend_begin=m.index('    if(leave && owner==SCREEN_HOME && app_can_suspend() &&')
suspend_end=m.index('    // Let the guest persist',suspend_begin)
suspend_branch=m[suspend_begin:suspend_end]
fixture = r"""
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
#define SCREEN_HOME 0
#define ESP_OK 0
#define APP_ID_WORK "work"
static int owner, suspend_result, stopped, suspended_count, ticked;
static bool leave=true;
static bool app_can_suspend(void){return true;}
static struct {const char *id;} manifest={"test"};
static const __typeof__(manifest)*app_registry_current(void){return &manifest;}
static int app_suspend(void){return suspend_result;}
static void end_run(int err){assert(err==suspend_result);stopped++;}
static void suspend_run(void){suspended_count++;}
static void path(void){
""" + suspend_branch + r"""
 ticked++;
}
int main(void){
 suspend_result=7;path();assert(stopped==1&&!ticked&&!suspended_count);
 suspend_result=0;path();assert(stopped==1&&!ticked&&suspended_count==1);
 puts("failed suspend bypasses guest turn; native pending guards present PASS");
}
"""
p=tmp/'suspend_path.c';p.write_text(fixture)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(tmp/'suspend_path')],check=True)
subprocess.run([str(tmp/'suspend_path')],check=True)

temp.cleanup()
