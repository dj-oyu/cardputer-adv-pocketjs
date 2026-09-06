#include "wifi_time.h"
#include "solar_time.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "wifi";

#define WIFI_NAMESPACE "wifi"
#define WIFI_KEY_SSID  "ssid"
#define WIFI_KEY_PSK   "psk"

// docs/common-api.md:371 budgets 15 s for a Wi-Fi connection and 30 s for the
// longest wireless wait. The two below sit inside that: everything from
// esp_wifi_start() to an address, then everything from there to a set clock.
#define CONNECT_TIMEOUT_MS 15000
#define SNTP_TIMEOUT_MS    10000

// Association is retried because a first attempt loses to a busy channel often
// enough to be worth one more try, but a rejected key is not retried at all —
// see terminal_reason().
#define CONNECT_ATTEMPTS 3

#define NTP_SERVER "pool.ntp.org"

#define BIT_GOT_IP  BIT0
#define BIT_GIVEN_UP BIT1

// -------------------------------------------------------------- shared state

// The sync task is the only writer; shell tasks read it every frame to draw.
// A spinlock rather than a mutex because the copy is a few words and the reader
// must never block the frame it is drawing.
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_time_status_t status;
static atomic_bool running;

static void status_stage(wifi_time_stage_t stage, int reason) {
    taskENTER_CRITICAL(&status_lock);
    status.stage=stage; status.reason=reason;
    taskEXIT_CRITICAL(&status_lock);
}

static void status_state(wifi_time_state_t state) {
    taskENTER_CRITICAL(&status_lock);
    status.state=state;
    taskEXIT_CRITICAL(&status_lock);
}

wifi_time_status_t wifi_time_status(void) {
    taskENTER_CRITICAL(&status_lock);
    wifi_time_status_t copy=status;
    taskEXIT_CRITICAL(&status_lock);
    return copy;
}

const char *wifi_time_stage_name(wifi_time_stage_t stage) {
    switch(stage) {
    case WIFI_TIME_STAGE_NVS:  return "nvs";
    case WIFI_TIME_STAGE_INIT: return "init";
    case WIFI_TIME_STAGE_ASSOC:return "assoc";
    case WIFI_TIME_STAGE_AUTH: return "auth";
    case WIFI_TIME_STAGE_DHCP: return "dhcp";
    case WIFI_TIME_STAGE_SNTP: return "sntp";
    case WIFI_TIME_STAGE_NONE: break;
    }
    return "none";
}

// --------------------------------------------------------------- credentials
//
// nvs_flash_init() is idempotent and cheap, so this module calls it rather than
// assuming someone else did. shell_init() (shell.c:67) also calls it, but from
// inside an && chain whose failure is discarded — see the note in the report.
static esp_err_t nvs_ready(void) {
    esp_err_t err=nvs_flash_init();
    // NO_FREE_PAGES / NEW_VERSION_FOUND are recoverable only by erasing the
    // partition, which would also take the home settings, the SKK preferences
    // and every app's key-value store with it. That is a decision for a user
    // facing screen, not for a clock that wants the time; report and stop.
    return err;
}

