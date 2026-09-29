// The automatic clock sync and the link service under it, on the host.
//
// What runs here is the real main/pocket/net_service.c and the policy and
// attempt halves of main/pocket/net_autosync.c; what is faked is everything
// they stand on -- wifi_time.c's link (a script of how long association,
// SNTP and the teardown take), the clock (advanced only by the attempt's own
// sleeps, so every deadline is exact), and the "an app wants to start" signal.
// The FreeRTOS task, net_autosync_poll() and net_autosync_yield() are device
// only and are covered by the steps in docs/platform/wifi-autostart.md.
//
//   bash tools/build_net_autosync_test.sh     (WSL; ASan and UBSan)

#include "net_autosync.h"
#include "net_service.h"
#include "wifi_time.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond,...) do { if(!(cond)) { \
    printf("FAIL %s:%d ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while(0)

// ------------------------------------------------------------ the fake radio

static int64_t now;                 // ms; moved only by fake_sleep()

static struct {
    // script
    int64_t up_after;               // association time; <0 never comes up
    int64_t fail_after;             // the link gives up at this age; <0 never
    int64_t teardown_ms;            // stop request to lock released
    esp_err_t start_result;         // what wifi_time_link_start_for returns
    // state
    bool running;                   // the single-attempt lock
    wifi_time_link_owner_t owner;
    wifi_time_link_t state;
    int64_t started_at, stop_at;    // stop_at: when the teardown completes, -1 none
    int starts, stops_honoured, stops_ignored;
    wifi_time_status_t status;
} radio;

static struct {
    esp_err_t start_result;
    int64_t ok_after;               // <0 never answers
    esp_err_t final_error;          // returned instead of TIMEOUT once past ok_after, if set
    bool active;
    int64_t started_at;
    int stops;
    bool active_at_release;         // SNTP still up when the link was released
} sntp;

static int synchronized_calls;
static int64_t abort_at=-1;         // when "an app start" asks for the radio

static void radio_reset(void) {
    memset(&radio,0,sizeof radio);
    radio.up_after=3000; radio.fail_after=-1; radio.teardown_ms=200;
    radio.start_result=ESP_OK; radio.stop_at=-1;
    memset(&sntp,0,sizeof sntp);
    sntp.ok_after=1500;
    synchronized_calls=0; abort_at=-1;
}

// The link task, advanced to `now`.
static void radio_advance(void) {
    if(!radio.running) return;
    if(radio.stop_at>=0 && now>=radio.stop_at) {
        radio.running=false; radio.owner=0; radio.state=WIFI_TIME_LINK_DOWN;
        radio.stop_at=-1;
        return;
    }
    if(radio.state==WIFI_TIME_LINK_CONNECTING && radio.stop_at<0) {
        int64_t age=now-radio.started_at;
        if(radio.fail_after>=0 && age>=radio.fail_after) {
            // As wifi_time.c: a failed connect ends DOWN, owner cleared, and
            // the stage it failed at is left in the status.
            radio.running=false; radio.owner=0; radio.state=WIFI_TIME_LINK_DOWN;
            radio.status=(wifi_time_status_t){.state=WIFI_TIME_FAILED,.stage=WIFI_TIME_STAGE_ASSOC};
        } else if(radio.up_after>=0 && age>=radio.up_after) {
            radio.state=WIFI_TIME_LINK_UP;
            radio.status=(wifi_time_status_t){.state=WIFI_TIME_OK};
        }
    }
}

esp_err_t wifi_time_link_start_for(wifi_time_link_owner_t owner) {
    radio_advance();
    if(radio.running) return ESP_ERR_INVALID_STATE;
    if(radio.start_result!=ESP_OK) return radio.start_result;
    radio.running=true; radio.owner=owner; radio.state=WIFI_TIME_LINK_CONNECTING;
    radio.started_at=now; radio.stop_at=-1; radio.starts++;
    radio.status=(wifi_time_status_t){.state=WIFI_TIME_RUNNING,.stage=WIFI_TIME_STAGE_ASSOC};
    return ESP_OK;
}
void wifi_time_link_stop_for(wifi_time_link_owner_t owner) {
    radio_advance();
    if(radio.running && radio.owner==owner && radio.stop_at<0) {
        radio.stop_at=now+radio.teardown_ms; radio.stops_honoured++;
        if(sntp.active) sntp.active_at_release=true;
    } else radio.stops_ignored++;
}
wifi_time_link_owner_t wifi_time_link_owner(void) { radio_advance(); return radio.owner; }
wifi_time_link_t wifi_time_link_state(void) { radio_advance(); return radio.state; }
bool wifi_time_busy(void) { radio_advance(); return radio.running; }
void wifi_time_status_settle(wifi_time_state_t state, wifi_time_stage_t stage, int reason) {
    radio.status.state=state; radio.status.stage=stage; radio.status.reason=reason;
}

// ------------------------------------------------------------- the attempt ops

static int64_t fake_now(void) { return now; }
static void fake_sleep(unsigned ms) { now+=ms; radio_advance(); }
static bool fake_abort(void) { return abort_at>=0 && now>=abort_at; }
static esp_err_t fake_sntp_start(void) {
    if(sntp.start_result!=ESP_OK) return sntp.start_result;
    sntp.active=true; sntp.started_at=now;
    return ESP_OK;
}
static esp_err_t fake_sntp_wait(unsigned slice) {
    int64_t due=sntp.ok_after>=0 ? sntp.started_at+sntp.ok_after : -1;
    if(due>=0 && now+slice>=due) {
        if(now<due) now=due;
        return sntp.final_error?sntp.final_error:ESP_OK;
    }
    now+=slice; radio_advance();
    return ESP_ERR_TIMEOUT;
}
static void fake_sntp_stop(void) { sntp.active=false; sntp.stops++; }
static void fake_synchronized(void) { synchronized_calls++; }

static const autosync_ops_t ops={
    .now_ms=fake_now, .sleep_ms=fake_sleep, .abort_requested=fake_abort,
    .sntp_start=fake_sntp_start, .sntp_wait=fake_sntp_wait, .sntp_stop=fake_sntp_stop,
    .set_synchronized=fake_synchronized, .radio_busy=wifi_time_busy,
};

// ------------------------------------------------------------------ policy

static autosync_inputs_t ready(void) {
    return (autosync_inputs_t){.enabled=true,.has_credentials=true,
                               .idle_ms=AUTOSYNC_IDLE_MS,.free_bytes=274*1024};
}

static void policy_gates(void) {
    autosync_policy_t p; autosync_policy_init(&p);
    autosync_inputs_t in=ready();

    in.enabled=false;
    CHECK(autosync_policy_decide(&p,&in,0)==AUTOSYNC_WAIT,"the setting off starts nothing");
    in=ready(); in.idle_ms=AUTOSYNC_IDLE_MS-1;
    CHECK(autosync_policy_decide(&p,&in,0)==AUTOSYNC_WAIT,"one ms short of idle");
    in.idle_ms=-1;
    CHECK(autosync_policy_decide(&p,&in,0)==AUTOSYNC_WAIT,"not on the home screen");
    CHECK(p.attempts==0,"none of those counted");

    in=ready(); in.has_credentials=false;
    CHECK(autosync_policy_decide(&p,&in,0)==AUTOSYNC_NO_CREDENTIALS,"nothing stored");
    CHECK(p.next_ms==AUTOSYNC_RETRY_MS && p.attempts==0,"deferred a minute, not counted");
    in.has_credentials=true;
    CHECK(autosync_policy_decide(&p,&in,AUTOSYNC_RETRY_MS-1)==AUTOSYNC_WAIT,
          "and not asked again inside that minute");

    in.free_bytes=AUTOSYNC_MIN_FREE-1;
    CHECK(autosync_policy_decide(&p,&in,AUTOSYNC_RETRY_MS)==AUTOSYNC_LOW_MEMORY,"below the floor");
    CHECK(p.attempts==0 && p.failures==0,"low memory is neither an attempt nor a failure");
    in.free_bytes=AUTOSYNC_MIN_FREE;
    CHECK(autosync_policy_decide(&p,&in,2*AUTOSYNC_RETRY_MS)==AUTOSYNC_START,"at the floor");
    CHECK(p.attempts==1 && p.running,"counted and running");
    CHECK(autosync_policy_decide(&p,&in,2*AUTOSYNC_RETRY_MS+1)==AUTOSYNC_WAIT,
          "no second attempt while one runs");
}

static void policy_backoff(void) {
    autosync_policy_t p; autosync_policy_init(&p);
    autosync_inputs_t in=ready();
    int64_t t=0;
    const int64_t gaps[]={AUTOSYNC_BACKOFF_1_MS,AUTOSYNC_BACKOFF_2_MS,AUTOSYNC_BACKOFF_3_MS};
    for(int i=0;i<3;i++) {
        CHECK(autosync_policy_decide(&p,&in,t)==AUTOSYNC_START,"attempt %d",i+1);
        autosync_policy_record(&p,AUTOSYNC_FAILED,t);
        CHECK(p.failures==(unsigned)i+1,"failure %d counted",i+1);
        CHECK(p.next_ms==t+gaps[i],"backoff %d is %lld ms",i+1,(long long)gaps[i]);
        CHECK(autosync_policy_decide(&p,&in,t+gaps[i]-1)==AUTOSYNC_WAIT,"not before it");
        t+=gaps[i];
    }
    CHECK(autosync_policy_decide(&p,&in,t)==AUTOSYNC_START,"the fourth attempt");
    autosync_policy_record(&p,AUTOSYNC_FAILED,t);
    CHECK(p.gave_up,"four failures in a row end it for the boot");
    CHECK(autosync_policy_decide(&p,&in,t+AUTOSYNC_RESYNC_MS*10)==AUTOSYNC_WAIT,
          "and nothing starts afterwards");
    CHECK(p.attempts==AUTOSYNC_MAX_FAILURES,"exactly %d attempts",AUTOSYNC_MAX_FAILURES);

    // Turning the setting on again is net_autosync_set_enabled() re-initialising.
    autosync_policy_init(&p);
    CHECK(autosync_policy_decide(&p,&in,t)==AUTOSYNC_START,"a fresh policy tries again");
}

static void policy_success_and_resync(void) {
    autosync_policy_t p; autosync_policy_init(&p);
    autosync_inputs_t in=ready();
    CHECK(autosync_policy_decide(&p,&in,0)==AUTOSYNC_START,"first");
    autosync_policy_record(&p,AUTOSYNC_FAILED,0);
    CHECK(autosync_policy_decide(&p,&in,AUTOSYNC_BACKOFF_1_MS)==AUTOSYNC_START,"retry");
    int64_t t=AUTOSYNC_BACKOFF_1_MS+25000;
    autosync_policy_record(&p,AUTOSYNC_OK,t);
    CHECK(p.failures==0 && p.last_ok_ms==t,"a success clears the streak");
    CHECK(p.next_ms==t+AUTOSYNC_RESYNC_MS,"the next look is the resync");
    CHECK(AUTOSYNC_RESYNC_MS>=12LL*3600*1000 && AUTOSYNC_RESYNC_MS<=24LL*3600*1000,
          "resync inside 12..24 h");
    CHECK(autosync_policy_decide(&p,&in,t+AUTOSYNC_RESYNC_MS-1)==AUTOSYNC_WAIT,"not before 12 h");
    in.idle_ms=0;
    CHECK(autosync_policy_decide(&p,&in,t+AUTOSYNC_RESYNC_MS)==AUTOSYNC_WAIT,
          "due, but only from an idle home screen");
    in.idle_ms=AUTOSYNC_IDLE_MS;
    CHECK(autosync_policy_decide(&p,&in,t+AUTOSYNC_RESYNC_MS)==AUTOSYNC_START,"at 12 h");
}

static void policy_aborts_and_cap(void) {
    autosync_policy_t p; autosync_policy_init(&p);
    autosync_inputs_t in=ready();
    int64_t t=0;
    CHECK(autosync_policy_decide(&p,&in,t)==AUTOSYNC_START,"first");
    autosync_policy_record(&p,AUTOSYNC_ABORTED,t);
    CHECK(p.failures==0 && !p.gave_up,"a yield is not a failure");
    CHECK(p.next_ms==t+AUTOSYNC_RETRY_MS,"but it waits a minute");
    autosync_policy_record(&p,AUTOSYNC_BUSY,t);
    CHECK(p.failures==0,"nor is a busy radio");

    // Someone flicking between the menu and an app all day: bounded.
    autosync_policy_init(&p);
    unsigned started=0;
    for(int i=0;i<200;i++) {
        t+=AUTOSYNC_RETRY_MS;
        if(autosync_policy_decide(&p,&in,t)==AUTOSYNC_START) {
            started++;
            autosync_policy_record(&p,AUTOSYNC_ABORTED,t);
        }
    }
    CHECK(started==AUTOSYNC_MAX_ATTEMPTS,"%u attempts, cap %d",started,AUTOSYNC_MAX_ATTEMPTS);
    CHECK(p.gave_up,"the cap ends it");
}

// ----------------------------------------------------------------- attempt

static void attempt_ok(void) {
    radio_reset(); now=1000;
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_OK,"got %s",autosync_outcome_name(out));
    CHECK(synchronized_calls==1,"the clock is marked once (%d)",synchronized_calls);
    CHECK(sntp.stops==1 && !sntp.active_at_release,"SNTP stopped before the release");
    CHECK(net_service_holders()==0,"the hold is given back");
    CHECK(!radio.running,"the radio is down when the attempt returns");
    CHECK(radio.status.state==WIFI_TIME_OK,"the status says the clock was set");
    CHECK(radio.stops_honoured==1,"one stop");
}

