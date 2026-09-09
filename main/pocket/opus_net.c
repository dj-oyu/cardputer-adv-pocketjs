#include "opus_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// See opus_net.h for why this task exists and why it serves .pok today.
//
// THE STACK. pocket_net.c's per-request worker uses 6,144 for the same work
// (esp_http_client_open, fetch_headers, read), and that number has survived
// every request this firmware has made, so it is the one with evidence behind
// it rather than a fresh guess. It is also the whole point of this file: this
// task REPLACES that worker rather than joining it, and it drops the 2,048 +
// 1,024 + 1,024 of header and chunk buffers alongside, because the bytes go
// straight into the ring slot they will be published from.
//
// Measured at the refusal this file exists to fix: 11,404 free, 7,680 largest.
// 6,144 fits that run; the 4,096 of buffers is what was missing from the total.
#define NET_STACK      6144
#define NET_PRIO       5
// One read's patience. The ring holds 2.0 s of compressed audio, so a socket
// that goes quiet for a quarter of a second has cost nothing yet: it is counted
// as a stall and retried, not reported. NET_MAX_STALLS consecutive is 2.5 s,
// longer than the ring, and by then the underrun counter is already telling the
// truth about it.
#define NET_READ_MS    250
#define NET_MAX_STALLS 10
#define NET_URL_MAX    256
#define NET_HEAP       (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)

static TaskHandle_t    net_task;
static sound_stream_t *net_packets;
// A heap copy, not a 256-byte static and not a borrowed pointer. The static
// would be paid by every app on this board including the ones that never
// stream, and this feature's claim is that it costs nothing at rest. The
// borrowed pointer was worse: it aimed at pocket_av.c's player.path, which
// player_teardown() frees UNCONDITIONALLY -- including down the branch where a
// task is still running and the log says so. Narrow, since esp_http_client_init
// copies the url immediately, but a use-after-free that needs a task to be slow
// is exactly the kind that shows up once on somebody else's board.
static char           *net_url;
static opus_pak_t      net_pak;
// Written here and read on the JS task, so atomics rather than volatile: the
// same handshake, and the same reason, as opus_feed.c's dec_running.
static atomic_bool     net_halt, net_running, net_ready;
static atomic_uint     net_stalls;
static const char     *net_why;   // set before the task ends, read after

static void fail(const char *why) {
    if(!net_why) net_why=why;
    ESP_LOGE("opusnet","%s",why);
}

// Exactly `want` bytes, or false. Stalls are retried; anything else ends it.
static bool read_exact(esp_http_client_handle_t c, uint8_t *out, uint32_t want) {
    uint32_t got=0;
    unsigned quiet=0;
    while(got<want) {
        if(atomic_load(&net_halt)) return false;
        int n=esp_http_client_read(c,(char *)out+got,(int)(want-got));
        if(n<0) { fail("the connection failed mid-stream"); return false; }
        if(n==0) {
            if(esp_http_client_is_complete_data_received(c)) return false;
            atomic_fetch_add(&net_stalls,1);
            if(++quiet>NET_MAX_STALLS) { fail("the source stopped sending"); return false; }
            continue;
        }
        quiet=0;
        got+=(uint32_t)n;
    }
    return true;
}

// The index is not read, it is walked past: seeking a stream is not a thing this
// host does, so the only value those bytes have is their length. Discarded
// through the stack in small bites rather than into a buffer of its own.
static bool skip_bytes(esp_http_client_handle_t c, uint32_t bytes) {
    uint8_t bin[128];
    while(bytes) {
        uint32_t take=bytes<sizeof bin?bytes:(uint32_t)sizeof bin;
        if(!read_exact(c,bin,take)) return false;
        bytes-=take;
    }
    return true;
}

