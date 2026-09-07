#include "pocket_net.h"
#include "pocket_api.h"
#include "wifi_time.h"
#include "solar_time.h"
#include "esp_crt_bundle.h"

#include "esp_tls_errors.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pocket.net";

// Every number below is enforced somewhere in this file and published through
// capability.limits, which is the whole point of section 2: an app has to be
// able to find out what it is getting before it asks for it. Nothing here comes
// from section 14's proposals except where the code happens to agree with them.

#define NET_LEASE_WATCHES      4       // onChange listeners on one lease
#define NET_URL_MAX          256
#define NET_REQ_HEADERS_MAX    8
#define NET_REQ_HEADER_BYTES 2048      // keys + values + a NUL each
#define NET_REQ_BODY_MAX    4096
#define NET_RSP_HEADERS_MAX   16
#define NET_RSP_HEADER_BYTES 1024
#define NET_READ_MAX        1024       // one read() hands back at most this
#define NET_RESPONSE_MAX   (64*1024)   // total body an app may take from one
                                       // response before it is cut off

#define NET_TIMEOUT_MAX_MS       30000
#define NET_ACQUIRE_TIMEOUT_MS   20000  // the link's own budget is 15 s; this
                                        // sits above it so the refusal carries
                                        // the stage rather than a bare TIMEOUT
#define NET_SCAN_TIMEOUT_MS       8000  // radio up, one channel sweep, down
#define NET_REQUEST_TIMEOUT_MS   15000
#define NET_READ_TIMEOUT_MS       5000

// The headroom a TLS handshake needs before it is worth starting. mbedTLS at
// this build's defaults wants a 16 KiB record buffer, a 4 KiB output buffer and
// its context and parsed chain on top -- call it 30 KiB, of which one block is
// 16 KiB. Both numbers below started as estimates from those defaults. What has
// since been measured (see TLSCOST below) is that they are checked about 14 KB
// before the allocation that actually fails, because esp_http_client builds
// itself in between: at 41,300 free here, mbedtls_ssl_setup was reached with
// 27,232 and returned ALLOC_FAILED. So the margin is not the slack it looks
// like. They remain deliberately above the handshake: an app that is refused
// here is told OUT_OF_MEMORY with a number it can act on, whereas a handshake
// that runs the internal heap out takes the whole session down. Free heap at
// the home screen is about 222 KiB, a linked radio costs about 37 KiB of that,
// and a guest that has grown into its 144 KiB cap can leave less than this.
// Measured 2026-09-08 against https://example.com/ with CONFIG_MBEDTLS_DYNAMIC_BUFFER
// on. The handshake succeeded and cost 7,172 bytes of free heap -- a quarter of
// the 30 KiB the paragraph above guessed -- but it took 15,360 out of the
// largest block, which is the number that actually binds. At the gate there
// were about 46 KiB free and 31,744 in the largest block; by the time
// esp_http_client_open ran, 32,328 and 23,552. Both thresholds are set from
// that with margin, and both are checked roughly 14 KB before the allocation
// they are protecting.
//
// One server, one certificate chain, one run. A longer chain wants more, so
// these stay above what was measured rather than at it.
//
// What was called fragmentation for most of a day was mostly these thresholds.
// Instrumenting each stage of a request showed the largest free block is
// 31,744 at task creation, stays 31,744 through the handshake, dips during the
// body read, and is 31,744 again after esp_http_client_cleanup -- for nine
// consecutive requests. A request leaves nothing behind. What declined was the
// free heap, by about 2,100 a round, and that was the test app's own promise
// chain growing the guest heap, which comes out of the same pool. Twelve of
// twelve requests succeed with the numbers below.
//
// Making the worker task persistent instead of one per request was tried and
// measured: the block held its maximum for five rounds instead of three, and
// the ninth request was refused exactly as before. It bought no requests, so
// it was reverted rather than kept for the graph. What would actually fix this
// is holding the connection and its TLS context across requests, which section
// 11 does not currently describe.
//
// There is a way out an app can take today, and it works completely: close the
// lease and acquire it again. Measured over sixteen requests with one cycle in
// the middle -- after eight, free was 39,660 and the block 29,696 and falling;
// after close, three seconds, and a fresh acquire, 57,104 and 31,744, which are
// the round-zero numbers exactly. Eight more requests then succeeded. So the
// fragmentation lives entirely in allocations the net stack gives back when the
// radio comes down, and nothing accumulates across a lease. apps/netcheck
// documents the recipe; it is deliberately not done automatically here, because
// dropping an app's link underneath it costs three seconds and is a decision
// the app should make rather than discover.
#define NET_TLS_MIN_FREE   (20*1024)
// Raised, not lowered, by the measurement: 20 KiB here would have left about
// 12 KiB by the time the 15,360 byte allocation was made, and it would have
// failed. The old value was the one genuinely wrong number of the two.
// Both numbers were sized for a world where mbedTLS allocated from the general
// heap. A dedicated 20 KiB arena for mbedTLS was built and measured and then
// withdrawn: it removed mbedTLS from the general heap entirely and made things
// worse, because taking 20 KiB of one piece costs more contiguity than the
// ninety small blocks it removed. Eight consecutive requests without it, six
// with it lazily taken, seven with it taken at lease time. What survives from
// that work is these two numbers, measured with the arena in place and still
// true without it: a handshake wants about 6,600 to 8,300 bytes of free heap
// and its largest single allocation is 4,437.
#define NET_TLS_MIN_BLOCK  (8*1024)
// Plain HTTP is a socket, a 512 byte client buffer and this file's two arenas.
#define NET_PLAIN_MIN_FREE (12*1024)
// Measured on the board, 2026-09-07, with the probe in wifi_time.c: esp_wifi_init
// costs 27,192 bytes, and the netif and event loop it needs first cost about
// 21,000 more. Both are freed again when the radio comes down, so this is a
// recurring price and not a one-off. LWIP itself is not in the number: v6.0.1
// has no esp_netif_deinit, so the stack stays up for the life of the boot and is
// already paid for by the time any app asks.
//
// The two above are for a request on a link that is already up. This one is the
// cost of bringing one up, which is four times larger, and using the smaller
// number here is what made net.wifi claim to be available at 30 KB free.
#define NET_RADIO_MIN_FREE (56*1024)

// The one Wi-Fi profile this host has: the SSID and key the settings screen
// stores in NVS. Section 11 makes profileId a reference to host configuration
// precisely so that the passphrase never has to reach the app, and this device
// keeps exactly one.
#define NET_PROFILE_ID "default"

// ------------------------------------------------------------------ options
//
// Section 4's Options, read the same way pocket_av.c reads them. The token is
// kept rather than observed once: a request outlives many frames and the pump
// polls the token for the whole of it.

typedef struct {
    int32_t timeout_ms;      // 0 for "use the default"
    JSValue cancel;          // JS_UNDEFINED when none was passed
    bool    cancelled;       // already cancelled at the call
} net_options_t;

static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, net_options_t *out) {
    out->timeout_ms=0;
    out->cancel=JS_UNDEFINED;
    out->cancelled=false;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        // Section 4 refuses to round an over-range request quietly.
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms ||
           ms<1 || ms>NET_TIMEOUT_MAX_MS)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "timeoutMs must be a whole number of 1 to 30000",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
        out->cancel=cancel;      // kept, and freed when the operation settles
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

static int64_t deadline_from(const net_options_t *o, int32_t fallback_ms) {
    return esp_timer_get_time()+1000LL*(o->timeout_ms?o->timeout_ms:fallback_ms);
}

// ------------------------------------------------------------------- state

static JSClassID lease_class, response_class;