esp_err_t wifi_time_credentials_set(const char *ssid, const char *psk) {
    if(!ssid||!psk) return ESP_ERR_INVALID_ARG;
    size_t ssid_len=strlen(ssid), psk_len=strlen(psk);
    if(ssid_len==0||ssid_len>WIFI_TIME_SSID_MAX) return ESP_ERR_INVALID_SIZE;
    if(psk_len>WIFI_TIME_PSK_MAX) return ESP_ERR_INVALID_SIZE;
    // An empty PSK is an open network, which is a real thing to want. Anything
    // between 1 and 7 characters is not a WPA passphrase and would only fail
    // later as a handshake timeout, which reads as a hardware problem.
    if(psk_len>0&&psk_len<8) return ESP_ERR_INVALID_SIZE;

    esp_err_t err=nvs_ready();
    if(err!=ESP_OK) return err;
    nvs_handle_t h;
    err=nvs_open(WIFI_NAMESPACE,NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    err=nvs_set_str(h,WIFI_KEY_SSID,ssid);
    if(err==ESP_OK) err=nvs_set_str(h,WIFI_KEY_PSK,psk);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    // The SSID is broadcast by the AP; the key never appears here or anywhere
    // else, not even as a length, which would narrow a search.
    if(err==ESP_OK) ESP_LOGI(TAG,"WIFI_CREDENTIALS_SAVED ssid=%s",ssid);
    else ESP_LOGW(TAG,"WIFI_CREDENTIALS_FAILED err=%d",(int)err);
    return err;
}

esp_err_t wifi_time_credentials_clear(void) {
    esp_err_t err=nvs_ready();
    if(err!=ESP_OK) return err;
    nvs_handle_t h;
    err=nvs_open(WIFI_NAMESPACE,NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    // ERASE of an absent key is not a failure to a caller who wants it gone.
    esp_err_t a=nvs_erase_key(h,WIFI_KEY_SSID), b=nvs_erase_key(h,WIFI_KEY_PSK);
    if(a==ESP_ERR_NVS_NOT_FOUND) a=ESP_OK;
    if(b==ESP_ERR_NVS_NOT_FOUND) b=ESP_OK;
    err=(a!=ESP_OK)?a:b;
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK) ESP_LOGI(TAG,"WIFI_CREDENTIALS_CLEARED");
    return err;
}

esp_err_t wifi_time_ssid_get(char *out, size_t size) {
    if(!out||size==0) return ESP_ERR_INVALID_ARG;
    out[0]='\0';
    esp_err_t err=nvs_ready();
    if(err!=ESP_OK) return err;
    nvs_handle_t h;
    err=nvs_open(WIFI_NAMESPACE,NVS_READONLY,&h);
    if(err!=ESP_OK) return err;
    size_t len=size;
    err=nvs_get_str(h,WIFI_KEY_SSID,out,&len);
    nvs_close(h);
    if(err!=ESP_OK) out[0]='\0';
    return err;
}

bool wifi_time_has_credentials(void) {
    char ssid[WIFI_TIME_SSID_MAX+1];
    return wifi_time_ssid_get(ssid,sizeof ssid)==ESP_OK&&ssid[0]!='\0';
}

// ------------------------------------------------------------------- attempt

static EventGroupHandle_t events;
static esp_netif_t *sta_netif;
static esp_event_handler_instance_t wifi_handler, ip_handler;
// The default event loop may already belong to something else by the time this
// runs. Only the creator deletes it.
static bool owns_event_loop;
static unsigned attempts_left;

// The disconnect reason is the only evidence of what actually went wrong, and
// the split below is what the UI needs: "the network is not there" sends the
// user to the SSID field, "the network said no" sends them to the password.
static wifi_time_stage_t reason_stage(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return WIFI_TIME_STAGE_AUTH;
    default:
        return WIFI_TIME_STAGE_ASSOC;
    }
}