static void attempt_link_fails(void) {
    radio_reset(); now=0;
    radio.up_after=-1; radio.fail_after=15000;     // the link's own 15 s
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_FAILED,"got %s",autosync_outcome_name(out));
    CHECK(synchronized_calls==0,"no sync without a link");
    CHECK(sntp.stops==0,"SNTP never started");
    CHECK(net_service_holders()==0,"the hold is given back");
    CHECK(radio.status.state==WIFI_TIME_FAILED && radio.status.stage==WIFI_TIME_STAGE_ASSOC,
          "the link's stage is left for the Wi-Fi screen");
    CHECK(now>=15000 && now<15000+AUTOSYNC_SLICE_MS+1,"returns a slice after the link gave up (%lld)",
          (long long)now);
    CHECK(radio.stops_ignored==1,"the release found no link to stop, and nothing else did");
}

static void attempt_link_never_answers(void) {
    // A link task that neither comes up nor gives up: the outer bound holds.
    radio_reset(); now=0;
    radio.up_after=-1; radio.fail_after=-1;
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_FAILED,"got %s",autosync_outcome_name(out));
    CHECK(now<=AUTOSYNC_CONNECT_WAIT_MS+radio.teardown_ms+AUTOSYNC_SLICE_MS,
          "bounded by the connect wait (%lld)",(long long)now);
    CHECK(!radio.running && net_service_holders()==0,"and cleaned up");
}