// The lease. One at a time, and a second acquire of the same profile shares it
// rather than opening another, which is what section 11 means by reference
// sharing. `handle` is what a lease object carries in its opaque, so a lease
// the app kept past close() is inert rather than dangerous.
static struct {
    uint32_t         handle;        // 0 when no lease exists
    uint32_t         next_handle;
    unsigned         refs;          // live lease objects
    pocket_request_t acquire;       // the Promise waiting for the link
    wifi_time_link_t reported;      // the state onChange last saw
    // Set when the lease was given back while the worker still owned the
    // socket. The worker takes the radio down on its way out; see the header
    // for why nothing blocks the frame waiting for it.
    bool             stop_when_idle;
} lease;

static struct {
    pocket_request_t request;
} scan;

// The HTTP worker's command word. Written by the JS task, read by the worker.
#define NET_CMD_NONE  0u
#define NET_CMD_READ  1u
#define NET_CMD_CLOSE 2u

// Statuses the worker posts besides POCKET_STATUS_OK. A read posts the byte
// count it produced, so only the negatives below are special.
#define NET_STATUS_TRANSPORT (-1)
#define NET_STATUS_TLS       (-2)
#define NET_STATUS_DROPPED   (-3)   // the link went away under the request

// Everything the worker and the JS task share. The JS task fills the request
// half before the task exists and reads the response half only after a
// completion has landed, so the only genuinely concurrent fields are the
// atomics.
static struct {
    atomic_bool  alive;          // a worker task exists
    atomic_bool  abort;          // stop() asked; checked between steps
    atomic_uint  command;
    atomic_uint  read_request;   // the Promise a pending read settles
    pocket_request_t open;       // the Promise waiting for the headers

    uint32_t     generation;     // bumped per request; a response object's
                                 // opaque holds it, so a stale one is inert
    bool         open_response;  // a response object the app has not closed

    // request
    char         url[NET_URL_MAX+1];
    esp_http_client_method_t method;
    bool         tls;
    int32_t      timeout_ms;
    char        *req_headers;    // key\0value\0 ... , NET_REQ_HEADER_BYTES
    size_t       req_headers_len;
    uint8_t     *body;
    size_t       body_len;

    // response
    int          status_code;
    char        *rsp_headers;    // key\0value\0 ... , NET_RSP_HEADER_BYTES
    size_t       rsp_headers_len;
    unsigned     rsp_header_count;
    bool         headers_truncated;
    uint8_t     *chunk;          // NET_READ_MAX
    int          want;           // bytes the pending read asked for
    uint32_t     received;       // bytes handed to the app so far
    bool         eof;
} http;

// Created once and never deleted, so that giving it is safe whether or not a
// worker exists. A task handle would have to be checked against a task that may
// have just deleted itself, and there is no way to do that without a lock.
static SemaphoreHandle_t wake;

// ------------------------------------------------------------ URL checking

static bool host_is_private(const char *host, size_t len) {
    char tmp[48];
    if(len==0 || len>=sizeof tmp) return false;
    memcpy(tmp,host,len); tmp[len]='\0';
    char *colon=strchr(tmp,':');
    if(colon) *colon='\0';
    unsigned a,b,c,d; char extra;
    if(sscanf(tmp,"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra)!=4) return false;
    if(a>255||b>255||c>255||d>255) return false;
    return a==10 || a==127 ||
           (a==192&&b==168) ||
           (a==172&&b>=16&&b<=31) ||
           (a==169&&b==254);         // link-local, for a device handing out DHCP
}

// Returns NULL when this host will open the URL, or the PocketError code that
// says why not. Section 11 wants the destination checked against what the app
// is registered for; there is no per-app registration on this firmware yet, so
// the standing rule is the one that can be enforced without one: TLS to
// anywhere, plaintext only to an address on the local network.
static const char *url_check(const char *url, bool *tls, const char **why) {
    size_t n=strlen(url);
    if(n==0 || n>NET_URL_MAX) { *why="url must be 1 to 256 bytes"; return POCKET_ERR_INVALID_ARGUMENT; }
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)url[i];
        // A space or a control byte in a URL is either a mistake or an attempt
        // to smuggle a second request line past the client.
        if(c<0x21 || c>0x7e) { *why="url must be printable ASCII with no spaces"; return POCKET_ERR_INVALID_ARGUMENT; }
    }
    const char *rest;
    if(!strncmp(url,"https://",8)) { *tls=true;  rest=url+8; }
    else if(!strncmp(url,"http://",7)) { *tls=false; rest=url+7; }
    else { *why="url must be http:// or https://"; return POCKET_ERR_INVALID_ARGUMENT; }

    size_t host_len=strcspn(rest,"/?#");
    if(host_len==0) { *why="url has no host"; return POCKET_ERR_INVALID_ARGUMENT; }
    // Credentials in the authority cannot be carried without either logging
    // them somewhere or stripping them silently, and section 7 forbids the
    // first. Refused rather than quietly rewritten.
    if(memchr(rest,'@',host_len)) {
        *why="a user:password in the url is not accepted";
        return POCKET_ERR_PERMISSION_DENIED;
    }
    if(*tls) {
        // Section 11: certificate validation needs a clock, and a wrong clock
        // rejects a good certificate or accepts an expired one.
        // UTC is solar_time.c's "the wall clock is believable", whether it was
        // set by SNTP this run or carried across a reset by the RTC. That is
        // the same question a certificate's validity window asks.
        if(solar_time_now(0).source!=SOLAR_TIME_UTC) {
            *why="the clock is not set, so a certificate cannot be checked";
            return POCKET_ERR_TLS_ERROR;
        }
        return NULL;
    }
    if(!host_is_private(rest,host_len)) {
        *why="plaintext http is allowed only to a private address";
        return POCKET_ERR_PERMISSION_DENIED;
    }
    return NULL;
}

// ------------------------------------------------------------- the worker
//
// One task per request, for the length of that request and its body reads. It
// touches no JS value: results go back through pocket_api_complete(), which the
// header of pocket_api.h explains is safe from any task.

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if(evt->event_id!=HTTP_EVENT_ON_HEADER) return ESP_OK;
    const char *key=evt->header_key, *value=evt->header_value;
    if(!key||!value||!http.rsp_headers) return ESP_OK;
    size_t kl=strlen(key), vl=strlen(value);
    // Section 11 lower-cases header names and does not promise multi-value
    // fidelity: a repeated name overwrites when the object is built, and a
    // header that does not fit is dropped with headersTruncated set rather than
    // silently lost.
    if(http.rsp_header_count>=NET_RSP_HEADERS_MAX ||
       http.rsp_headers_len+kl+vl+2>NET_RSP_HEADER_BYTES) {
        http.headers_truncated=true;
        return ESP_OK;
    }
    char *p=http.rsp_headers+http.rsp_headers_len;
    for(size_t i=0;i<kl;i++) p[i]=(key[i]>='A'&&key[i]<='Z')?(char)(key[i]+32):key[i];
    p[kl]='\0';
    memcpy(p+kl+1,value,vl);
    p[kl+1+vl]='\0';
    http.rsp_headers_len+=kl+vl+2;
    http.rsp_header_count++;
    return ESP_OK;
}

static bool link_is_up(void) {
    return wifi_time_link_state()==WIFI_TIME_LINK_UP;
}