static void net_task_fn(void *arg) {
    (void)arg;
    esp_http_client_config_t cfg={
        .url=net_url,
        .method=HTTP_METHOD_GET,
        .timeout_ms=NET_READ_MS,
        // No event handler: response headers are what pocket_net.c keeps a
        // 1,024-byte buffer for, and nothing here reads one.
    };
    esp_http_client_handle_t c=esp_http_client_init(&cfg);
    if(!c) { fail("no room for the http client"); goto done; }

    if(esp_http_client_open(c,0)!=ESP_OK) { fail("the server did not answer"); goto shut; }
    int64_t length=esp_http_client_fetch_headers(c);
    if(esp_http_client_get_status_code(c)!=200) {
        fail("the server refused the request"); goto shut;
    }
    // A .pok header states lengths that have to be checked against the size of
    // the thing they are in, and over HTTP that size is Content-Length. Without
    // one there is nothing to check them against, so the walk that turns a lying
    // header into a refusal rather than a read past the end cannot run. Refused
    // rather than trusted -- and a chunked server becomes fine to support the
    // moment the container is Ogg, which needs no length at all.
    if(length<=0) { fail("this host needs a Content-Length for a .pok stream"); goto shut; }

    uint8_t head[OPUS_PAK_HEADER];
    if(!read_exact(c,head,OPUS_PAK_HEADER)) {
        if(!net_why) fail("the stream ended inside its header");
        goto shut;
    }
    const char *bad=opus_pak_parse(head,(uint32_t)length,&net_pak);
    if(bad) { fail(bad); goto shut; }
    if(!skip_bytes(c,net_pak.data_offset-OPUS_PAK_HEADER)) {
        if(!net_why) fail("the stream ended inside its index");
        goto shut;
    }
    atomic_store(&net_ready,true);

    bool eof=false;
    opus_net_carry_t carry={0};
    while(!atomic_load(&net_halt)&&!eof) {
        uint8_t *slot=sound_stream_slot(net_packets);
        if(!slot) { vTaskDelay(1); continue; }   // the decoder is behind
        // The leftover from the slot just published, moved to the head of this
        // one. tools/test_opus_stream.c drives these same three inline calls
        // with sockets that hand over one byte at a time.
        uint32_t have=opus_net_resume(&carry,slot);
        while(have<SOUND_STREAM_SLOT_BYTES) {
            if(atomic_load(&net_halt)) goto shut;
            int n=esp_http_client_read(c,(char *)slot+have,
                                       (int)(SOUND_STREAM_SLOT_BYTES-have));
            if(n<0) { fail("the connection failed mid-stream"); eof=true; break; }
            if(n==0) {
                if(esp_http_client_is_complete_data_received(c)) { eof=true; break; }
                atomic_fetch_add(&net_stalls,1);
                // Not an error and not silence: the ring is still feeding the
                // decoder while this waits. Only a drained ring is audible.
                continue;
            }
            have+=(uint32_t)n;
        }
        opus_net_cut_t cut=opus_net_cut(slot,have,eof);
        if(!cut.publish) {
            // Either a packet claims to be longer than a slot -- which
            // opus_pak_parse already refused through maxPacketBytes, so the
            // stream disagrees with its own header -- or eof arrived with
            // nothing whole in hand. Ending is honest; retrying would read the
            // same bytes for ever.
            if(!eof) fail("a packet does not fit the stream slot");
            sound_stream_publish(net_packets,0,true);
            break;
        }
        sound_stream_publish(net_packets,cut.publish,cut.last&&!cut.carry_len);
        if(cut.last&&cut.carry_len) {
            // Whole packets published, a fragment abandoned. The audio that
            // arrived still plays; the stream ends where it stops being whole.
            ESP_LOGW("opusnet","%u bytes of a partial packet at the end",
                     (unsigned)cut.carry_len);
            sound_stream_publish(net_packets,0,true);
            break;
        }
        opus_net_advance(&carry,slot,cut);
    }

shut:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
done:
    // Whatever happened, the decoder must not wait for bytes that are not
    // coming: eof is what turns a dead producer into a stream that ends rather
    // than one that reports "playing" in silence for ever.
    atomic_store(&net_packets->eof,true);
    ESP_LOGI("opusnet","OPUSNET stalls=%u ready=%d why=%s stack_used=%u of %d",
             (unsigned)atomic_load(&net_stalls),(int)atomic_load(&net_ready),
             net_why?net_why:"-",
             (unsigned)(NET_STACK-uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t)),
             NET_STACK);
    free(net_url); net_url=NULL;
    atomic_store(&net_running,false);
    vTaskDelete(NULL);
}

opus_net_start_t opus_net_start(const char *url, sound_stream_t *packets) {
    if(atomic_load(&net_running)) return OPUS_NET_BUSY;
    if(!url||!packets) return OPUS_NET_BAD_URL;
    size_t n=strlen(url);
    if(n==0||n>=NET_URL_MAX) return OPUS_NET_BAD_URL;
    if(strncmp(url,"http://",7)&&strncmp(url,"https://",8)) return OPUS_NET_BAD_URL;
    net_url=malloc(n+1);
    if(!net_url) return OPUS_NET_NOMEM;
    memcpy(net_url,url,n+1);
    net_packets=packets;
    net_why=NULL;
    atomic_store(&net_halt,false);
    atomic_store(&net_ready,false);
    atomic_store(&net_stalls,0);
    atomic_store(&net_running,true);
    if(xTaskCreate(net_task_fn,"opusnet",NET_STACK/sizeof(StackType_t),NULL,
                   NET_PRIO,&net_task)!=pdPASS) {
        free(net_url); net_url=NULL;
        ESP_LOGE("opusnet","the receive task (%d of stack) would not start: "
                           "%u free, %u largest",NET_STACK,
                 (unsigned)heap_caps_get_free_size(NET_HEAP),
                 (unsigned)heap_caps_get_largest_free_block(NET_HEAP));
        atomic_store(&net_running,false);
        return OPUS_NET_NOMEM;
    }
    return OPUS_NET_OK;
}

bool opus_net_stop(void) {
    if(!atomic_load(&net_running)) return true;
    atomic_store(&net_halt,true);
    // One read's patience plus slack: the task's longest uninterruptible step is
    // a single esp_http_client_read bounded by NET_READ_MS.
    for(int i=0;i<200&&atomic_load(&net_running);i++) vTaskDelay(pdMS_TO_TICKS(5));
    return !atomic_load(&net_running);
}

bool opus_net_ready(void) { return atomic_load(&net_ready); }
const opus_pak_t *opus_net_header(void) { return &net_pak; }
const char *opus_net_error(void) { return net_why; }
uint32_t opus_net_stalls(void) { return atomic_load(&net_stalls); }