static void attempt_sntp_timeout(void) {
    radio_reset(); now=0;
    sntp.ok_after=-1;
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_FAILED,"got %s",autosync_outcome_name(out));
    CHECK(synchronized_calls==0,"no answer, no sync");
    CHECK(sntp.stops==1 && !sntp.active_at_release,"SNTP stopped before the release");
    CHECK(radio.status.state==WIFI_TIME_FAILED && radio.status.stage==WIFI_TIME_STAGE_SNTP,
          "failed at sntp, not the link's OK");
    int64_t sntp_began=radio.up_after;
    CHECK(now>=sntp_began+AUTOSYNC_SNTP_WAIT_MS &&
          now<=sntp_began+AUTOSYNC_SNTP_WAIT_MS+AUTOSYNC_SLICE_MS+radio.teardown_ms+10,
          "SNTP given its 10 s and no more (%lld)",(long long)now);
}

static void attempt_sntp_start_fails(void) {
    radio_reset(); now=0;
    sntp.start_result=ESP_ERR_NO_MEM;
    CHECK(autosync_attempt(&ops)==AUTOSYNC_FAILED,"a refused SNTP start fails");
    CHECK(synchronized_calls==0 && net_service_holders()==0 && !radio.running,"cleanly");
}

static void attempt_sntp_error(void) {
    radio_reset(); now=0;
    sntp.ok_after=500; sntp.final_error=ESP_FAIL;
    CHECK(autosync_attempt(&ops)==AUTOSYNC_FAILED,"a final SNTP error fails");
    CHECK(synchronized_calls==0,"and does not mark the clock");
}