static void http_task(void *arg) {
    (void)arg;
    esp_http_client_config_t cfg={
        .url=http.url,
        .method=http.method,
        .timeout_ms=http.timeout_ms,
        .event_handler=on_http_event,
        // Section 11: no redirects, no retries and no decompression in this
        // version. Each of those is a decision an app cannot see us make.
        .disable_auto_redirect=true,
        .max_redirection_count=0,
        // 512 bytes each way. The client only has to hold one header line and
        // whatever a read asks for goes into our own chunk.
        .buffer_size=512,
        .buffer_size_tx=512,
        .crt_bundle_attach=http.tls?esp_crt_bundle_attach:NULL,
    };
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    int32_t status=POCKET_STATUS_OK;
    if(!client) status=NET_STATUS_TRANSPORT;

    for(size_t at=0; client && at<http.req_headers_len; ) {
        const char *key=http.req_headers+at;
        const char *value=key+strlen(key)+1;
        at+=strlen(key)+strlen(value)+2;
        esp_http_client_set_header(client,key,value);
    }

    if(client) {
        // Kept rather than removed: it is the only place the handshake's real
        // appetite becomes visible, and the thresholds above were guesses until
        // it existed. Measured 2026-09-08 with the gates lowered on purpose:
        // the request-time check saw 41,300 free, this point saw 27,232, and
        // mbedtls_ssl_setup then failed with -0x008D (ALLOC_FAILED). The 14 KB
        // between the two is esp_http_client building itself, which is why a
        // threshold that looked generous against the handshake is not.
        size_t before_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        size_t before_block=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        esp_err_t err=esp_http_client_open(client,(int)http.body_len);
        size_t after_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        size_t after_block=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        ESP_LOGI(TAG,"TLSCOST open=%s free %u->%u (used %d) block %u->%u",
                 esp_err_to_name(err),(unsigned)before_free,(unsigned)after_free,
                 (int)before_free-(int)after_free,
                 (unsigned)before_block,(unsigned)after_block);
        if(err!=ESP_OK)
            // esp_http_client folds the handshake into open(), so a refused
            // connection and a rejected certificate arrive the same way and
            // only the code separates them. esp-tls numbers its generic socket
            // failures below +0x10 and everything mbedTLS reported at or above
            // it, which is exactly the line section 11 draws between an
            // IO_ERROR and a TLS_ERROR.
            status=(err>=ESP_ERR_ESP_TLS_BASE+0x10 &&
                    err<=ESP_ERR_ESP_TLS_BASE+0x1d) ||
                   err==ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT
                   ?NET_STATUS_TLS:NET_STATUS_TRANSPORT;
    }
    for(size_t sent=0; status==POCKET_STATUS_OK && sent<http.body_len; ) {
        if(atomic_load(&http.abort)) { status=NET_STATUS_TRANSPORT; break; }
        size_t take=http.body_len-sent;
        if(take>512) take=512;      // one client buffer, so an abort lands soon
        int n=esp_http_client_write(client,(const char *)http.body+sent,take);
        if(n<0) { status=NET_STATUS_TRANSPORT; break; }
        sent+=(size_t)n;
    }
    if(status==POCKET_STATUS_OK) {
        if(esp_http_client_fetch_headers(client)<0) status=NET_STATUS_TRANSPORT;
        else http.status_code=esp_http_client_get_status_code(client);
    }
    if(status==POCKET_STATUS_OK && atomic_load(&http.abort))
        status=NET_STATUS_TRANSPORT;
    // The headers are in (or they are not); either way the app's request
    // settles now and the body follows one read at a time.
    pocket_api_complete(http.open,status);

    while(status==POCKET_STATUS_OK) {
        // A stale give from the previous request would only spin this loop
        // once; the command word is what actually says there is work.
        xSemaphoreTake(wake,pdMS_TO_TICKS(200));
        if(atomic_load(&http.abort)) break;
        unsigned cmd=atomic_exchange(&http.command,NET_CMD_NONE);
        if(cmd==NET_CMD_CLOSE) break;
        if(cmd!=NET_CMD_READ) continue;
        pocket_request_t req=(pocket_request_t)atomic_load(&http.read_request);
        int32_t got;
        if(!link_is_up()) got=NET_STATUS_DROPPED;
        else {
            // Each read carries its own deadline (section 14 gives a body read
            // 5 s), and the socket is what enforces it.
            esp_http_client_set_timeout_ms(client,http.timeout_ms);
            int n=esp_http_client_read(client,(char *)http.chunk,http.want);
            if(n<0) got=NET_STATUS_TRANSPORT;
            else {
                // Zero means the body is over -- or that the peer closed early,
                // which esp_http_client reports the same way. is_complete is
                // the only thing that separates them.
                if(n==0) http.eof=true;
                got=n;
            }
        }
        pocket_api_complete(req,got);
        if(got<0) break;
    }

    if(client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(http.body);        http.body=NULL;        http.body_len=0;
    free(http.req_headers); http.req_headers=NULL; http.req_headers_len=0;
    // rsp_headers and chunk belong to the response object and are freed by the
    // JS task when it closes; a worker that ends first leaves them alone.
    atomic_store(&http.alive,false);
    // After cleanup, so mbedTLS has returned everything: this is the edge on
    // which a pool whose session already ended is actually handed back.
    ESP_LOGI(TAG,"HTTP_WORKER_DONE status=%d",(int)http.status_code);
    // Last, and only here: the lease may have been given back while the socket
    // was still open, and the radio is not taken down under a live connection.
    if(lease.stop_when_idle) { lease.stop_when_idle=false; wifi_time_link_stop(); }
    vTaskDelete(NULL);
}

// Asks the worker to stop and to wake up and notice. Never waits: the caller is
// the JS task and the frame is not the place to block on a socket.
static void http_ask_stop(void) {
    atomic_store(&http.abort,true);
    atomic_store(&http.command,NET_CMD_CLOSE);
    if(wake) xSemaphoreGive(wake);
}

// ---------------------------------------------------------------- the lease

static pocket_sub_slot_t lease_slots[NET_LEASE_WATCHES];
static pocket_sub_table_t lease_table = {
    .slots=lease_slots, .count=NET_LEASE_WATCHES,
    .tag="pocket.net", .what="onChange",
    // Unlike a watch, this fires only when the link actually moves, so a
    // listener that throws has no runaway to stop. Same reasoning
    // capabilities.onChange uses.
    .close_on_throw=false,
};

static const char *link_state_name(void) {
    switch(wifi_time_link_state()) {
        case WIFI_TIME_LINK_UP:         return "connected";
        case WIFI_TIME_LINK_CONNECTING: return "connecting";
        default:                        return "disconnected";
    }
}

static JSValue link_status_object(JSContext *ctx) {
    JSValue o=JS_NewObject(ctx);
    if(JS_IsException(o)) return o;
    JS_SetPropertyStr(ctx,o,"state",JS_NewString(ctx,link_state_name()));
    char ip[16];
    wifi_time_link_ip(ip,sizeof ip);
    JS_SetPropertyStr(ctx,o,"address",ip[0]?JS_NewString(ctx,ip):JS_NULL);
    return o;
}

static bool lease_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    (void)slot; (void)user;
    *payload=link_status_object(ctx);
    return true;
}

// True when this object still names the lease that exists now.
static bool lease_live(JSValueConst self) {
    uint32_t h=(uint32_t)(uintptr_t)JS_GetOpaque(self,lease_class);
    return h!=0 && h==lease.handle;
}

// Gives the radio back, or arranges for the worker to do it. Idempotent.
static void lease_release(void) {
    lease.handle=0;
    lease.refs=0;
    if(atomic_load(&http.alive)) {
        // The app gave back the thing the request was running on, so the
        // request goes too -- and the worker, which owns the socket, is what
        // takes the radio down afterwards.
        http_ask_stop();
        lease.stop_when_idle=true;
    } else {
        wifi_time_link_stop();
    }
}

