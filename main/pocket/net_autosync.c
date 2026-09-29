#include "net_autosync.h"
#include "net_service.h"
#include "wifi_time.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "system/sys_clock.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#endif

// ------------------------------------------------------------------ policy

void autosync_policy_init(autosync_policy_t *p) {
    *p=(autosync_policy_t){.last_ok_ms=-1};
}

autosync_decision_t autosync_policy_decide(autosync_policy_t *p,
                                           const autosync_inputs_t *in,
                                           int64_t now_ms) {
    if(p->running || p->gave_up || !in->enabled) return AUTOSYNC_WAIT;
    if(now_ms<p->next_ms) return AUTOSYNC_WAIT;
    if(in->idle_ms<AUTOSYNC_IDLE_MS) return AUTOSYNC_WAIT;
    // The two refusals below push the next look a minute out rather than
    // asking again next frame: both are answered by a read (NVS, the heap)
    // that is cheap once and not worth making thirty times a second.
    if(!in->has_credentials) {
        p->next_ms=now_ms+AUTOSYNC_RETRY_MS;
        return AUTOSYNC_NO_CREDENTIALS;
    }
    if(in->free_bytes<AUTOSYNC_MIN_FREE) {
        p->next_ms=now_ms+AUTOSYNC_RETRY_MS;
        return AUTOSYNC_LOW_MEMORY;
    }
    if(p->attempts>=AUTOSYNC_MAX_ATTEMPTS) { p->gave_up=true; return AUTOSYNC_WAIT; }
    p->attempts++;
    p->running=true;
    return AUTOSYNC_START;
}

void autosync_policy_record(autosync_policy_t *p, autosync_outcome_t outcome,
                            int64_t now_ms) {
    static const int64_t backoff[]={AUTOSYNC_BACKOFF_1_MS,AUTOSYNC_BACKOFF_2_MS,
                                    AUTOSYNC_BACKOFF_3_MS};
    p->running=false;
    switch(outcome) {
    case AUTOSYNC_OK:
        p->failures=0;
        p->last_ok_ms=now_ms;
        p->next_ms=now_ms+AUTOSYNC_RESYNC_MS;
        break;
    case AUTOSYNC_FAILED:
        // A network that is not there at 09:00 is often there at 09:30, and
        // one that has failed four times in a row is not one this boot will
        // reach by trying a fifth: each attempt is up to 25 s of radio and
        // 48 KB of heap for nothing.
        if(++p->failures>=AUTOSYNC_MAX_FAILURES) { p->gave_up=true; break; }
        p->next_ms=now_ms+backoff[p->failures-1<2?p->failures-1:2];
        break;
    case AUTOSYNC_ABORTED:
    case AUTOSYNC_BUSY:
        // Not the network's fault, so not a failure; but not immediately
        // again either, or someone flicking between the menu and an app
        // would bring the radio up and down each time they came back.
        p->next_ms=now_ms+AUTOSYNC_RETRY_MS;
        break;
    }
}

const char *autosync_outcome_name(autosync_outcome_t outcome) {
    switch(outcome) {
    case AUTOSYNC_OK:      return "ok";
    case AUTOSYNC_FAILED:  return "failed";
    case AUTOSYNC_ABORTED: return "aborted";
    case AUTOSYNC_BUSY:    return "busy";
    }
    return "?";
}

// ----------------------------------------------------------------- attempt

#define HOLDER "time"

