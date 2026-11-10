/* Compile the production main.c callbacks against an asynchronous synthetic
 * display. No target code, device, private asset or callback copy is used. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/ui/diagnostic_control.h"
#define ESP_OK 0
static unsigned ksn_probe_owner;
static diagnostic_session ksn_probe_session=DIAGNOSTIC_SESSION_INIT;
static diagnostic_control native_diagnostic;
static const uint8_t typed_id[16]={1};
static const char *home_error;
static unsigned home_phase;
static struct {
    char events[32]; unsigned used,barriers,releases;
    unsigned fail,cancel,borrow; bool unavailable;
    unsigned char buffer[32];
} f;
static void event(char c){f.events[f.used++]=c;f.events[f.used]=0;}
static int board_present_finish(void){
    unsigned phase=++f.barriers;event(phase==1?'D':phase==2?'B':'F');
    if(f.cancel==phase)(void)diagnostic_command_cancel(&ksn_probe_session.command);
    if(f.fail==phase)return -1;
    /* The consumer still reads the borrowed bytes at completion, not submit. */
    if(f.borrow)assert(f.buffer[0]==(unsigned char)f.borrow);
    f.borrow=0;return ESP_OK;
}
static bool ksn_device_probe_run(void){
    event('W');assert(!f.borrow);f.borrow=2;memset(f.buffer,2,sizeof f.buffer);
    if(f.cancel==4)(void)diagnostic_command_cancel(&ksn_probe_session.command);
    return f.fail!=4;
}
static int shell_draw_checked(const char *error,unsigned phase){
    (void)error;(void)phase;event('R');assert(!f.borrow);
    f.borrow=3;memset(f.buffer,3,sizeof f.buffer);
    if(f.cancel==5)(void)diagnostic_command_cancel(&ksn_probe_session.command);
    return f.fail==5?-1:ESP_OK;
}
/* Generated directly from the edited main.c callback definitions. */
#include "diagnostic_main_callbacks.inc"
static void *acquire(void *ctx){event('A');return f.unavailable?NULL:ksn_probe_acquire(ctx);}
static void release(void *ctx,void *owner){
    event('L');assert(!f.borrow);assert(owner==&ksn_probe_owner);
    ++f.releases;ksn_probe_release(ctx,owner);
}
static const diagnostic_lifecycle_ops ops={.acquire=acquire,.drain=ksn_probe_drain,
    .work=ksn_probe_work,.restore=ksn_probe_restore,.release=release};
static const diagnostic_lifecycle_ops ksn_probe_ops={.acquire=acquire,.drain=ksn_probe_drain,
    .work=ksn_probe_work,.restore=ksn_probe_restore,.release=release};
