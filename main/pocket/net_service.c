#include "net_service.h"
#include "wifi_time.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

static const char *TAG = "net_service";

// A mutex, not a spinlock: starting the link creates a task, and that cannot
// happen inside a critical section. It is held across the start and the stop
// so that a release racing an acquire cannot see "0 holders, stop" between the
// acquire's count and its start -- wifi_time_link_start_for() clears any stop
// request, and a stop landing just before it would otherwise be lost, leaving a
// link up with no one holding it. Static storage: created once at boot and
// never freed, so it costs 84 B of .bss (the map) instead of a heap block.
#ifdef ESP_PLATFORM
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static void lock_take(void) { if(lock) xSemaphoreTake(lock,portMAX_DELAY); }
static void lock_give(void) { if(lock) xSemaphoreGive(lock); }
void net_service_init(void) { if(!lock) lock=xSemaphoreCreateMutexStatic(&lock_storage); }
#else
// The host test is single threaded; the ordering above is what it checks.
static void lock_take(void) {}
static void lock_give(void) {}
void net_service_init(void) {}
#endif

static struct {
    const char *name;
    unsigned    count;
} reasons[NET_SERVICE_REASONS];
static unsigned total;

static int find(const char *reason) {
    for(int i=0;i<NET_SERVICE_REASONS;i++)
        if(reasons[i].name && !strcmp(reasons[i].name,reason)) return i;
    return -1;
}

esp_err_t net_service_acquire(const char *reason) {
    if(!reason || !reason[0]) return ESP_ERR_INVALID_ARG;
    lock_take();
    int at=find(reason);
    if(at<0) {
        for(int i=0;i<NET_SERVICE_REASONS;i++)
            if(!reasons[i].name) { at=i; break; }
        if(at<0) { lock_give(); return ESP_ERR_NO_MEM; }
    }
    // A link is started whenever this service does not have one running, not
    // only on the first hold: a link that failed has exited on its own while
    // its holders still hold, and a new acquire is how a holder asks again.
    // One that is still ours -- up, connecting, or on its way down after the
    // last release -- is left alone; in the last case the start below would
    // have been refused by the radio lock anyway.
    if(wifi_time_link_owner()!=WIFI_TIME_LINK_SERVICE) {
        esp_err_t err=wifi_time_link_start_for(WIFI_TIME_LINK_SERVICE);
        if(err!=ESP_OK) {
            lock_give();
            ESP_LOGW(TAG,"NET_ACQUIRE_REFUSED %s %s",reason,esp_err_to_name(err));
            return err;
        }
    } else if(total==0) {
        // Ours, but the last holder already asked for it back and the task has
        // not finished tearing down. Counting a hold on a link that is about to
        // disappear would report UP to nobody and FAILED a moment later; the
        // radio is busy, which is what the caller is told.
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }
    if(!reasons[at].name) reasons[at].name=reason;
    reasons[at].count++;
    total++;
    lock_give();
    ESP_LOGI(TAG,"NET_ACQUIRE %s holders=%u",reason,total);
    return ESP_OK;
}

esp_err_t net_service_release(const char *reason) {
    if(!reason || !reason[0]) return ESP_ERR_INVALID_ARG;
    lock_take();
    int at=find(reason);
    if(at<0 || reasons[at].count==0) {
        lock_give();
        // Refused rather than clamped: a stray release is a bug in its caller,
        // and letting it through would stop the radio under another holder.
        ESP_LOGW(TAG,"NET_RELEASE_UNMATCHED %s",reason);
        return ESP_ERR_INVALID_STATE;
    }
    if(--reasons[at].count==0) reasons[at].name=NULL;
    if(--total==0) wifi_time_link_stop_for(WIFI_TIME_LINK_SERVICE);
    unsigned left=total;
    lock_give();
    ESP_LOGI(TAG,"NET_RELEASE %s holders=%u",reason,left);
    return ESP_OK;
}

unsigned net_service_holders(void) { return total; }

net_service_state_t net_service_state(void) {
    if(total==0) return NET_SERVICE_OFF;
    // Held, so the link is either ours and moving, or gone. A failed attempt
    // ends DOWN rather than FAILED in wifi_time.c (FAILED there means "was up
    // and was lost"), and the owner is cleared as the task exits; either way a
    // holder that has no link is a holder whose link failed.
    if(wifi_time_link_owner()!=WIFI_TIME_LINK_SERVICE) return NET_SERVICE_FAILED;
    switch(wifi_time_link_state()) {
    case WIFI_TIME_LINK_CONNECTING: return NET_SERVICE_CONNECTING;
    case WIFI_TIME_LINK_UP:         return NET_SERVICE_UP;
    default:                        return NET_SERVICE_FAILED;
    }
}

const char *net_service_state_name(net_service_state_t state) {
    switch(state) {
    case NET_SERVICE_OFF:        return "off";
    case NET_SERVICE_CONNECTING: return "connecting";
    case NET_SERVICE_UP:         return "up";
    case NET_SERVICE_FAILED:     return "failed";
    }
    return "?";
}

void net_service_describe(char *out, size_t size) {
    if(!out || !size) return;
    out[0]='\0';
    size_t used=0;
    lock_take();
    for(int i=0;i<NET_SERVICE_REASONS;i++) {
        if(!reasons[i].name) continue;
        int n=snprintf(out+used,size-used,used?" %s=%u":"%s=%u",
                       reasons[i].name,reasons[i].count);
        if(n<0 || (size_t)n>=size-used) break;
        used+=(size_t)n;
    }
    lock_give();
}

// ------------------------------------------------------------ notification

static struct {
    net_service_listener_t fn;
    void *user;
} listeners[NET_SERVICE_LISTENERS];
static net_service_state_t reported=NET_SERVICE_OFF;

bool net_service_listen(net_service_listener_t fn, void *user) {
    if(!fn) return false;
    for(int i=0;i<NET_SERVICE_LISTENERS;i++)
        if(!listeners[i].fn) { listeners[i].fn=fn; listeners[i].user=user; return true; }
    return false;
}

void net_service_unlisten(net_service_listener_t fn, void *user) {
    for(int i=0;i<NET_SERVICE_LISTENERS;i++)
        if(listeners[i].fn==fn && listeners[i].user==user) listeners[i].fn=NULL;
}

void net_service_pump(void) {
    net_service_state_t now=net_service_state();
    if(now==reported) return;
    reported=now;
    for(int i=0;i<NET_SERVICE_LISTENERS;i++)
        if(listeners[i].fn) listeners[i].fn(now,listeners[i].user);
}