autosync_outcome_t autosync_attempt(const autosync_ops_t *ops) {
    esp_err_t err=net_service_acquire(HOLDER);
    // The settings screen's sync or scan, or an app's lease. Its owner will
    // put the radio down; this attempt just comes back later.
    if(err==ESP_ERR_INVALID_STATE) return AUTOSYNC_BUSY;
    if(err!=ESP_OK) return AUTOSYNC_FAILED;

    autosync_outcome_t out=AUTOSYNC_FAILED;
    int64_t began=ops->now_ms();
    net_service_state_t state;
    while((state=net_service_state())==NET_SERVICE_CONNECTING) {
        if(ops->abort_requested()) { out=AUTOSYNC_ABORTED; goto release; }
        if(ops->now_ms()-began>=AUTOSYNC_CONNECT_WAIT_MS) goto release;
        ops->sleep_ms(AUTOSYNC_SLICE_MS);
    }
    // Anything but UP is a link that failed, and wifi_time.c has already put
    // the stage it failed at in the status the Wi-Fi screen shows.
    if(state!=NET_SERVICE_UP) goto release;
    if(ops->abort_requested()) { out=AUTOSYNC_ABORTED; goto release; }

    err=ops->sntp_start();
    if(err!=ESP_OK) {
        wifi_time_status_settle(WIFI_TIME_FAILED,WIFI_TIME_STAGE_SNTP,(int)err);
        goto release;
    }
    began=ops->now_ms();
    for(;;) {
        err=ops->sntp_wait(AUTOSYNC_SLICE_MS);
        if(err!=ESP_ERR_TIMEOUT) break;               // set, or a final error
        if(ops->abort_requested()) { out=AUTOSYNC_ABORTED; break; }
        if(ops->now_ms()-began>=AUTOSYNC_SNTP_WAIT_MS) break;
    }
    // Before the release, never after: the link task's teardown also deinits
    // SNTP, and two tasks deinitialising it at once is a double free.
    ops->sntp_stop();
    if(err==ESP_OK) {
        // The only place this attempt changes anything outside itself. Not
        // conditional on the abort check: a clock that was stepped is right,
        // whoever was waiting for the radio.
        ops->set_synchronized();
        wifi_time_status_settle(WIFI_TIME_OK,WIFI_TIME_STAGE_NONE,0);
        out=AUTOSYNC_OK;
    } else if(out!=AUTOSYNC_ABORTED) {
        wifi_time_status_settle(WIFI_TIME_FAILED,WIFI_TIME_STAGE_SNTP,(int)err);
    }

release:
    // An abandoned attempt concluded nothing. The link said OK when it got an
    // address, and leaving that would show "CLOCK SET" on the Wi-Fi screen
    // the yield was made for.
    if(out==AUTOSYNC_ABORTED)
        wifi_time_status_settle(WIFI_TIME_IDLE,WIFI_TIME_STAGE_NONE,0);
    net_service_release(HOLDER);
    // The attempt is not over until the heap is back: a yield returns when
    // this function does, and the guest it was yielding to is built next.
    began=ops->now_ms();
    while(ops->radio_busy() && ops->now_ms()-began<AUTOSYNC_DOWN_WAIT_MS)
        ops->sleep_ms(10);
    return out;
}

// ----------------------------------------------------------------- runtime

#ifdef ESP_PLATFORM

static const char *TAG = "autosync";

// Same server as the settings screen's sync (wifi_time.c's NTP_SERVER).
#define NTP_SERVER "pool.ntp.org"

// THE FLOOR, AUTOSYNC_MIN_FREE. NET_RADIO_MIN_FREE (pocket_net.c, 56 KiB,
// measured 2026-09-07) is what bringing the radio up costs; the 8 KiB on top is
// an ESTIMATE for what association, DHCP and one SNTP exchange add while it is
// up (the driver's dynamic RX/TX buffers), which that measurement did not
// cover. AUTOSYNC_DONE logs min_free for every attempt, and the first device
// run should replace this estimate with that number. The home screen idles at
// about 274 KiB, so the floor only matters with an app kept asleep (which is
// guaranteed 96 KiB, APP_SUSPEND_MIN_FREE) or an overlay running (guaranteed
// 56 KiB, OVERLAY_FREE_FLOOR) -- in the second case this can skip a sync the
// overlay's floor would have allowed, which is the side to err on.