static void attempt_abort_connecting(void) {
    radio_reset(); now=0;
    abort_at=1234;                              // the app starts mid-association
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_ABORTED,"got %s",autosync_outcome_name(out));
    CHECK(synchronized_calls==0,"no sync");
    CHECK(!radio.running,"the radio is down when the yield would return");
    // What the app waits: at most one slice to notice, then the teardown.
    CHECK(now-abort_at<=AUTOSYNC_SLICE_MS+radio.teardown_ms+10,
          "yield latency %lld ms",(long long)(now-abort_at));
    CHECK(radio.status.state==WIFI_TIME_IDLE,"an abandoned attempt concluded nothing");
    CHECK(net_service_holders()==0,"the hold is given back");
}

static void attempt_abort_sntp(void) {
    radio_reset(); now=0;
    sntp.ok_after=8000;
    abort_at=radio.up_after+2000;               // during the SNTP wait
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_ABORTED,"got %s",autosync_outcome_name(out));
    CHECK(synchronized_calls==0,"no sync");
    CHECK(sntp.stops==1 && !sntp.active_at_release,"SNTP stopped before the release");
    CHECK(now-abort_at<=AUTOSYNC_SLICE_MS+radio.teardown_ms+10,
          "yield latency %lld ms",(long long)(now-abort_at));
    CHECK(radio.status.state==WIFI_TIME_IDLE,"not the link's OK");
}