// Retrying a key the AP has already rejected gains nothing and, on APs that
// count failures, costs a temporary lockout that makes the next honest attempt
// look like a different fault.
static bool terminal_reason(uint8_t reason) {
    return reason==WIFI_REASON_AUTH_FAIL||
           reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT||
           reason==WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if(id==WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if(id==WIFI_EVENT_STA_CONNECTED) {
        // Associated and authenticated; anything that fails from here is the
        // address, not the credentials.
        status_stage(WIFI_TIME_STAGE_DHCP,0);
    } else if(id==WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e=data;
        status_stage(reason_stage(e->reason),e->reason);
        if(terminal_reason(e->reason)||attempts_left==0) {
            xEventGroupSetBits(events,BIT_GIVEN_UP);
        } else {
            attempts_left--;
            esp_wifi_connect();
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if(id!=IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *e=data;
    taskENTER_CRITICAL(&status_lock);
    snprintf(status.ip,sizeof status.ip,IPSTR,IP2STR(&e->ip_info.ip));
    taskEXIT_CRITICAL(&status_lock);
    xEventGroupSetBits(events,BIT_GOT_IP);
}

// Undoes exactly what bring_up() managed to do, in reverse, and is safe to call
// after a partial failure. esp_netif_deinit() is deliberately absent: ESP-IDF
// v6.0.1 documents it as unsupported and it returns ESP_ERR_NOT_SUPPORTED, so
// the LWIP task and its buffers survive the first sync for the life of the boot.
static void tear_down(bool wifi_inited, bool wifi_started) {
    esp_netif_sntp_deinit();
    if(wifi_started) { esp_wifi_disconnect(); esp_wifi_stop(); }
    if(ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,ip_handler);
        ip_handler=NULL;
    }
    if(wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_handler);
        wifi_handler=NULL;
    }
    if(wifi_inited) esp_wifi_deinit();
    if(sta_netif) { esp_netif_destroy_default_wifi(sta_netif); sta_netif=NULL; }
    if(owns_event_loop) { esp_event_loop_delete_default(); owns_event_loop=false; }
}

static void finish(wifi_time_stage_t stage, int reason) {
    status_stage(stage,reason);
    status_state(WIFI_TIME_FAILED);
    ESP_LOGW(TAG,"SYNC_FAILED stage=%s reason=%d",wifi_time_stage_name(stage),reason);
}

static void sync_task(void *arg) {
    (void)arg;
    bool wifi_inited=false, wifi_started=false;
    esp_err_t err;

    taskENTER_CRITICAL(&status_lock);
    status=(wifi_time_status_t){.state=WIFI_TIME_RUNNING,.stage=WIFI_TIME_STAGE_NVS};
    taskEXIT_CRITICAL(&status_lock);

    char ssid[WIFI_TIME_SSID_MAX+1]="";
    // Zeroed again before this frame goes away; see the wipe at the end.
    char psk[WIFI_TIME_PSK_MAX+1]="";
    {
        err=nvs_ready();
        nvs_handle_t h;
        if(err==ESP_OK) err=nvs_open(WIFI_NAMESPACE,NVS_READONLY,&h);
        if(err==ESP_OK) {
            size_t len=sizeof ssid;
            err=nvs_get_str(h,WIFI_KEY_SSID,ssid,&len);
            if(err==ESP_OK) {
                len=sizeof psk;
                esp_err_t p=nvs_get_str(h,WIFI_KEY_PSK,psk,&len);
                // A saved open network has no psk key at all.
                if(p==ESP_ERR_NVS_NOT_FOUND) psk[0]='\0';
                else if(p!=ESP_OK) err=p;
            }
            nvs_close(h);
        }
        if(err!=ESP_OK||ssid[0]=='\0') {
            finish(WIFI_TIME_STAGE_NVS,err==ESP_OK?ESP_ERR_NVS_NOT_FOUND:(int)err);
            goto done;
        }
    }

    ESP_LOGI(TAG,"SYNC_START ssid=%s",ssid);
    status_stage(WIFI_TIME_STAGE_INIT,0);

    events=xEventGroupCreate();
    if(!events) { finish(WIFI_TIME_STAGE_INIT,ESP_ERR_NO_MEM); goto done; }
    attempts_left=CONNECT_ATTEMPTS-1;

    err=esp_netif_init();
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }
    err=esp_event_loop_create_default();
    if(err==ESP_OK) owns_event_loop=true;
    else if(err!=ESP_ERR_INVALID_STATE) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }

    sta_netif=esp_netif_create_default_wifi_sta();
    if(!sta_netif) { finish(WIFI_TIME_STAGE_INIT,ESP_ERR_NO_MEM); goto done; }

    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    err=esp_wifi_init(&init);
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }
    wifi_inited=true;

    err=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,on_wifi,NULL,&wifi_handler);
    if(err==ESP_OK)
        err=esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,on_ip,NULL,&ip_handler);
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }

    // RAM storage keeps the driver from writing a second copy of the key into
    // its own NVS namespace, where nothing in this module could clear it.
    err=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(err==ESP_OK) err=esp_wifi_set_mode(WIFI_MODE_STA);
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }

    wifi_config_t cfg={0};
    // Not snprintf: these are wire fields, 32 and 64 bytes with no NUL, and a
    // 32 character SSID is legal. The struct is already zeroed, so copying the
    // bytes and leaving the rest is both shorter and the only correct form.
    memcpy(cfg.sta.ssid,ssid,strnlen(ssid,sizeof cfg.sta.ssid));
    memcpy(cfg.sta.password,psk,strnlen(psk,sizeof cfg.sta.password));
    // Accepting anything from open upward lets one stored network work on a WEP
    // guest AP and on WPA3; the AP decides, and a threshold here would only turn
    // a working network into an unexplained "not found".
    cfg.sta.threshold.authmode=WIFI_AUTH_OPEN;
    err=esp_wifi_set_config(WIFI_IF_STA,&cfg);
    memset(&cfg,0,sizeof cfg);
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }

    status_stage(WIFI_TIME_STAGE_ASSOC,0);
    err=esp_wifi_start();          // STA_START connects; see on_wifi()
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_INIT,err); goto done; }
    wifi_started=true;

    EventBits_t bits=xEventGroupWaitBits(events,BIT_GOT_IP|BIT_GIVEN_UP,pdFALSE,pdFALSE,
                                         pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if(!(bits&BIT_GOT_IP)) {
        // The stage already says where it stalled: ASSOC if the AP never
        // answered, DHCP if it did and the address never arrived.
        wifi_time_status_t now=wifi_time_status();
        finish(now.stage,now.reason);
        goto done;
    }

    wifi_ap_record_t ap;
    if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK) {
        taskENTER_CRITICAL(&status_lock);
        status.rssi=ap.rssi;
        taskEXIT_CRITICAL(&status_lock);
    }
    {
        wifi_time_status_t now=wifi_time_status();
        ESP_LOGI(TAG,"SYNC_CONNECTED ip=%s rssi=%d",now.ip,now.rssi);
    }

    status_stage(WIFI_TIME_STAGE_SNTP,0);
    esp_sntp_config_t sntp=ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    // IMMED, not smooth: settimeofday() steps the clock once and solar_time.c
    // reads it on the next frame. docs/solar-sail.md asks that a correction be
    // complete before it is announced, and a step is complete when it returns.
    sntp.smooth_sync=false;
    err=esp_netif_sntp_init(&sntp);
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_SNTP,err); goto done; }
    err=esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS));
    if(err!=ESP_OK) { finish(WIFI_TIME_STAGE_SNTP,err); goto done; }

    // The only call site. solar_time_set_synchronized(false) is never made
    // here: docs/solar-sail.md:48-50 keeps a clock that was once set running
    // through a disconnect, and only a clock that has become untrustworthy is
    // withdrawn — a radio going away does not make the seconds wrong.
    solar_time_set_synchronized(true);
    status_stage(WIFI_TIME_STAGE_NONE,0);
    status_state(WIFI_TIME_OK);
    {
        struct timeval tv;
        gettimeofday(&tv,NULL);
        ESP_LOGI(TAG,"SYNC_OK epoch=%lld",(long long)tv.tv_sec);
    }

done:
    // The passphrase leaves RAM before the radio does. A plain memset on a dead
    // local is exactly what a compiler is allowed to drop, so the write goes
    // through a volatile pointer.
    for(volatile char *p=psk;p<psk+sizeof psk;p++) *p=0;
    tear_down(wifi_inited,wifi_started);
    if(events) { vEventGroupDelete(events); events=NULL; }
    ESP_LOGI(TAG,"SYNC_DONE state=%d free=%u",
             (int)wifi_time_status().state,
             (unsigned)esp_get_free_heap_size());
    atomic_store(&running,false);
    vTaskDelete(NULL);
}

esp_err_t wifi_time_sync_start(void) {
    bool expected=false;
    if(!atomic_compare_exchange_strong(&running,&expected,true))
        return ESP_ERR_INVALID_STATE;
    // 4 KiB covers this task's own frames; the driver and LWIP run on their own
    // tasks and are sized by Kconfig, not from here.
    if(xTaskCreate(sync_task,"wifi_time",4096,NULL,5,NULL)!=pdPASS) {
        atomic_store(&running,false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