static bool running,modal,overlay,starting,guest,dormant;
static int screen;
#define SCREEN_HOME 0
static bool home_modal(void){return modal;}
static bool overlay_running(void){return overlay;}
static bool overlay_starting(void){return starting;}
static bool app_guest_present(void){return guest;}
static const char *app_dormant_id(void){return dormant?"synthetic":"";}
static unsigned ready,terminal;
#define ESP_LOGI(tag,message) ((void)(tag),(void)(message),++ready)
#define ESP_LOGE(tag,message) ((void)(tag),(void)(message),++terminal)
static void dispatch(void){
    for(unsigned once=0;once<1;once++){
#include "diagnostic_main_dispatch.inc"
    }
}
static diagnostic_session_result pump(bool admitted){return diagnostic_session_pump(&ksn_probe_session,&ops,admitted);}
static void reset(void){
    /* Distinct fixture construction, never a firmware recovery operation. */
    ksn_probe_session=(diagnostic_session)DIAGNOSTIC_SESSION_INIT;
    assert(diagnostic_control_init(&native_diagnostic,1,true,&ksn_probe_session));
    memset(&f,0,sizeof f);home_phase=0;
    running=modal=overlay=starting=guest=dormant=false;screen=0;ready=terminal=0;
}
int main(void){
    reset();assert(pump(true)==DIAG_SESSION_WAITING);
    assert(diagnostic_command_request(&ksn_probe_session.command)==DIAG_REQUEST_QUEUED);
    assert(pump(false)==DIAG_SESSION_WAITING&&!f.used);
    assert(diagnostic_command_cancel(&ksn_probe_session.command)==DIAG_CANCEL_SUPPRESSED);
    assert(pump(true)==DIAG_SESSION_WAITING&&!f.used);
    for(unsigned cancel=0;cancel<=5;cancel++){
        reset();f.cancel=cancel;f.borrow=1;f.buffer[0]=1;
        assert(diagnostic_command_request(&ksn_probe_session.command)==DIAG_REQUEST_QUEUED);
        assert(diagnostic_command_request(&ksn_probe_session.command)==DIAG_REQUEST_COALESCED);
        assert(pump(true)==(cancel?DIAG_SESSION_CANCELLED:DIAG_SESSION_COMPLETE));
        assert(!strcmp(f.events,"ADWBRFL")&&f.releases==1&&!f.borrow);
        assert(!ksn_probe_session.lifecycle.owner&&!ksn_probe_session.lifecycle.quarantined);
        assert(diagnostic_command_request(&ksn_probe_session.command)==DIAG_REQUEST_QUEUED);
        f.cancel=0;assert(pump(true)==DIAG_SESSION_COMPLETE);assert(f.releases==2);
    }
    const char *failure_events[]={"","AD","ADWB","ADWBRF","ADW","ADWBR"};
    for(unsigned failure=1;failure<=5;failure++){
        reset();f.fail=failure;f.borrow=1;f.buffer[0]=1;
        (void)diagnostic_command_request(&ksn_probe_session.command);
        assert(pump(true)==DIAG_SESSION_TERMINAL);
        assert(!strcmp(f.events,failure_events[failure]));
        assert(ksn_probe_session.lifecycle.owner==&ksn_probe_owner);
        assert(ksn_probe_session.lifecycle.quarantined&&f.borrow&&!f.releases);
        unsigned used=f.used;
        assert(diagnostic_command_cancel(&ksn_probe_session.command)==DIAG_CANCEL_TERMINAL);
        assert(diagnostic_command_request(&ksn_probe_session.command)==DIAG_REQUEST_TERMINAL);
        assert(pump(true)==DIAG_SESSION_TERMINAL&&f.used==used);
    }
    reset();f.unavailable=true;(void)diagnostic_command_request(&ksn_probe_session.command);
    assert(pump(true)==DIAG_SESSION_UNAVAILABLE&&!strcmp(f.events,"A"));
    assert(!ksn_probe_session.lifecycle.owner&&!ksn_probe_session.lifecycle.quarantined);
    f.unavailable=false;(void)diagnostic_command_request(&ksn_probe_session.command);
    assert(pump(true)==DIAG_SESSION_COMPLETE);
    /* The edited UI dispatch itself must wait for every admission condition. */
    for(unsigned blocked=0;blocked<7;blocked++){
        reset();
        if(blocked==0)running=true;
        if(blocked==1)screen=1;
        if(blocked==2)modal=true;
        if(blocked==3)overlay=true;
        if(blocked==4)starting=true;
        if(blocked==5)guest=true;
        if(blocked==6)dormant=true;
        (void)diagnostic_command_request(&ksn_probe_session.command);dispatch();
        assert(!f.used&&!ready&&!terminal);
        running=modal=overlay=starting=guest=dormant=false;screen=0;
        dispatch();assert(ready==1&&!terminal&&!strcmp(f.events,"ADWBRFL"));
    }
    for(unsigned failure=1;failure<=5;failure++){
        reset();f.fail=failure;(void)diagnostic_command_request(&ksn_probe_session.command);
        dispatch();assert(terminal==1&&!ready&&f.borrow== (failure==1?0:failure==2||failure==4?2:3));
        unsigned used=f.used;dispatch();assert(terminal==2&&!ready&&f.used==used);
    }
    /* Execute the typed and manual priority block extracted from main.c,
     * with its actual callbacks, admission expression and final-drain barrier. */
    for(unsigned blocked=0;blocked<7;blocked++){
        reset();
        if(blocked==0)running=true;
        if(blocked==1)screen=1;
        if(blocked==2)modal=true;
        if(blocked==3)overlay=true;
        if(blocked==4)starting=true;
        if(blocked==5)guest=true;
        if(blocked==6)dormant=true;
        assert(diagnostic_control_request(&native_diagnostic,1,typed_id).state==DC_PENDING);
        dispatch();
        diagnostic_control_snapshot st=diagnostic_control_status(&native_diagnostic,1,typed_id);
        assert(st.state==DC_REFUSED&&st.flags==DC_NOT_STARTED&&!f.used&&!ready);
        running=modal=overlay=starting=guest=dormant=false;screen=0;
        dispatch();assert(!f.used); /* No replay after the admission refusal. */
    }
    for(unsigned cancel=0;cancel<2;cancel++){
        reset();f.borrow=1;f.buffer[0]=1;
        assert(diagnostic_control_request(&native_diagnostic,1,typed_id).state==DC_PENDING);
        if(cancel)assert(diagnostic_control_cancel(&native_diagnostic,1,typed_id).state==DC_CANCEL_REQUESTED);
        dispatch();
        diagnostic_control_snapshot st=diagnostic_control_status(&native_diagnostic,1,typed_id);
        assert(st.state==(cancel?DC_CANCELLED:DC_COMPLETED));
        assert(st.flags==(cancel?0x3e:0x1f));
        assert(!strcmp(f.events,cancel?"ADRBL":"ADWBRFL"));
        assert(f.releases==1&&!f.borrow&&ready==1&&!terminal);
        unsigned used=f.used;
        (void)diagnostic_control_request(&native_diagnostic,1,typed_id);
        dispatch();assert(f.used==used&&ready==1);
    }
    for(unsigned failure=1;failure<=5;failure++){
        reset();f.fail=failure;
        (void)diagnostic_control_request(&native_diagnostic,1,typed_id);
        dispatch();
        diagnostic_control_snapshot st=diagnostic_control_status(&native_diagnostic,1,typed_id);
        assert(st.state==DC_QUARANTINED&&!ready&&terminal==1&&!f.releases);
        assert(ksn_probe_session.lifecycle.owner==&ksn_probe_owner);
        unsigned used=f.used;dispatch();assert(f.used==used&&terminal==2);
        assert(diagnostic_control_cancel(&native_diagnostic,1,typed_id).state==DC_QUARANTINED);
    }
    puts("diagnostic firmware callback/session/UI-dispatch integration: PASS");return 0;
}