static void attempt_abort_before(void) {
    // The request lands between acquire and the first look: still no sync.
    radio_reset(); now=0;
    abort_at=0;
    CHECK(autosync_attempt(&ops)==AUTOSYNC_ABORTED,"aborted at once");
    CHECK(synchronized_calls==0 && !radio.running && net_service_holders()==0,"cleanly");
}

static void attempt_busy(void) {
    radio_reset(); now=0;
    radio.running=true;                          // the settings screen's scan
    autosync_outcome_t out=autosync_attempt(&ops);
    CHECK(out==AUTOSYNC_BUSY,"got %s",autosync_outcome_name(out));
    CHECK(net_service_holders()==0,"nothing counted");
    CHECK(radio.stops_honoured==0 && radio.stops_ignored==0,"and nothing released");
    CHECK(radio.running,"the scan is left alone");
}

// ------------------------------------------------------------- the service

static net_service_state_t heard[16];
static int heard_n;
static void listener(net_service_state_t s, void *user) {
    (void)user;
    if(heard_n<16) heard[heard_n++]=s;
}

static void service_counts(void) {
    radio_reset(); now=0;
    CHECK(net_service_state()==NET_SERVICE_OFF,"off with no holder");
    CHECK(net_service_acquire(NULL)==ESP_ERR_INVALID_ARG,"no name");
    CHECK(net_service_acquire("")==ESP_ERR_INVALID_ARG,"empty name");
    CHECK(net_service_acquire("a")==ESP_OK,"a");
    CHECK(net_service_acquire("a")==ESP_OK,"a twice");
    CHECK(net_service_acquire("b")==ESP_OK,"b");
    CHECK(radio.starts==1,"one link for three holds (%d)",radio.starts);
    CHECK(net_service_holders()==3,"three holds");
    char text[40];
    net_service_describe(text,sizeof text);
    CHECK(!strcmp(text,"a=2 b=1"),"describe: '%s'",text);
    net_service_describe(text,4);
    CHECK(strlen(text)<4,"describe truncates rather than overflowing: '%s'",text);
    CHECK(net_service_state()==NET_SERVICE_CONNECTING,"connecting");
    fake_sleep(3000);
    CHECK(net_service_state()==NET_SERVICE_UP,"up");

    CHECK(net_service_release("c")==ESP_ERR_INVALID_STATE,"a name that never acquired");
    CHECK(net_service_release("a")==ESP_OK && radio.stops_honoured==0,"a: still held");
    CHECK(net_service_release("b")==ESP_OK && radio.stops_honoured==0,"b: still held by a");
    CHECK(net_service_release("a")==ESP_OK && radio.stops_honoured==1,"the last one stops it");
    CHECK(net_service_release("a")==ESP_ERR_INVALID_STATE,"one release too many is refused");
    CHECK(net_service_holders()==0 && radio.stops_honoured==1,"and changes nothing");
    CHECK(net_service_state()==NET_SERVICE_OFF,"off");

    // Released but still tearing down: a new hold is told the radio is busy
    // rather than counted onto a link that is about to vanish.
    CHECK(net_service_acquire("a")==ESP_ERR_INVALID_STATE,"busy while tearing down");
    CHECK(net_service_holders()==0,"not counted");
    fake_sleep(radio.teardown_ms);
    CHECK(net_service_acquire("a")==ESP_OK && radio.starts==2,"and fine afterwards");
    net_service_release("a"); fake_sleep(radio.teardown_ms);
}