static autosync_policy_t policy;
static bool enabled=true;
static int  credentials=-1;          // cached; -1 = read NVS on next need
static int64_t eligible_since=-1;
static atomic_bool active;           // an attempt task exists
static atomic_bool abort_req;
static atomic_int  posted=-1;        // the outcome the task left, -1 none

static int64_t dev_now_ms(void) { return esp_timer_get_time()/1000; }
static void dev_sleep_ms(unsigned ms) {
    TickType_t t=pdMS_TO_TICKS(ms);
    vTaskDelay(t?t:1);
}
static bool dev_abort(void) { return atomic_load(&abort_req); }
static esp_err_t dev_sntp_start(void) {
    esp_sntp_config_t c=ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    // Stepped, not slewed, as the manual sync does (wifi_time.c).
    c.smooth_sync=false;
    return esp_netif_sntp_init(&c);
}
static esp_err_t dev_sntp_wait(unsigned ms) {
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(ms));
}
static void dev_sntp_stop(void) { esp_netif_sntp_deinit(); }
// The single call site for this path, like wifi_time.c's for the manual one.
// sys_clock_set_synchronized(false) is never made: a failed sync does not make
// a clock that was once set wrong (docs/scenes/solar-sail.md).
static void dev_set_synchronized(void) { sys_clock_set_synchronized(true); }

static const autosync_ops_t device_ops={
    .now_ms=dev_now_ms, .sleep_ms=dev_sleep_ms, .abort_requested=dev_abort,
    .sntp_start=dev_sntp_start, .sntp_wait=dev_sntp_wait, .sntp_stop=dev_sntp_stop,
    .set_synchronized=dev_set_synchronized, .radio_busy=wifi_time_busy,
};

static size_t free_internal(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}