static JSValue js_lease_status(JSContext *ctx, JSValueConst self,
                               int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    if(!lease_live(self)) {
        // A closed lease is not an error to ask about; it reports what it is.
        JSValue o=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,o,"state",JS_NewString(ctx,"disconnected"));
        JS_SetPropertyStr(ctx,o,"address",JS_NULL);
        return o;
    }
    return link_status_object(ctx);
}

static JSValue js_lease_on_change(JSContext *ctx, JSValueConst self,
                                  int argc, JSValueConst *argv) {
    if(!lease_live(self))
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"net.wifi.onChange",
                                "the lease is closed",false,NULL);
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"net.wifi.onChange",
                                "onChange(listener) needs a function",false,NULL);
    return pocket_api_sub_open(ctx,&lease_table,argv[0],"net.wifi.onChange",
                               "too many listeners",NULL);
}

static JSValue js_lease_close(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    if(!lease_live(self)) return JS_UNDEFINED;   // close() is idempotent
    JS_SetOpaque((JSValue)self,NULL);
    if(--lease.refs==0) {
        pocket_api_sub_close_all(&lease_table);
        lease_release();
        ESP_LOGI(TAG,"LEASE_CLOSED");
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry lease_methods[] = {
    JS_CFUNC_DEF("status",   0, js_lease_status),
    JS_CFUNC_DEF("onChange", 1, js_lease_on_change),
    JS_CFUNC_DEF("close",    0, js_lease_close),
};
static const JSClassDef lease_class_def = { .class_name="WifiLease" };

static JSValue lease_object(JSContext *ctx) {
    JSValue o=JS_NewObjectClass(ctx,lease_class);
    if(JS_IsException(o)) return o;
    JS_SetOpaque(o,(void *)(uintptr_t)lease.handle);
    return o;
}

// ------------------------------------------------------------ wifi.acquire

static void acquire_stop(void *user, const char *code) {
    (void)user; (void)code;
    // Section 4 gives the host the wait for the native stop: the link task is
    // asked to come down and the Promise settles when the pump sees it gone.
    wifi_time_link_stop();
}

static JSValue acquire_finish(JSContext *ctx, void *user, int32_t status,
                              const char *stop_code, bool *rejected) {
    (void)user;
    lease.acquire=0;
    *rejected=true;
    if(stop_code) {
        // A link that came up as the cancel landed is already on its way down;
        // outcome says the app cannot tell which side won.
        if(status==POCKET_STATUS_OK) wifi_time_link_stop();
        return pocket_api_error(ctx,stop_code,"net.wifi.acquire",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the link did not come up in time"
                                    :"cancelled while connecting",
                                true,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(status!=POCKET_STATUS_OK) {
        wifi_time_status_t s=wifi_time_status();
        char message[80];
        // The stage, not the passphrase: "auth" tells the person to fix the
        // stored key without this code ever having seen it.
        snprintf(message,sizeof message,"the link failed at %s",
                 wifi_time_stage_name(s.stage));
        return pocket_api_error(ctx,
                                s.stage==WIFI_TIME_STAGE_AUTH?POCKET_ERR_AUTH_FAILED
                                                             :POCKET_ERR_NOT_AVAILABLE,
                                "net.wifi.acquire",message,
                                s.stage!=WIFI_TIME_STAGE_AUTH,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    lease.handle=++lease.next_handle;
    if(!lease.handle) lease.handle=++lease.next_handle;   // 0 means "no lease"
    lease.refs=1;
    lease.reported=WIFI_TIME_LINK_UP;
    ESP_LOGI(TAG,"LEASE_OPEN");
    *rejected=false;
    return lease_object(ctx);
}

static const pocket_promise_ops_t acquire_ops = {
    .settle=acquire_finish, .stop=acquire_stop,
};

static JSValue js_acquire(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="net.wifi.acquire";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "acquire({profileId}) needs an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue id=JS_GetPropertyStr(ctx,argv[0],"profileId");
    if(JS_IsException(id)) return JS_EXCEPTION;
    const char *text=JS_IsString(id)?JS_ToCString(ctx,id):NULL;
    bool known=text && !strcmp(text,NET_PROFILE_ID);
    if(text) JS_FreeCString(ctx,text);
    JS_FreeValue(ctx,id);
    if(!known)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                 "the only profile on this device is \"default\"",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    net_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

    // Section 11: a second acquire of the same profile shares the connection
    // rather than opening a second one. There is one profile here, so this is
    // every repeat acquire, and it costs the app nothing.
    if(lease.handle && link_is_up()) {
        JS_FreeValue(ctx,options.cancel);
        lease.refs++;
        return pocket_api_settled(ctx,lease_object(ctx),false);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the link",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(lease.acquire) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a lease is already being acquired",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!wifi_time_has_credentials()) {
        JS_FreeValue(ctx,options.cancel);
        // Not the app's to fix: the SSID and key are entered on the settings
        // screen, and section 11 keeps them out of the app's reach on purpose.
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "no Wi-Fi network is configured on this device",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    }

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    esp_err_t err=wifi_time_link_start();
    if(err!=ESP_OK) {
        pocket_api_promise_abandon(request);
        JS_FreeValue(ctx,options.cancel);
        // INVALID_STATE is the clock or the SSID scan holding the radio. It is
        // a real BUSY and it does clear, which is what retryable is for.
        return pocket_api_reject(ctx,
                                 err==ESP_ERR_INVALID_STATE?POCKET_ERR_BUSY
                                                           :POCKET_ERR_OUT_OF_MEMORY,
                                 OP,
                                 err==ESP_ERR_INVALID_STATE
                                     ?"the radio is busy with the clock or a scan"
                                     :"no room for the link task",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    lease.reported=WIFI_TIME_LINK_CONNECTING;
    JSValue promise=pocket_api_promise_arm(ctx,request,&acquire_ops,NULL,
                                           options.cancel,
                                           deadline_from(&options,NET_ACQUIRE_TIMEOUT_MS));
    if(JS_IsException(promise)) { wifi_time_link_stop(); return promise; }
    lease.acquire=request;
    return promise;
}

// --------------------------------------------------------------- wifi.scan

static JSValue scan_finish(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    scan.request=0;
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"net.wifi.scan",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the scan outlived timeoutMs"
                                    :"cancelled while scanning",
                                true,POCKET_OUTCOME_NOT_APPLIED);
    if(status!=POCKET_STATUS_OK)
        return pocket_api_error(ctx,POCKET_ERR_NOT_AVAILABLE,"net.wifi.scan",
                                "the radio could not scan",true,
                                POCKET_OUTCOME_NOT_APPLIED);

    wifi_time_network_t found[WIFI_TIME_SCAN_MAX];
    bool truncated=false;
    unsigned n=wifi_time_scan_networks(found,WIFI_TIME_SCAN_MAX,&truncated);
    JSValue list=JS_NewArray(ctx);
    if(JS_IsException(list)) return list;
    for(unsigned i=0;i<n;i++) {
        JSValue item=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,item,"ssid",JS_NewString(ctx,found[i].ssid));
        JS_SetPropertyStr(ctx,item,"rssiDbm",JS_NewInt32(ctx,found[i].rssi));
        JS_SetPropertyStr(ctx,item,"secure",JS_NewBool(ctx,found[i].secure));
        JS_SetPropertyUint32(ctx,list,i,item);
    }
    JSValue result=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,result,"networks",list);
    JS_SetPropertyStr(ctx,result,"truncated",JS_NewBool(ctx,truncated));
    *rejected=false;
    return result;
}

// A scan has no abort in the driver, so there is nothing to ask. Section 4 then
// requires that the Promise not settle until the native side has finished, and
// that is exactly what happens: the pump keeps waiting and the stop code is
// what the settle uses when the scan does end.
static const pocket_promise_ops_t scan_ops = { .settle=scan_finish };

static JSValue js_scan(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="net.wifi.scan";
    net_options_t options;
    JSValue bad=take_options(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the scan",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(scan.request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,"a scan is already running",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    esp_err_t err=wifi_time_scan_start();
    if(err!=ESP_OK) {
        pocket_api_promise_abandon(request);
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,
                                 err==ESP_ERR_INVALID_STATE?POCKET_ERR_BUSY
                                                           :POCKET_ERR_OUT_OF_MEMORY,
                                 OP,
                                 err==ESP_ERR_INVALID_STATE
                                     ?"the radio is busy with a link or the clock"
                                     :"no room for the scan task",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    JSValue promise=pocket_api_promise_arm(ctx,request,&scan_ops,NULL,options.cancel,
                                           deadline_from(&options,NET_SCAN_TIMEOUT_MS));
    if(JS_IsException(promise)) return promise;
    scan.request=request;
    return promise;
}

// ------------------------------------------------------------ the response

static bool response_live(JSValueConst self) {
    uint32_t g=(uint32_t)(uintptr_t)JS_GetOpaque(self,response_class);
    return g!=0 && g==http.generation && http.open_response;
}

// Frees what the response owned. The worker frees the request half; these two
// outlive it because the app reads through them.
static void response_free(void) {
    free(http.rsp_headers); http.rsp_headers=NULL;
    free(http.chunk);       http.chunk=NULL;
    http.open_response=false;
}

static JSValue js_response_close(JSContext *ctx, JSValueConst self,
                                 int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    if(!response_live(self)) return JS_UNDEFINED;   // idempotent
    JS_SetOpaque((JSValue)self,NULL);
    http_ask_stop();
    response_free();
    return JS_UNDEFINED;
}

static JSValue read_finish(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    atomic_store(&http.read_request,0);
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"net.http.read",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the read outlived its deadline"
                                    :"cancelled while reading",
                                true,
                                // The bytes may or may not have left the socket
                                // buffer by now, and a body read that stops
                                // half way is a body the app cannot reassemble.
                                POCKET_OUTCOME_UNKNOWN);
    if(status==NET_STATUS_DROPPED)
        return pocket_api_error(ctx,POCKET_ERR_DISCONNECTED,"net.http.read",
                                "the link went away during the body",true,
                                POCKET_OUTCOME_UNKNOWN);
    if(status<0)
        return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"net.http.read",
                                "the body could not be read",true,
                                POCKET_OUTCOME_UNKNOWN);
    *rejected=false;
    // Zero bytes is the end of the body, which section 11 types as null.
    if(status==0) return JS_NULL;
    http.received+=(uint32_t)status;
    // What the app keeps is the Uint8Array, so a read costs the guest exactly
    // the bytes it asked for; the chunk itself stays outside the guest heap.
    return JS_NewUint8ArrayCopy(ctx,http.chunk,(size_t)status);
}

static void read_stop(void *user, const char *code) {
    (void)user; (void)code;
    // There is no way to interrupt a socket read that is already in the driver,
    // so the abort is what the worker sees at the next step. The Promise stays
    // unsettled until the worker posts, which is what section 4 asks for.
    atomic_store(&http.abort,true);
    if(wake) xSemaphoreGive(wake);
}

static const pocket_promise_ops_t read_ops = {
    .settle=read_finish, .stop=read_stop,
};

static JSValue js_response_read(JSContext *ctx, JSValueConst self,
                                int argc, JSValueConst *argv) {
    static const char OP[]="net.http.read";
    if(!response_live(self))
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,"the response is closed",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    double want=0;
    if(argc<1 || !JS_IsNumber(argv[0]) || JS_ToFloat64(ctx,&want,argv[0]) ||
       !isfinite(want) || want!=(double)(int64_t)want ||
       want<1 || want>NET_READ_MAX)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "maxBytes must be a whole number of 1 to 1024",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    net_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

    if(http.eof) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_settled(ctx,JS_NULL,false);
    }
    if(atomic_load(&http.read_request)) {
        JS_FreeValue(ctx,options.cancel);
        // Section 4 allows one operation of a kind per handle.
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,"a read is already running",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!atomic_load(&http.alive)) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,
                                 "the connection is no longer open",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(http.received>=NET_RESPONSE_MAX) {
        JS_FreeValue(ctx,options.cancel);
        // Section 11 closes a response that goes past the total. The app is
        // told rather than handed a truncated body it cannot detect.
        http_ask_stop();
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the response passed 65536 bytes",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,"cancelled before the read",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    }
    uint32_t room=NET_RESPONSE_MAX-http.received;
    if((uint32_t)want>room) want=room;    // the cut-off arrives as the refusal
                                          // above on the next read, not as a
                                          // short read that looks like an end

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    http.want=(int)want;
    http.timeout_ms=options.timeout_ms?options.timeout_ms:NET_READ_TIMEOUT_MS;
    atomic_store(&http.read_request,request);
    atomic_store(&http.command,NET_CMD_READ);
    xSemaphoreGive(wake);
    JSValue promise=pocket_api_promise_arm(ctx,request,&read_ops,NULL,options.cancel,
                                           deadline_from(&options,NET_READ_TIMEOUT_MS));
    if(JS_IsException(promise)) {
        // Nothing can settle a Promise that was not built. The worker is asked
        // to stop; its completion carries a request number nobody will read.
        atomic_store(&http.read_request,0);
        http_ask_stop();
    }
    return promise;
}

static const JSCFunctionListEntry response_methods[] = {
    JS_CFUNC_DEF("read",  2, js_response_read),
    JS_CFUNC_DEF("close", 0, js_response_close),
};
static const JSClassDef response_class_def = { .class_name="HttpResponse" };

// --------------------------------------------------------- http.request

static void request_stop(void *user, const char *code) {
    (void)user; (void)code;
    http_ask_stop();
}

static JSValue request_finish(JSContext *ctx, void *user, int32_t status,
                              const char *stop_code, bool *rejected) {
    (void)user;
    http.open=0;
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"net.http.request",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the request outlived timeoutMs"
                                    :"cancelled before the headers arrived",
                                true,
                                // Section 11: a request whose outcome the app
                                // cannot see may still have been applied at the
                                // other end, and a retry would repeat it.
                                POCKET_OUTCOME_UNKNOWN);
    if(status==NET_STATUS_TLS)
        return pocket_api_error(ctx,POCKET_ERR_TLS_ERROR,"net.http.request",
                                "the TLS handshake failed",true,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(status==NET_STATUS_DROPPED)
        return pocket_api_error(ctx,POCKET_ERR_DISCONNECTED,"net.http.request",
                                "the link went away during the request",true,
                                POCKET_OUTCOME_UNKNOWN);
    if(status!=POCKET_STATUS_OK)
        return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"net.http.request",
                                "the request did not reach a response",true,
                                POCKET_OUTCOME_UNKNOWN);

    JSValue o=JS_NewObjectClass(ctx,response_class);
    if(JS_IsException(o)) { http_ask_stop(); response_free(); return o; }
    JSValue headers=JS_NewObject(ctx);
    for(size_t at=0; at<http.rsp_headers_len; ) {
        const char *key=http.rsp_headers+at;
        const char *value=key+strlen(key)+1;
        at+=strlen(key)+strlen(value)+2;
        JS_SetPropertyStr(ctx,headers,key,JS_NewString(ctx,value));
    }
    JS_SetPropertyStr(ctx,o,"status",JS_NewInt32(ctx,http.status_code));
    JS_SetPropertyStr(ctx,o,"headers",headers);
    // Not in section 11's type, and deliberately added: a response whose
    // headers were cut has to say so, or an app reads a missing header as an
    // absent one. Section 11 only declines full multi-value fidelity.
    JS_SetPropertyStr(ctx,o,"headersTruncated",JS_NewBool(ctx,http.headers_truncated));
    JS_SetOpaque(o,(void *)(uintptr_t)http.generation);
    http.open_response=true;
    *rejected=false;
    // 4xx and 5xx are a status, not a rejection: section 11 rejects only for
    // transport and TLS.
    ESP_LOGI(TAG,"HTTP_STATUS %d",http.status_code);
    return o;
}

static const pocket_promise_ops_t request_ops = {
    .settle=request_finish, .stop=request_stop,
};

// Copies the app's headers into one arena as key\0value\0 pairs. Returns a
// PocketError code, or NULL on success.
static const char *take_headers(JSContext *ctx, JSValueConst value,
                                const char **why) {
    http.req_headers_len=0;
    if(JS_IsUndefined(value)||JS_IsNull(value)) return NULL;
    if(!JS_IsObject(value)) { *why="headers must be an object"; return POCKET_ERR_INVALID_ARGUMENT; }
    JSPropertyEnum *props=NULL;
    uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,value,
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)) {
        *why="headers are unreadable";
        return POCKET_ERR_INVALID_ARGUMENT;
    }
    const char *code=NULL;
    if(count>NET_REQ_HEADERS_MAX) { *why="at most 8 request headers"; code=POCKET_ERR_LIMIT_EXCEEDED; }
    for(uint32_t i=0;i<count && !code;i++) {
        const char *key=JS_AtomToCString(ctx,props[i].atom);
        JSValue v=JS_GetProperty(ctx,value,props[i].atom);
        const char *val=JS_IsString(v)?JS_ToCString(ctx,v):NULL;
        if(!key||!val) { *why="each header must be a string"; code=POCKET_ERR_INVALID_ARGUMENT; }
        else {
            size_t kl=strlen(key), vl=strlen(val);
            // A newline in a header is how a second request line gets smuggled
            // into the first, so the bytes are checked rather than trusted.
            for(size_t j=0;j<kl && !code;j++)
                if((unsigned char)key[j]<0x21||(unsigned char)key[j]>0x7e||key[j]==':')
                    { *why="a header name must be printable ASCII"; code=POCKET_ERR_INVALID_ARGUMENT; }
            for(size_t j=0;j<vl && !code;j++)
                if((unsigned char)val[j]<0x20||(unsigned char)val[j]>0x7e)
                    { *why="a header value must be printable ASCII"; code=POCKET_ERR_INVALID_ARGUMENT; }
            if(!code && http.req_headers_len+kl+vl+2>NET_REQ_HEADER_BYTES)
                { *why="request headers must total 2048 bytes or less"; code=POCKET_ERR_LIMIT_EXCEEDED; }
            if(!code) {
                char *p=http.req_headers+http.req_headers_len;
                memcpy(p,key,kl+1);
                memcpy(p+kl+1,val,vl+1);
                http.req_headers_len+=kl+vl+2;
            }
        }
        if(key) JS_FreeCString(ctx,key);
        if(val) JS_FreeCString(ctx,val);
        JS_FreeValue(ctx,v);
    }
    JS_FreePropertyEnum(ctx,props,count);
    return code;
}

static JSValue js_request(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="net.http.request";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "request({wifi,url,method}) needs an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    net_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) return bad;

#define REFUSE(code,message) do { \
        JS_FreeValue(ctx,options.cancel); \
        return pocket_api_reject(ctx,(code),OP,(message),false, \
                                 POCKET_OUTCOME_NOT_APPLIED); \
    } while(0)

    // Section 11 requires a live lease. It is the app's proof that it asked for
    // the radio and knows it is paying for it.
    JSValue wifi=JS_GetPropertyStr(ctx,argv[0],"wifi");
    bool leased=JS_IsObject(wifi) && lease_live(wifi);
    JS_FreeValue(ctx,wifi);
    if(!leased) REFUSE(POCKET_ERR_INVALID_ARGUMENT,
                       "wifi must be a lease from pocket.net.wifi.acquire()");
    if(!link_is_up()) REFUSE(POCKET_ERR_DISCONNECTED,"the link is not up");

    JSValue url=JS_GetPropertyStr(ctx,argv[0],"url");
    const char *url_text=JS_IsString(url)?JS_ToCString(ctx,url):NULL;
    JS_FreeValue(ctx,url);
    if(!url_text) REFUSE(POCKET_ERR_INVALID_ARGUMENT,"url must be a string");
    bool tls=false;
    const char *why=NULL;
    const char *code=url_check(url_text,&tls,&why);
    if(code) {
        // The URL is never logged and never echoed into the message: it can
        // carry a token in its query, and section 7 keeps those out of a log.
        JS_FreeCString(ctx,url_text);
        REFUSE(code,why);
    }

    JSValue method=JS_GetPropertyStr(ctx,argv[0],"method");
    const char *method_text=JS_IsString(method)?JS_ToCString(ctx,method):NULL;
    JS_FreeValue(ctx,method);
    static const struct { const char *name; esp_http_client_method_t m; } METHODS[]={
        {"GET",HTTP_METHOD_GET}, {"POST",HTTP_METHOD_POST},
        {"PUT",HTTP_METHOD_PUT}, {"DELETE",HTTP_METHOD_DELETE},
    };
    esp_http_client_method_t verb=HTTP_METHOD_MAX;
    for(unsigned i=0;method_text&&i<sizeof METHODS/sizeof METHODS[0];i++)
        if(!strcmp(method_text,METHODS[i].name)) verb=METHODS[i].m;
    if(method_text) JS_FreeCString(ctx,method_text);
    if(verb==HTTP_METHOD_MAX) {
        JS_FreeCString(ctx,url_text);
        REFUSE(POCKET_ERR_INVALID_ARGUMENT,"method must be GET, POST, PUT or DELETE");
    }

    JSValue body=JS_GetPropertyStr(ctx,argv[0],"body");
    size_t body_len=0;
    uint8_t *body_bytes=NULL;
    if(!JS_IsUndefined(body)&&!JS_IsNull(body)) {
        body_bytes=JS_GetUint8Array(ctx,&body_len,body);
        if(!body_bytes) {
            JS_FreeValue(ctx,body);
            JS_FreeCString(ctx,url_text);
            REFUSE(POCKET_ERR_INVALID_ARGUMENT,"body must be a Uint8Array");
        }
        if(body_len>NET_REQ_BODY_MAX) {
            JS_FreeValue(ctx,body);
            JS_FreeCString(ctx,url_text);
            REFUSE(POCKET_ERR_LIMIT_EXCEEDED,"body must be 4096 bytes or less");
        }
    }

    if(atomic_load(&http.alive) || http.open_response || http.open) {
        JS_FreeValue(ctx,body);
        JS_FreeCString(ctx,url_text);
        // One request at a time, and a response the app has not closed still
        // owns the socket. Section 4 answers the second with BUSY.
        REFUSE(POCKET_ERR_BUSY,"a request is already in flight");
    }

    // The one refusal that is about this board rather than about the request.
    // A handshake that runs the internal heap out does not fail, it aborts, and
    // the app is better served by a number it can print.
    size_t free_now=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    size_t block=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(free_now<(tls?NET_TLS_MIN_FREE:NET_PLAIN_MIN_FREE) ||
       (tls&&block<NET_TLS_MIN_BLOCK)) {
        char message[96];
        snprintf(message,sizeof message,
                 "not enough memory for %s: %u free, %u largest block",
                 tls?"a TLS handshake":"a request",
                 (unsigned)free_now,(unsigned)block);
        JS_FreeValue(ctx,body);
        JS_FreeCString(ctx,url_text);
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,message,true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    http.req_headers=malloc(NET_REQ_HEADER_BYTES);
    http.rsp_headers=malloc(NET_RSP_HEADER_BYTES);
    http.chunk=malloc(NET_READ_MAX);
    http.body=body_len?malloc(body_len):NULL;
    if(!http.req_headers||!http.rsp_headers||!http.chunk||(body_len&&!http.body)) {
        free(http.req_headers); http.req_headers=NULL;
        free(http.body); http.body=NULL;
        response_free();
        JS_FreeValue(ctx,body);
        JS_FreeCString(ctx,url_text);
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no room for the request buffers",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // Section 4: the bytes are copied at the call, so what the app does to its
    // array afterwards does not reach the wire.
    if(body_len) memcpy(http.body,body_bytes,body_len);
    http.body_len=body_len;
    JS_FreeValue(ctx,body);

    JSValue header_field=JS_GetPropertyStr(ctx,argv[0],"headers");
    code=take_headers(ctx,header_field,&why);
    JS_FreeValue(ctx,header_field);
    if(code) {
        free(http.req_headers); http.req_headers=NULL;
        free(http.body); http.body=NULL;
        response_free();
        JS_FreeCString(ctx,url_text);
        REFUSE(code,why);
    }

    snprintf(http.url,sizeof http.url,"%s",url_text);
    JS_FreeCString(ctx,url_text);
    http.method=verb;
    http.tls=tls;
    http.timeout_ms=options.timeout_ms?options.timeout_ms:NET_REQUEST_TIMEOUT_MS;
    http.status_code=0;
    http.rsp_headers_len=0;
    http.rsp_header_count=0;
    http.headers_truncated=false;
    http.received=0;
    http.eof=false;
    http.generation++;
    if(!http.generation) http.generation=1;
    atomic_store(&http.abort,false);
    atomic_store(&http.command,NET_CMD_NONE);
    atomic_store(&http.read_request,0);
    while(xSemaphoreTake(wake,0)==pdTRUE) { }   // drop a stale give

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        free(http.req_headers); http.req_headers=NULL;
        free(http.body); http.body=NULL;
        response_free();
        REFUSE(POCKET_ERR_BUSY,"too many operations are pending");
    }
    http.open=request;
    atomic_store(&http.alive,true);
    // The worker owns mbedTLS's allocations from here until it says otherwise,
    // and the pool must not be handed back underneath it.
    // 6 KiB: a TLS handshake's own frames are the deep part, and this task also
    // carries esp_http_client and the socket calls under it.
    if(xTaskCreate(http_task,"pocket_http",6144,NULL,5,NULL)!=pdPASS) {
        atomic_store(&http.alive,false);
        http.open=0;
        pocket_api_promise_abandon(request);
        free(http.req_headers); http.req_headers=NULL;
        free(http.body); http.body=NULL;
        response_free();
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no room for the request task",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    JSValue promise=pocket_api_promise_arm(ctx,request,&request_ops,NULL,
                                           options.cancel,
                                           deadline_from(&options,NET_REQUEST_TIMEOUT_MS));
    if(JS_IsException(promise)) {
        http.open=0;
        http_ask_stop();
    }
    return promise;
#undef REFUSE
}

// -------------------------------------------------------------------- pump

void pocket_net_pump(void) {
    wifi_time_link_t now=wifi_time_link_state();

    if(lease.acquire && now!=WIFI_TIME_LINK_CONNECTING)
        pocket_api_complete(lease.acquire,
                            now==WIFI_TIME_LINK_UP?POCKET_STATUS_OK:-1);

    if(scan.request) {
        wifi_time_state_t s=wifi_time_scan_state();
        if(s!=WIFI_TIME_RUNNING)
            pocket_api_complete(scan.request,s==WIFI_TIME_OK?POCKET_STATUS_OK:-1);
    }

    // A lease that lost its link tells its listeners once, and the lease stays
    // the app's to close: section 4 makes close() the app's, and taking the
    // object out from under it would only turn a reported drop into a
    // TypeError on the next status().
    if(lease.handle && now!=lease.reported) {
        lease.reported=now;
        if(lease_table.open) pocket_api_sub_deliver(&lease_table,lease_payload,NULL);
    }
}

// ------------------------------------------------------------------- reset

// Whether pocket.net was ever read. Without it no lease was taken and no
// request was started, so there is no radio to bring down.
static bool built;

void pocket_net_reset(void) {
    if(!built) return;
    built=false;
    pocket_api_sub_close_all(&lease_table);
    lease_table.ctx=NULL;
    lease.acquire=0;
    scan.request=0;
    if(http.open_response || atomic_load(&http.alive)) {
        http_ask_stop();
        response_free();
    }
    if(lease.handle) {
        lease_release();
        ESP_LOGI(TAG,"LEASE_DROPPED");
    } else if(wifi_time_link_state()==WIFI_TIME_LINK_CONNECTING) {
        // A session that ended while acquire was still waiting.
        wifi_time_link_stop();
    }
    // mbedTLS's pool is a session-scoped loan, not a static buffer: an app that
    // never opened a socket never took it, and one that did gives it back here
    // rather than holding 20 KB across the next app's whole run.
}

// ---------------------------------------------------------------- capability

static bool have_credentials;   // read once at install; see the probe

static const pocket_limit_t wifi_limits[] = {
    {.name="maxLeases",        .kind=POCKET_LIMIT_INT,  .number=1},
    {.name="maxListeners",     .kind=POCKET_LIMIT_INT,  .number=NET_LEASE_WATCHES},
    {.name="profileIds",       .kind=POCKET_LIMIT_TEXT, .text=NET_PROFILE_ID},
    {.name="band",             .kind=POCKET_LIMIT_TEXT, .text="2.4GHz"},
    // Published because an app that sees available=false with LOW_MEMORY has no
    // other way to learn how far short it is, and because the number is the one
    // real constraint on using the radio from an app at all.
    {.name="radioMinFreeBytes", .kind=POCKET_LIMIT_INT,  .number=NET_RADIO_MIN_FREE},
    {.name="mode",             .kind=POCKET_LIMIT_TEXT, .text="sta"},
    {.name="scanMaxNetworks",  .kind=POCKET_LIMIT_INT,  .number=WIFI_TIME_SCAN_MAX},
    {.name="scanTimeoutMs",    .kind=POCKET_LIMIT_INT,  .number=NET_SCAN_TIMEOUT_MS},
    {.name="acquireTimeoutMs", .kind=POCKET_LIMIT_INT,  .number=NET_ACQUIRE_TIMEOUT_MS},
    {.name="maxTimeoutMs",     .kind=POCKET_LIMIT_INT,  .number=NET_TIMEOUT_MAX_MS},
    // The two costs an app is really choosing between when it acquires. Both
    // are measured figures from this firmware, not a budget.
    {.name="linkHeapCostBytes",.kind=POCKET_LIMIT_INT,  .number=37*1024},
    {.name="linkRetainedBytes",.kind=POCKET_LIMIT_INT,  .number=4915},
    {0},
};

static const pocket_limit_t http_limits[] = {
    {.name="maxConcurrentRequests",.kind=POCKET_LIMIT_INT, .number=1},
    {.name="maxUrlBytes",          .kind=POCKET_LIMIT_INT, .number=NET_URL_MAX},
    {.name="maxRequestHeaders",    .kind=POCKET_LIMIT_INT, .number=NET_REQ_HEADERS_MAX},
    {.name="maxRequestHeaderBytes",.kind=POCKET_LIMIT_INT, .number=NET_REQ_HEADER_BYTES},
    {.name="maxRequestBodyBytes",  .kind=POCKET_LIMIT_INT, .number=NET_REQ_BODY_MAX},
    {.name="maxResponseHeaders",   .kind=POCKET_LIMIT_INT, .number=NET_RSP_HEADERS_MAX},
    {.name="maxResponseHeaderBytes",.kind=POCKET_LIMIT_INT,.number=NET_RSP_HEADER_BYTES},
    {.name="maxReadBytes",         .kind=POCKET_LIMIT_INT, .number=NET_READ_MAX},
    {.name="maxResponseBytes",     .kind=POCKET_LIMIT_INT, .number=NET_RESPONSE_MAX},
    {.name="requestTimeoutMs",     .kind=POCKET_LIMIT_INT, .number=NET_REQUEST_TIMEOUT_MS},
    {.name="readTimeoutMs",        .kind=POCKET_LIMIT_INT, .number=NET_READ_TIMEOUT_MS},
    {.name="maxTimeoutMs",         .kind=POCKET_LIMIT_INT, .number=NET_TIMEOUT_MAX_MS},
    {.name="methods",              .kind=POCKET_LIMIT_TEXT,.text="GET,POST,PUT,DELETE"},
    {.name="tls",                  .kind=POCKET_LIMIT_FLAG,.number=1},
    // Plain HTTP goes to a device on the local network and nowhere else; there
    // is no per-app registration on this firmware to widen that with.
    {.name="plaintext",            .kind=POCKET_LIMIT_TEXT,.text="private-address-only"},
    {.name="redirects",            .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="decompression",        .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="cookies",              .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="tlsMinFreeBytes",      .kind=POCKET_LIMIT_INT, .number=NET_TLS_MIN_FREE},
    {.name="tlsMinBlockBytes",     .kind=POCKET_LIMIT_INT, .number=NET_TLS_MIN_BLOCK},
    {0},
};

static void wifi_probe(const pocket_capability_t *cap, bool *available,
                       const char **reason) {
    (void)cap;
    // Credentials are entered on a settings screen that cannot be reached while
    // an app runs, so the answer cannot change inside a session and the NVS
    // read stays out of a per-call probe.
    if(!have_credentials) { *available=false; *reason=POCKET_REASON_DISABLED; return; }
    // Section 2 calls available an observation rather than a reservation, but
    // an observation still has to be one. This said true to apps/netcheck while
    // the radio could not start at all: esp_wifi_init returned ESP_ERR_NO_MEM
    // at 9,160 bytes free, and it does that without degrading first.
    //
    // A link already up costs nothing to keep, so it is available whatever the
    // heap looks like. Bringing one up is what has a price.
    if(!wifi_time_radio_is_up()) {
        size_t free_now=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(free_now<NET_RADIO_MIN_FREE) { *available=false; *reason="LOW_MEMORY"; return; }
    }
    *available=true;
    *reason=NULL;
}

static void http_probe(const pocket_capability_t *cap, bool *available,
                       const char **reason) {
    (void)cap;
    if(!have_credentials) { *available=false; *reason=POCKET_REASON_DISABLED; return; }
    size_t free_now=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    // Every request goes over a lease, so http is only as available as the link
    // under it. Without one already held, that means the radio still has to come
    // up, and the 12 KB below is nowhere near what that costs -- reporting true
    // here said the same untruth net.wifi used to, one level further up.
    size_t need = lease.handle ? NET_PLAIN_MIN_FREE
                : wifi_time_radio_is_up() ? NET_PLAIN_MIN_FREE : NET_RADIO_MIN_FREE;
    if(free_now<need) { *available=false; *reason="LOW_MEMORY"; return; }
    if(atomic_load(&http.alive)) { *available=false; *reason=POCKET_REASON_BUSY; return; }
    *available=true;
    *reason=NULL;
}

static const pocket_capability_t wifi_capability = {
    .name="net.wifi", .supported=true, .available=false,
    .reason=POCKET_REASON_DISABLED, .limits=wifi_limits, .probe=wifi_probe,
};
static const pocket_capability_t http_capability = {
    .name="net.http", .supported=true, .available=false,
    .reason=POCKET_REASON_DISABLED, .limits=http_limits, .probe=http_probe,
};

// ----------------------------------------------------------------- install

// Built the first time an app reads pocket.net, and not before: the classes,
// the two prototypes and the namespace objects are guest heap, and an app that
// never asks for the radio should not pay for them. The capability entries are
// C data and stay eager, so a feature test still answers without building any
// of this.
static esp_err_t build_net(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JSRuntime *rt=JS_GetRuntime(ctx);
    // JS_NewClassID assigns once for the process; JS_NewClass is per runtime,
    // and a session that never reaches here never needs either.
    JS_NewClassID(rt,&lease_class);
    JS_NewClassID(rt,&response_class);
    if(JS_NewClass(rt,lease_class,&lease_class_def)<0 ||
       JS_NewClass(rt,response_class,&response_class_def)<0) return ESP_FAIL;

    // One shared prototype each. Per-handle closures would put a function
    // object on the guest heap for every method, and guest heap is what runs
    // out on this board.
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return ESP_ERR_NO_MEM;
    JS_SetPropertyFunctionList(ctx,proto,lease_methods,
                               (int)(sizeof lease_methods/sizeof lease_methods[0]));
    JS_SetClassProto(ctx,lease_class,proto);
    proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return ESP_ERR_NO_MEM;
    JS_SetPropertyFunctionList(ctx,proto,response_methods,
                               (int)(sizeof response_methods/sizeof response_methods[0]));
    JS_SetClassProto(ctx,response_class,proto);

    // The table's realm is only needed once a listener can exist, which is once
    // the namespace does.
    lease_table.ctx=ctx;

    JSValue wifi=JS_NewObject(ctx);
    JSValue httpns=JS_NewObject(ctx);
    if(JS_IsException(wifi)||JS_IsException(httpns)) {
        JS_FreeValue(ctx,wifi); JS_FreeValue(ctx,httpns);
        return ESP_ERR_NO_MEM;
    }
    JS_SetPropertyStr(ctx,wifi,"acquire",JS_NewCFunction(ctx,js_acquire,"acquire",2));
    JS_SetPropertyStr(ctx,wifi,"scan",JS_NewCFunction(ctx,js_scan,"scan",1));
    JS_SetPropertyStr(ctx,httpns,"request",JS_NewCFunction(ctx,js_request,"request",2));
    JS_SetPropertyStr(ctx,(JSValue)ns,"wifi",wifi);
    JS_SetPropertyStr(ctx,(JSValue)ns,"http",httpns);
    built=true;
    return ESP_OK;
}

esp_err_t pocket_net_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    have_credentials=wifi_time_has_credentials();
    pocket_api_register(&wifi_capability);
    pocket_api_register(&http_capability);

    // Created once for the life of the run, not per session: giving it has to
    // be safe whether or not a worker exists. See where it is declared.
    if(!wake) {
        wake=xSemaphoreCreateBinary();
        if(!wake) return ESP_ERR_NO_MEM;
    }

    // Nothing survives a session: the realm going away takes the callbacks with
    // it, so the table starts empty on every install. The radio does not survive
    // one either -- app_stop() calls pocket_net_reset().
    for(int i=0;i<NET_LEASE_WATCHES;i++) {
        lease_slots[i].callback=JS_UNDEFINED;
        lease_slots[i].handle=0;
    }
    lease_table.open=0;
    lease_table.ctx=NULL;
    lease.handle=0; lease.refs=0; lease.acquire=0;
    lease.reported=WIFI_TIME_LINK_DOWN;
    scan.request=0;

    return pocket_api_lazy(ctx,"net",build_net,NULL);
}