static void service_table_full(void) {
    radio_reset(); now=0;
    const char *names[]={"n0","n1","n2","n3"};
    for(int i=0;i<NET_SERVICE_REASONS;i++)
        CHECK(net_service_acquire(names[i])==ESP_OK,"%s",names[i]);
    CHECK(net_service_acquire("extra")==ESP_ERR_NO_MEM,"a fifth name");
    CHECK(net_service_acquire("n0")==ESP_OK,"an existing name still counts");
    CHECK(net_service_holders()==NET_SERVICE_REASONS+1,"holders");
    net_service_release("n0");
    for(int i=0;i<NET_SERVICE_REASONS;i++) net_service_release(names[i]);
    CHECK(net_service_holders()==0 && radio.stops_honoured==1,"all back, one stop");
    CHECK(net_service_acquire("extra")==ESP_ERR_INVALID_STATE,"a freed slot, but the radio is still coming down");
    fake_sleep(radio.teardown_ms);
    CHECK(net_service_acquire("extra")==ESP_OK,"a freed slot is reused");
    net_service_release("extra"); fake_sleep(radio.teardown_ms);
}

static void service_failure_and_retry(void) {
    radio_reset(); now=0;
    radio.up_after=-1; radio.fail_after=15000;
    CHECK(net_service_acquire("bg")==ESP_OK,"held");
    fake_sleep(15000);
    CHECK(net_service_state()==NET_SERVICE_FAILED,"a held link that failed is FAILED, not OFF");
    radio.up_after=1000; radio.fail_after=-1;
    CHECK(net_service_acquire("time")==ESP_OK && radio.starts==2,
          "a new acquire starts it again");
    fake_sleep(1000);
    CHECK(net_service_state()==NET_SERVICE_UP,"up on the retry");
    net_service_release("time"); net_service_release("bg");
    CHECK(radio.stops_honoured==1,"stopped once, by the last holder");
    fake_sleep(radio.teardown_ms);
}

static void service_refused_start(void) {
    radio_reset(); now=0;
    radio.running=true; radio.owner=WIFI_TIME_LINK_APP;   // an app's lease
    CHECK(net_service_acquire("time")==ESP_ERR_INVALID_STATE,"the app has the radio");
    CHECK(net_service_holders()==0,"nothing counted");
    CHECK(net_service_release("time")==ESP_ERR_INVALID_STATE,"so nothing to release");
    CHECK(radio.running && radio.owner==WIFI_TIME_LINK_APP,"and the app's link untouched");
    radio.running=false; radio.owner=0;
    radio.start_result=ESP_ERR_NO_MEM;
    CHECK(net_service_acquire("time")==ESP_ERR_NO_MEM,"no room for the task");
    CHECK(net_service_holders()==0,"nothing counted");
}

static void service_notifies(void) {
    radio_reset(); now=0; heard_n=0;
    CHECK(net_service_listen(listener,NULL),"listen");
    net_service_pump();
    CHECK(heard_n==0,"no change, no call");
    net_service_acquire("time");
    net_service_pump(); net_service_pump();
    CHECK(heard_n==1 && heard[0]==NET_SERVICE_CONNECTING,"connecting, once");
    fake_sleep(3000); net_service_pump();
    CHECK(heard_n==2 && heard[1]==NET_SERVICE_UP,"up");
    net_service_release("time"); net_service_pump();
    CHECK(heard_n==3 && heard[2]==NET_SERVICE_OFF,"off at the release");
    fake_sleep(radio.teardown_ms); net_service_pump();
    CHECK(heard_n==3,"the teardown is not a second change");
    net_service_unlisten(listener,NULL);
    net_service_acquire("time"); net_service_pump();
    CHECK(heard_n==3,"unlistened");
    net_service_release("time"); net_service_pump(); fake_sleep(radio.teardown_ms);
}

int main(void) {
    net_service_init();
    policy_gates();
    policy_backoff();
    policy_success_and_resync();
    policy_aborts_and_cap();
    attempt_ok();
    attempt_link_fails();
    attempt_link_never_answers();
    attempt_sntp_timeout();
    attempt_sntp_start_fails();
    attempt_sntp_error();
    attempt_abort_connecting();
    attempt_abort_sntp();
    attempt_abort_before();
    attempt_busy();
    service_counts();
    service_table_full();
    service_failure_and_retry();
    service_refused_start();
    service_notifies();
    if(failures) { printf("NET_AUTOSYNC_FAILED %d\n",failures); return 1; }
    printf("NET_AUTOSYNC_OK\n");
    return 0;
}