static void attempt_task(void *arg) {
    (void)arg;
    int64_t t0=esp_timer_get_time();
    size_t before=free_internal();
    // The local low-water mark is the number this feature has to be judged by:
    // the transient peak between esp_wifi_init and the end of SNTP, which the
    // free figures either side of it cannot see. `mon` says whether this task
    // started the monitor; if something else already had (the MEGADEMO trace
    // build), the minimum is still a true minimum, just over a longer window.
    bool mon=heap_caps_monitor_local_minimum_free_size_start()==ESP_OK;
    ESP_LOGI(TAG,"AUTOSYNC_START free=%u largest=%u",(unsigned)before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    autosync_outcome_t out=autosync_attempt(&device_ops);
    size_t low=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(mon) heap_caps_monitor_local_minimum_free_size_stop();
    wifi_time_status_t st=wifi_time_status();
    ESP_LOGI(TAG,"AUTOSYNC_DONE %s stage=%s ms=%u free_before=%u min_free=%u free_after=%u%s",
             autosync_outcome_name(out),wifi_time_stage_name(st.stage),
             (unsigned)((esp_timer_get_time()-t0)/1000),(unsigned)before,
             (unsigned)low,(unsigned)free_internal(),mon?"":" (shared monitor)");
    atomic_store(&posted,(int)out);
    atomic_store(&active,false);
    vTaskDelete(NULL);
}

void net_autosync_init(void) {
    net_service_init();
    autosync_policy_init(&policy);
}

bool net_autosync_enabled(void) { return enabled; }

void net_autosync_set_enabled(bool on) {
    if(on==enabled) return;
    enabled=on;
    if(!on) {
        // Not waited for: the settings row is applied on the home screen, and
        // nothing is about to need the heap. The task finishes on its own.
        if(atomic_load(&active)) atomic_store(&abort_req,true);
    } else {
        // A fresh start, including after the boot gave up. An attempt still
        // winding down keeps its `running` so a second task cannot start
        // before its outcome is collected.
        bool was=policy.running;
        autosync_policy_init(&policy);
        policy.running=was;
    }
    ESP_LOGI(TAG,"AUTOSYNC_ENABLED %d",(int)on);
}

void net_autosync_poll(bool eligible, bool key) {
    int64_t now=dev_now_ms();
    int out=atomic_exchange(&posted,-1);
    if(out>=0) {
        autosync_policy_record(&policy,(autosync_outcome_t)out,now);
        if(policy.gave_up)
            ESP_LOGW(TAG,"AUTOSYNC_GAVE_UP failures=%u attempts=%u",
                     policy.failures,policy.attempts);
        else
            ESP_LOGI(TAG,"AUTOSYNC_NEXT in_s=%lld",(long long)((policy.next_ms-now)/1000));
    }
    if(!eligible) {
        eligible_since=-1;
        // Credentials change only on the Wi-Fi screen, which is never the home
        // screen, so re-reading them after every spell away is exact.
        credentials=-1;
        return;
    }
    if(key || eligible_since<0) eligible_since=now;
    // The cheap half of the policy, so NVS and the heap are asked only when an
    // attempt is actually due.
    if(!enabled || policy.running || policy.gave_up || now<policy.next_ms ||
       now-eligible_since<AUTOSYNC_IDLE_MS || atomic_load(&active))
        return;
    // A clock someone set before the first attempt (the settings screen) does
    // not need another; the next look is the resync.
    if(policy.last_ok_ms<0 && sys_clock_read().synchronized) {
        policy.last_ok_ms=now;
        policy.next_ms=now+AUTOSYNC_RESYNC_MS;
        ESP_LOGI(TAG,"AUTOSYNC_SKIP already synchronized");
        return;
    }
    if(credentials<0) credentials=wifi_time_has_credentials();
    autosync_inputs_t in={
        .enabled=enabled, .has_credentials=credentials>0,
        .idle_ms=now-eligible_since, .free_bytes=free_internal(),
    };
    switch(autosync_policy_decide(&policy,&in,now)) {
    case AUTOSYNC_START:
        atomic_store(&abort_req,false);
        atomic_store(&active,true);
        // Below the UI task (5): this task mostly sleeps, and when it does not
        // it must not take a frame from the home screen.
        if(xTaskCreate(attempt_task,"autosync",4096,NULL,4,NULL)!=pdPASS) {
            atomic_store(&active,false);
            autosync_policy_record(&policy,AUTOSYNC_BUSY,now);
            ESP_LOGW(TAG,"AUTOSYNC_NO_TASK free=%u",(unsigned)in.free_bytes);
        }
        break;
    case AUTOSYNC_LOW_MEMORY:
        ESP_LOGW(TAG,"AUTOSYNC_DEFERRED low_memory free=%u floor=%u",
                 (unsigned)in.free_bytes,(unsigned)AUTOSYNC_MIN_FREE);
        break;
    case AUTOSYNC_NO_CREDENTIALS:   // the ordinary state of a device never set up
    case AUTOSYNC_WAIT:
        break;
    }
}

void net_autosync_yield(const char *why) {
    if(!atomic_load(&active)) return;
    int64_t t0=esp_timer_get_time();
    atomic_store(&abort_req,true);
    // Polled, not signalled: the wait is a slice plus a radio teardown, and a
    // 10 ms poll costs nothing next to either. A notification would need the
    // task handle of a task that deletes itself.
    while(atomic_load(&active) && esp_timer_get_time()-t0<AUTOSYNC_YIELD_WAIT_MS*1000LL)
        vTaskDelay(pdMS_TO_TICKS(10));
    bool late=atomic_load(&active);
    // `late` means the caller goes ahead with the radio still up. It builds a
    // guest into less room than it expects, and if that fails it fails as a
    // START_FAILED like any other -- this line is how that gets traced here.
    ESP_LOGI(TAG,"AUTOSYNC_YIELD %s waited_ms=%u%s free=%u",why?why:"?",
             (unsigned)((esp_timer_get_time()-t0)/1000),late?" LATE":"",
             (unsigned)free_internal());
}

#endif // ESP_PLATFORM
