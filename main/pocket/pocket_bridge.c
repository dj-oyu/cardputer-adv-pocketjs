#include "pocket_bridge.h"
#include "pocket_api.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "pocket.bridge"

// One session at a time, because one PC is at the other end of one cable. A
// second connect() is BUSY rather than a second sessionId to keep apart.
#define BRIDGE_CALLS     4      // connect() and request() in flight together
#define BRIDGE_LISTENERS 4      // onEvent() subscriptions
#define BRIDGE_TOPIC     24     // including the NUL
#define BRIDGE_PEER      32     // including the NUL
#define BRIDGE_METHOD    32     // excluding the NUL that separates it

// Section 14's ceiling for a general Promise, the same number pocket_av.c and
// storage.kv enforce. The default is short on purpose: a PC that is not running
// the adapter should be reported as absent in seconds, not after half a minute.
#define BRIDGE_MAX_TIMEOUT_MS     30000
#define BRIDGE_DEFAULT_TIMEOUT_MS 5000

// What the pump posts for a call. POCKET_STATUS_OK means the PC answered.
#define BRIDGE_STATUS_REMOTE    1   // it answered with an ERROR frame
#define BRIDGE_STATUS_STOPPED   2   // close(), or a stop with no driver to wait for
#define BRIDGE_STATUS_NO_MEMORY 3   // it answered and we could not keep the answer

// Our own copy of the reflected CRC32 pet_hub_core.c uses. Five lines and no
// table, against a build-time dependency on a file another session owns and is
// editing; the duplication is the cheaper of the two.
static uint32_t bridge_crc(const uint8_t *p, size_t n) {
    uint32_t c=0xffffffffu;
    while(n--) { c^=*p++; for(unsigned i=0;i<8;i++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return c^0xffffffffu;
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void write32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

// ---------------------------------------------------------------- the way in
//
// Bytes arrive on the input task, one per call from main.c. A frame is decoded
// straight into the slot the JS task will read, so the only DRAM this transport
// holds is the two slots themselves -- there is no third assembly buffer.
//
// Single producer (input task), single consumer (JS task), two slots: the
// producer starts a frame only when head-tail < 2, which is exactly when head&1
// and tail&1 differ, so the two never touch the same slot. That is why there is
// no lock here, and why growing the ring past two would need one.

typedef struct {
    uint16_t length;
    uint8_t  data[POCKET_BRIDGE_MAX_FRAME];
} bridge_slot_t;

static bridge_slot_t inbox[2];
static atomic_uint   inbox_head, inbox_tail;
// Section 5 asks for a lost message to be counted rather than swallowed.
static atomic_uint   inbox_dropped;

enum { RX_IDLE=0, RX_TYPE, RX_HIGH, RX_LOW, RX_SKIP };
static uint8_t        rx_state;
static uint8_t        rx_high;
static uint16_t       rx_length;
static bridge_slot_t *rx_slot;

static int hex_digit(uint8_t c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}

// A completed hex run. Everything structural is checked here, so the JS task
// only ever sees frames that are whole, intact and self-consistent.
static void rx_finish(void) {
    rx_state=RX_IDLE;
    const uint8_t *d=rx_slot->data;
    if(rx_length<POCKET_BRIDGE_HEADER+4) { atomic_fetch_add(&inbox_dropped,1); return; }
    unsigned payload=(unsigned)d[2]|(unsigned)d[3]<<8;
    if(d[0]!=POCKET_BRIDGE_VERSION ||
       POCKET_BRIDGE_HEADER+payload+4u!=rx_length ||
       bridge_crc(d,rx_length-4u)!=read32(d+rx_length-4)) {
        atomic_fetch_add(&inbox_dropped,1);
        return;
    }
    rx_slot->length=rx_length;
    // Published last: the consumer reads the slot only after this store.
    atomic_store(&inbox_head,atomic_load(&inbox_head)+1);
}

static void rx_feed(uint8_t c) {
    switch(rx_state) {
    case RX_IDLE:
        if(c==0x1d) rx_state=RX_TYPE;
        return;
    case RX_TYPE:
        if(c==0x1d) return;                   // a restart inside a restart
        if(c!='B') { rx_state=RX_SKIP; return; }
        if(atomic_load(&inbox_head)-atomic_load(&inbox_tail)>=2) {
            // The JS task has not drained. Refusing the frame here rather than
            // overwriting a slot is what keeps the ring lock-free.
            atomic_fetch_add(&inbox_dropped,1);
            rx_state=RX_SKIP;
            return;
        }
        rx_slot=&inbox[atomic_load(&inbox_head)&1u];
        rx_length=0;
        rx_state=RX_HIGH;
        return;
    case RX_HIGH:
    case RX_LOW: {
        if(c=='\n') { rx_finish(); return; }
        if(c==0x1d) { rx_state=RX_TYPE; return; }
        int v=hex_digit(c);
        if(v<0 || (rx_state==RX_HIGH && rx_length>=POCKET_BRIDGE_MAX_FRAME)) {
            rx_state=RX_SKIP;
            return;
        }
        if(rx_state==RX_HIGH) { rx_high=(uint8_t)(v<<4); rx_state=RX_LOW; }
        else { rx_slot->data[rx_length++]=(uint8_t)(rx_high|v); rx_state=RX_HIGH; }
        return;
    }
    default:                                  // RX_SKIP
        if(c==0x1d) rx_state=RX_TYPE;
        else if(c=='\n') rx_state=RX_IDLE;
        return;
    }
}

bool pocket_bridge_usb(uint8_t byte) {
    if(rx_state==RX_IDLE && byte!=0x1d) return false;
    rx_feed(byte);
    // main.c's input task takes one byte per 5ms poll, which would spend five
    // seconds on a full frame. The rest of it is already sitting in the driver's
    // RX buffer, so it is taken here with no timeout at all: this never blocks,
    // and it stops the moment the frame ends or the buffer runs dry.
    //
    // One byte per read rather than a block: the byte after a frame's LF may be
    // a keystroke, and there is nowhere to hand a byte back to main.c.
    for(unsigned i=0; rx_state!=RX_IDLE && i<POCKET_BRIDGE_MAX_WIRE; i++) {
        uint8_t next;
        if(usb_serial_jtag_read_bytes(&next,1,0)<=0) break;
        rx_feed(next);
    }
    return true;
}

// --------------------------------------------------------------- the way out

esp_err_t pocket_bridge_emit(const uint8_t *body, size_t len) {
    static const char HEX[]="0123456789abcdef";
    if(!body || len<POCKET_BRIDGE_HEADER || len>POCKET_BRIDGE_MAX_FRAME-4u)
        return ESP_ERR_INVALID_SIZE;
    if(!usb_serial_jtag_is_driver_installed()) return ESP_ERR_INVALID_STATE;

    uint8_t wire[POCKET_BRIDGE_MAX_WIRE];
    uint8_t crc[4];
    size_t  at=0;
    wire[at++]=0x1d;
    wire[at++]='B';
    write32(crc,bridge_crc(body,len));
    for(size_t i=0;i<len;i++) {
        wire[at++]=(uint8_t)HEX[body[i]>>4];
        wire[at++]=(uint8_t)HEX[body[i]&15];
    }
    for(unsigned i=0;i<4;i++) {
        wire[at++]=(uint8_t)HEX[crc[i]>>4];
        wire[at++]=(uint8_t)HEX[crc[i]&15];
    }
    wire[at++]='\n';
    // One call, so the driver's tx_mux and its all-or-nothing ring send keep the
    // frame contiguous against anything else writing to the same stream. A
    // refusal writes nothing at all, which is why a timeout here is honest to
    // report as "not applied" rather than as a guess.
    return usb_serial_jtag_write_bytes(wire,at,pdMS_TO_TICKS(50))==(int)at
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ------------------------------------------------------------------ the link

typedef struct {
    pocket_request_t request;     // 0 marks a free entry
    uint8_t          kind;        // POCKET_BRIDGE_HELLO or _REQUEST
    uint8_t         *answer;      // the reply's payload, kept until settle()
    uint16_t         answer_len;
    bool             posted;      // a completion is already on its way
} bridge_call_t;

static bridge_call_t calls[BRIDGE_CALLS];

static struct {
    uint32_t id;                  // 0 when there is no session
    bool     open;                // the PC answered HELLO
    char     peer[BRIDGE_PEER];
} link;

static pocket_sub_slot_t  event_slots[BRIDGE_LISTENERS];
static char               event_topics[BRIDGE_LISTENERS][BRIDGE_TOPIC];
static pocket_sub_table_t event_table = {
    .slots=event_slots, .count=BRIDGE_LISTENERS,
    .tag=TAG, .what="onEvent",
    // Unlike a sensor watch, an event arrives only when the PC sends one, so a
    // listener that throws is not a runaway and keeps its subscription. Same
    // reasoning capabilities.onChange uses, and the same answer.
    .close_on_throw=false,
};

static bridge_call_t *call_free(void) {
    for(int i=0;i<BRIDGE_CALLS;i++) if(!calls[i].request) return &calls[i];
    return NULL;
}
static bridge_call_t *call_of(uint32_t request) {
    if(!request) return NULL;
    for(int i=0;i<BRIDGE_CALLS;i++) if(calls[i].request==request) return &calls[i];
    return NULL;
}

// Keeps the reply and posts the completion. The copy is what section 4 asks
// for: what the app receives must not be a borrowed transport buffer, and the
// slot it arrived in goes straight back to the input task.
static void call_answer(bridge_call_t *c, int32_t status,
                        const uint8_t *payload, uint16_t length) {
    if(c->posted) return;
    if(length) {
        // One byte more than the payload, and terminated. JS_ParseJSON's
        // contract is buf[buf_len]=='\0' (quickjs.h), so an allocation of
        // exactly `length` would have it read one past the end.
        c->answer=malloc((size_t)length+1);
        if(!c->answer) { status=BRIDGE_STATUS_NO_MEMORY; length=0; }
        else { memcpy(c->answer,payload,length); c->answer[length]=0; }
    }
    c->answer_len=length;
    c->posted=true;
    pocket_api_complete(c->request,status);
}

// Splits a "name '\0' rest" payload. NULL when there is no NUL within `max`
// bytes -- which is a peer that is not speaking this protocol.
static const char *split(const uint8_t *payload, uint16_t length, unsigned max,
                         const char **rest, size_t *rest_len) {
    for(uint16_t i=0;i<length && i<max;i++) {
        if(payload[i]) continue;
        *rest=(const char *)payload+i+1;
        *rest_len=length-i-1u;
        return (const char *)payload;
    }
    return NULL;
}

// ------------------------------------------------------------------ inbound

static const char *known_code(const char *code) {
    static const char *const CODES[]={
        POCKET_ERR_INVALID_ARGUMENT, POCKET_ERR_UNSUPPORTED,
        POCKET_ERR_NOT_AVAILABLE,    POCKET_ERR_PERMISSION_DENIED,
        POCKET_ERR_BUSY,             POCKET_ERR_LIMIT_EXCEEDED,
        POCKET_ERR_OUT_OF_MEMORY,    POCKET_ERR_TIMEOUT,
        POCKET_ERR_CANCELLED,        POCKET_ERR_CLOSED,
        POCKET_ERR_DISCONNECTED,     POCKET_ERR_NOT_FOUND,
        POCKET_ERR_CORRUPT_DATA,     POCKET_ERR_IO_ERROR,
        POCKET_ERR_AUTH_FAILED,      POCKET_ERR_TLS_ERROR,
        POCKET_ERR_CONFLICT,
    };
    for(unsigned i=0;i<sizeof(CODES)/sizeof(CODES[0]);i++)
        if(!strcmp(code,CODES[i])) return CODES[i];
    // The PC does not get to invent codes. Section 4's list is what an app
    // switches on, and an unknown one would read as a code the app forgot to
    // handle rather than as a peer speaking out of turn.
    return NULL;
}

// One delivery round of an event.
typedef struct {
    const char *topic;
    const char *json;
    size_t      json_len;
    uint32_t    sequence;
} bridge_event_t;

static bool event_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    const bridge_event_t *e=user;
    if(strcmp(event_topics[slot],e->topic)) return false;
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) { JS_FreeValue(ctx,JS_GetException(ctx)); return false; }
    JS_SetPropertyStr(ctx,object,"sequence",JS_NewUint32(ctx,e->sequence));
    JSValue value=JS_NULL;
    if(e->json_len) {
        value=JS_ParseJSON(ctx,e->json,e->json_len,"<bridge-event>");
        // An event is not a Promise and has nowhere to reject to. A payload
        // that is not JSON becomes null, and the sequence still tells the app
        // an event happened.
        if(JS_IsException(value)) { JS_FreeValue(ctx,JS_GetException(ctx)); value=JS_NULL; }
    }
    JS_SetPropertyStr(ctx,object,"payload",value);
    *payload=object;
    return true;
}

static void handle_frame(uint8_t *d, uint16_t length) {
    uint32_t session=read32(d+4), request=read32(d+8);
    uint16_t n=(uint16_t)(length-POCKET_BRIDGE_HEADER-4u);
    // The payload is read in place and handed to JS_ParseJSON, which needs
    // buf[buf_len]=='\0'. The four bytes after it are the CRC, and rx_finish
    // has already checked it, so its first byte is free to become the
    // terminator: no copy, and the slot is ours until the tail moves.
    d[POCKET_BRIDGE_HEADER+n]=0;
    const uint8_t *payload=d+POCKET_BRIDGE_HEADER;
    // A frame addressed to a session that has ended matches nothing and is
    // dropped in silence. Same discipline the request numbers use, and it is
    // what makes a PC that kept talking across a restart harmless.
    if(!link.id || session!=link.id) return;

    if(d[1]==POCKET_BRIDGE_EVENT) {
        if(!link.open || !event_table.open) return;
        const char *json=NULL; size_t json_len=0;
        const char *topic=split(payload,n,BRIDGE_TOPIC,&json,&json_len);
        if(!topic) return;
        bridge_event_t round={.topic=topic,.json=json,.json_len=json_len,
                              .sequence=request};
        pocket_api_sub_deliver(&event_table,event_payload,&round);
        return;
    }

    bridge_call_t *c=call_of(request);
    if(!c) return;
    switch(d[1]) {
    case POCKET_BRIDGE_WELCOME:
        if(c->kind==POCKET_BRIDGE_HELLO) call_answer(c,POCKET_STATUS_OK,payload,n);
        return;
    case POCKET_BRIDGE_RESPONSE:
        if(c->kind==POCKET_BRIDGE_REQUEST) call_answer(c,POCKET_STATUS_OK,payload,n);
        return;
    case POCKET_BRIDGE_ERROR:
        call_answer(c,BRIDGE_STATUS_REMOTE,payload,n);
        return;
    default:
        return;
    }
}

void pocket_bridge_pump(void) {
    unsigned tail=atomic_load(&inbox_tail);
    while(tail!=atomic_load(&inbox_head)) {
        bridge_slot_t *s=&inbox[tail&1u];
        handle_frame(s->data,s->length);
        // Released only after the frame has been read out of the slot.
        atomic_store(&inbox_tail,++tail);
    }
    unsigned lost=atomic_exchange(&inbox_dropped,0);
    // Counted rather than swallowed: section 13 expects a lost frame to be
    // recovered by the PC retrying the same requestId, and nobody can know to
    // look for that unless the loss is visible somewhere.
    if(lost) ESP_LOGW(TAG,"BRIDGE_DROPPED %u",lost);
}

// ----------------------------------------------------------------- outbound

// Fills a header and its payload, which is always "name '\0' tail" or just
// tail. Returns the body length, or 0 when it does not fit.
static size_t build(uint8_t *body, uint8_t kind, uint32_t request,
                    const char *name, size_t name_len,
                    const char *tail, size_t tail_len) {
    size_t length=(name?name_len+1:0)+tail_len;
    if(length>POCKET_BRIDGE_MAX_PAYLOAD) return 0;
    body[0]=POCKET_BRIDGE_VERSION;
    body[1]=kind;
    body[2]=(uint8_t)length;
    body[3]=(uint8_t)(length>>8);
    write32(body+4,link.id);
    write32(body+8,request);
    uint8_t *at=body+POCKET_BRIDGE_HEADER;
    if(name) { memcpy(at,name,name_len); at+=name_len; *at++=0; }
    if(tail_len) memcpy(at,tail,tail_len);
    return POCKET_BRIDGE_HEADER+length;
}

// ------------------------------------------------------------------ options
//
// Section 4 puts an argument error from a Promise-returning method into the
// rejection, so every exit here goes through pocket_api_reject().

typedef struct {
    int32_t timeout_ms;    // 0 for "not given"
    JSValue cancel;        // JS_UNDEFINED when none
    bool    cancelled;
} bridge_options_t;

static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, bridge_options_t *out) {
    out->timeout_ms=0;
    out->cancel=JS_UNDEFINED;
    out->cancelled=false;
    if(JS_IsUndefined(value)||JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    // null is absent, not a bad value: section 4 types timeoutMs optional and
    // pocket_io.c already reads it this way. One meaning for the whole shape.
    if(!JS_IsUndefined(timeout) && !JS_IsNull(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout)||JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        // Section 4 refuses to round an over-range request quietly.
        if(bad||!isfinite(ms)||ms!=(double)(int64_t)ms||ms<1||ms>BRIDGE_MAX_TIMEOUT_MS)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "timeoutMs must be a whole number of 1 to 30000",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel)&&!JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
        out->cancel=cancel;      // kept, and freed when the call settles
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------- the promises

static JSValue bridge_object(JSContext *ctx);

static void bridge_stop(void *user, const char *code) {
    (void)code;
    bridge_call_t *c=user;
    // There is no driver to wait for. A PC either answers or it does not, so
    // the host's own stop is the only thing that can end the wait, and posting
    // here is what turns that stop into a settlement on the next pump.
    if(!c->posted) { c->posted=true; pocket_api_complete(c->request,BRIDGE_STATUS_STOPPED); }
}

static void bridge_release(void *user) {
    bridge_call_t *c=user;
    free(c->answer);
    memset(c,0,sizeof(*c));
}

static JSValue bridge_settle(JSContext *ctx, void *user, int32_t status,
                             const char *stop_code, bool *rejected) {
    bridge_call_t *c=user;
    bool        hello=c->kind==POCKET_BRIDGE_HELLO;
    const char *op=hello?"bridge.connect":"bridge.request";
    *rejected=true;

    if(stop_code||status==BRIDGE_STATUS_STOPPED) {
        const char *code=stop_code?stop_code:POCKET_ERR_CLOSED;
        if(hello) { link.id=0; link.open=false; }   // a connect that never landed
        // The PC may well have run the method; nothing came back to say either
        // way, and section 4 reserves outcome=unknown for exactly that.
        return pocket_api_error(ctx,code,op,
                                !strcmp(code,POCKET_ERR_TIMEOUT)
                                    ?"the PC did not answer in time"
                                    :"the link ended before the answer",
                                true,POCKET_OUTCOME_UNKNOWN);
    }
    if(status==BRIDGE_STATUS_NO_MEMORY) {
        if(hello) { link.id=0; link.open=false; }
        return pocket_api_error(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                "the answer arrived and could not be kept",true,
                                POCKET_OUTCOME_UNKNOWN);
    }
    if(status==BRIDGE_STATUS_REMOTE) {
        const char *message=NULL; size_t message_len=0;
        const char *code=split(c->answer,c->answer_len,32,&message,&message_len);
        const char *mapped=code?known_code(code):NULL;
        if(hello) { link.id=0; link.open=false; }
        if(!mapped)
            return pocket_api_error(ctx,POCKET_ERR_CORRUPT_DATA,op,
                                    "the PC replied with an error this API does not define",
                                    false,POCKET_OUTCOME_UNKNOWN);
        char text[96];
        snprintf(text,sizeof(text),"%.*s",(int)message_len,message);
        // The PC refusing for BUSY is the one answer worth trying again; every
        // other code describes the request itself, and retryable is not an
        // instruction to resend regardless.
        return pocket_api_error(ctx,mapped,op,text[0]?text:NULL,
                                !strcmp(mapped,POCKET_ERR_BUSY),
                                POCKET_OUTCOME_UNKNOWN);
    }

    if(hello) {
        // The peerId comes back so an app can tell "some adapter answered" from
        // "the adapter I asked for". This is a name check and not
        // authentication: section 13 leaves pairing to the host, and this
        // firmware has no pairing store yet.
        size_t peer_len=strlen(link.peer);
        if(c->answer_len!=peer_len || memcmp(c->answer,link.peer,peer_len)) {
            link.id=0; link.open=false;
            return pocket_api_error(ctx,POCKET_ERR_NOT_FOUND,op,
                                    "another peer answered on this cable",false,
                                    POCKET_OUTCOME_NOT_APPLIED);
        }
        link.open=true;
        *rejected=false;
        return bridge_object(ctx);
    }
    // A response with no payload at all is `undefined`, which is what a method
    // that returns nothing should look like from JS.
    if(!c->answer_len) { *rejected=false; return JS_UNDEFINED; }
    JSValue value=JS_ParseJSON(ctx,(const char *)c->answer,c->answer_len,"<bridge>");
    if(JS_IsException(value)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_error(ctx,POCKET_ERR_CORRUPT_DATA,op,
                                "the PC's result was not JSON",false,
                                POCKET_OUTCOME_UNKNOWN);
    }
    *rejected=false;
    return value;
}

static const pocket_promise_ops_t bridge_ops = {
    .settle=bridge_settle, .stop=bridge_stop, .release=bridge_release,
};

// Claims a slot, sends the frame and arms the Promise. Takes over `cancel`, and
// sets *started when a call is actually in flight.
static JSValue start_call(JSContext *ctx, const char *operation, uint8_t kind,
                          const char *name, size_t name_len,
                          const char *tail, size_t tail_len,
                          bridge_options_t *options, bool *started) {
    *started=false;
    bridge_call_t *c=call_free();
    if(!c) {
        JS_FreeValue(ctx,options->cancel);
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,operation,
                                 "four bridge calls are already in flight",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options->cancelled) {
        JS_FreeValue(ctx,options->cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,operation,
                                 "cancelled before the frame went out",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // The request number is the wire's requestId. It is unique for the life of
    // the run, so a PC answering late -- after this app ended and another
    // started -- carries a number that matches nothing and is read by nobody.
    // That is the same property pocket_api.c documents for its drivers.
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options->cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,operation,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    uint8_t body[POCKET_BRIDGE_MAX_FRAME-4];
    size_t  len=build(body,kind,request,name,name_len,tail,tail_len);
    esp_err_t sent=len?pocket_bridge_emit(body,len):ESP_ERR_INVALID_SIZE;
    if(sent!=ESP_OK) {
        pocket_api_promise_abandon(request);
        JS_FreeValue(ctx,options->cancel);
        if(sent==ESP_ERR_INVALID_SIZE)
            return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,operation,
                                     "the frame is over maxFrameBytes",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        // The driver's send writes everything or nothing, so a refusal here is
        // "not applied" and not a guess.
        return pocket_api_reject(ctx,
                                 sent==ESP_ERR_INVALID_STATE?POCKET_ERR_NOT_AVAILABLE
                                                            :POCKET_ERR_IO_ERROR,
                                 operation,"the USB stream would not take the frame",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    c->request=request;
    c->kind=kind;
    c->answer=NULL;
    c->answer_len=0;
    c->posted=false;
    // Section 4 measures timeoutMs from the call, queue wait included; there is
    // no queue here, so it is simply how long the PC has to answer.
    int64_t deadline=esp_timer_get_time()+
        1000LL*(options->timeout_ms?options->timeout_ms:BRIDGE_DEFAULT_TIMEOUT_MS);
    JSValue promise=pocket_api_promise_arm(ctx,request,&bridge_ops,c,
                                           options->cancel,deadline);
    // A Promise that was not built has nothing to settle to; the slot is
    // already back, and the PC's answer will match no request.
    if(JS_IsException(promise)) memset(c,0,sizeof(*c));
    else *started=true;
    return promise;
}

// ---------------------------------------------------------------- the surface

// 1..max bytes of printable ASCII. Section 4 counts a text limit in UTF-8
// bytes; these three fields are identifiers on a wire the PC splits on NUL, so
// they are held to ASCII rather than merely to a byte count.
static bool identifier(const char *s, size_t len, size_t max) {
    if(!s||!len||len>max) return false;
    for(size_t i=0;i<len;i++)
        if((unsigned char)s[i]<0x21||(unsigned char)s[i]>0x7e) return false;
    return true;
}

static JSValue js_request(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="bridge.request";
    if(!link.open)
        return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,"the bridge is closed",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(argc<1)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "request(method, payload, options) needs a method",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    size_t      method_len=0;
    const char *method=JS_ToCStringLen(ctx,&method_len,argv[0]);
    // NULL here is the app's own toString throwing, and its exception is already
    // pending. Rejecting with INVALID_ARGUMENT on top would hide the real error.
    if(!method) return JS_EXCEPTION;
    if(!identifier(method,method_len,BRIDGE_METHOD)) {
        JS_FreeCString(ctx,method);
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "method must be 1 to 32 printable ASCII bytes",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    }
    // undefined is not JSON, and a method that takes no arguments is the common
    // case, so it travels as null rather than being refused.
    JSValue text=JS_JSONStringify(ctx,argc>1?argv[1]:JS_NULL,JS_UNDEFINED,JS_UNDEFINED);
    if(JS_IsException(text)||JS_IsUndefined(text)) {
        if(JS_IsException(text)) JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,text);
        JS_FreeCString(ctx,method);
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "payload must be JSON-serialisable",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    size_t      json_len=0;
    const char *json=JS_ToCStringLen(ctx,&json_len,text);
    JS_FreeValue(ctx,text);
    if(!json) { JS_FreeCString(ctx,method); return JS_EXCEPTION; }

    JSValue result;
    if(method_len+1+json_len>POCKET_BRIDGE_MAX_PAYLOAD) {
        result=pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "method and payload are over maxPayloadBytes",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    } else {
        bridge_options_t options;
        JSValue bad=take_options(ctx,argc>2?argv[2]:JS_UNDEFINED,OP,&options);
        if(!JS_IsUndefined(bad)) result=bad;
        else {
            bool started=false;
            result=start_call(ctx,OP,POCKET_BRIDGE_REQUEST,method,method_len,
                              json,json_len,&options,&started);
        }
    }
    JS_FreeCString(ctx,json);
    JS_FreeCString(ctx,method);
    return result;
}

static JSValue js_on_event(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="bridge.onEvent";
    if(!link.open)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,OP,"the bridge is closed",
                                false,NULL);
    static const char *const SHAPE="onEvent(topic, listener) needs a topic of 1 to "
                                   "23 printable ASCII bytes and a function";
    if(argc<2||!JS_IsFunction(ctx,argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,SHAPE,false,NULL);
    size_t      topic_len=0;
    const char *topic=JS_ToCStringLen(ctx,&topic_len,argv[0]);
    if(!topic) return JS_EXCEPTION;      // the app's toString threw; leave it be
    if(!identifier(topic,topic_len,BRIDGE_TOPIC-1)) {
        JS_FreeCString(ctx,topic);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,SHAPE,false,NULL);
    }
    int     slot=0;
    JSValue subscription=pocket_api_sub_open(ctx,&event_table,argv[1],OP,
                                             "too many event listeners",&slot);
    if(!JS_IsException(subscription))
        snprintf(event_topics[slot],BRIDGE_TOPIC,"%.*s",(int)topic_len,topic);
    JS_FreeCString(ctx,topic);
    return subscription;
}

// Ends the session. Section 4: close() refuses new work at once and finishes
// what is pending with CLOSED rather than leaving it to a timeout.
static JSValue js_close(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if(!link.id) return JS_UNDEFINED;               // idempotent
    uint8_t body[POCKET_BRIDGE_HEADER];
    // Best effort, so the PC can drop what it was still working on. A cable
    // that is already gone changes nothing here.
    if(build(body,POCKET_BRIDGE_BYE,0,NULL,0,NULL,0))
        pocket_bridge_emit(body,POCKET_BRIDGE_HEADER);
    link.id=0;
    link.open=false;
    pocket_api_sub_close_all(&event_table);
    for(int i=0;i<BRIDGE_CALLS;i++)
        if(calls[i].request&&!calls[i].posted) {
            calls[i].posted=true;
            pocket_api_complete(calls[i].request,BRIDGE_STATUS_STOPPED);
        }
    return JS_UNDEFINED;
}

static JSValue bridge_object(JSContext *ctx) {
    JSValue bridge=JS_NewObject(ctx);
    if(JS_IsException(bridge)) return bridge;
    JS_SetPropertyStr(ctx,bridge,"request",JS_NewCFunction(ctx,js_request,"request",3));
    JS_SetPropertyStr(ctx,bridge,"onEvent",JS_NewCFunction(ctx,js_on_event,"onEvent",2));
    JS_SetPropertyStr(ctx,bridge,"close",JS_NewCFunction(ctx,js_close,"close",0));
    // Not in section 13's type. It is here because a program that logs it can be
    // matched against the frames on the wire without a packet capture.
    JS_SetPropertyStr(ctx,bridge,"sessionId",JS_NewUint32(ctx,link.id));
    return bridge;
}

static JSValue js_connect(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="bridge.connect";
    if(argc<1||!JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "connect({transport, peerId}) needs an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue field=JS_GetPropertyStr(ctx,argv[0],"transport");
    if(JS_IsException(field)) return JS_EXCEPTION;
    const char *transport=JS_ToCString(ctx,field);
    JS_FreeValue(ctx,field);
    // A missing property converts to "usb"'s neighbour "undefined" and is
    // refused below; NULL only happens when the app's own toString threw.
    if(!transport) return JS_EXCEPTION;
    bool usb=!strcmp(transport,"usb"), wifi=!strcmp(transport,"wifi");
    JS_FreeCString(ctx,transport);
    // "wifi" is in the document and not in this firmware. Section 2 wants that
    // difference to read as UNSUPPORTED and not as a spelling mistake.
    if(!usb)
        return pocket_api_reject(ctx,wifi?POCKET_ERR_UNSUPPORTED
                                         :POCKET_ERR_INVALID_ARGUMENT,OP,
                                 wifi?"this firmware bridges over usb only"
                                     :"transport must be \"usb\"",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    field=JS_GetPropertyStr(ctx,argv[0],"peerId");
    if(JS_IsException(field)) return JS_EXCEPTION;
    size_t      peer_len=0;
    const char *peer=JS_ToCStringLen(ctx,&peer_len,field);
    JS_FreeValue(ctx,field);
    if(!peer) return JS_EXCEPTION;
    if(!identifier(peer,peer_len,BRIDGE_PEER-1)) {
        JS_FreeCString(ctx,peer);
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "peerId must be 1 to 31 printable ASCII bytes",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(link.id) {
        JS_FreeCString(ctx,peer);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a bridge session is already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    bridge_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&options);
    if(!JS_IsUndefined(bad)) { JS_FreeCString(ctx,peer); return bad; }

    // The session number is what keeps a reply meant for a previous run from
    // being read by this one. The clock is the only thing on this board that
    // differs between two runs before anything else has happened.
    static uint32_t previous;
    uint32_t id=(uint32_t)esp_timer_get_time();
    if(!id||id==previous) id=previous+1;
    if(!id) id=1;
    previous=id;
    link.id=id;
    link.open=false;
    snprintf(link.peer,sizeof(link.peer),"%.*s",(int)peer_len,peer);

    bool    started=false;
    JSValue promise=start_call(ctx,OP,POCKET_BRIDGE_HELLO,peer,peer_len,
                               POCKET_API_VERSION,strlen(POCKET_API_VERSION),
                               &options,&started);
    JS_FreeCString(ctx,peer);
    // Nothing went out, so there is no session either. settle() clears it for a
    // HELLO that did leave; this is the path where none did.
    if(!started) { link.id=0; link.peer[0]=0; }
    return promise;
}

// ------------------------------------------------------------------ lifecycle

void pocket_bridge_reset(void) {
    pocket_api_sub_close_all(&event_table);
    event_table.ctx=NULL;
    link.id=0;
    link.open=false;
    link.peer[0]=0;
    // Whatever the input task left in the ring was addressed to a session that
    // no longer exists. Dropping it here means the next program starts with an
    // empty inbox rather than with the last one's mail.
    atomic_store(&inbox_tail,atomic_load(&inbox_head));
    atomic_store(&inbox_dropped,0);
    // The calls themselves belong to pocket_api_reset(), which runs after this
    // and reaches each one through bridge_stop() and bridge_release().
}

// ----------------------------------------------------------------- capability

static const pocket_limit_t bridge_limits[] = {
    {.name="transports",         .kind=POCKET_LIMIT_TEXT, .text="usb"},
    {.name="maxFrameBytes",      .kind=POCKET_LIMIT_INT,  .number=POCKET_BRIDGE_MAX_FRAME},
    {.name="maxPayloadBytes",    .kind=POCKET_LIMIT_INT,  .number=POCKET_BRIDGE_MAX_PAYLOAD},
    {.name="maxMethodBytes",     .kind=POCKET_LIMIT_INT,  .number=BRIDGE_METHOD},
    {.name="maxTopicBytes",      .kind=POCKET_LIMIT_INT,  .number=BRIDGE_TOPIC-1},
    {.name="maxPeerIdBytes",     .kind=POCKET_LIMIT_INT,  .number=BRIDGE_PEER-1},
    {.name="maxSessions",        .kind=POCKET_LIMIT_INT,  .number=1},
    {.name="maxPendingRequests", .kind=POCKET_LIMIT_INT,  .number=BRIDGE_CALLS},
    {.name="maxEventListeners",  .kind=POCKET_LIMIT_INT,  .number=BRIDGE_LISTENERS},
    {.name="defaultTimeoutMs",   .kind=POCKET_LIMIT_INT,  .number=BRIDGE_DEFAULT_TIMEOUT_MS},
    {.name="maxTimeoutMs",       .kind=POCKET_LIMIT_INT,  .number=BRIDGE_MAX_TIMEOUT_MS},
    {0},
};

// Whether a PC is listening is not observable here. usb_serial_jtag_is_connected()
// would answer it, but its own header says it costs time on every FreeRTOS tick,
// and one capability probe is not worth a system-wide tick cost. What is honest
// is whether the stream exists at all; section 2 already says available is an
// observation and that the open is the final word.
static void bridge_probe(const pocket_capability_t *cap, bool *available,
                         const char **reason) {
    (void)cap;
    *available=usb_serial_jtag_is_driver_installed();
    *reason=*available?NULL:POCKET_REASON_NO_DEVICE;
}

static const pocket_capability_t bridge_capability = {
    .name="bridge.pc", .supported=true, .available=false,
    .reason=POCKET_REASON_NO_DEVICE, .limits=bridge_limits, .probe=bridge_probe,
};

static esp_err_t build_bridge(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    for(int i=0;i<BRIDGE_LISTENERS;i++) {
        // A realm going away takes its callbacks with it, so the table starts
        // empty every time this is built.
        event_slots[i].callback=JS_UNDEFINED;
        event_slots[i].handle=0;
        event_topics[i][0]=0;
    }
    event_table.open=0;
    event_table.ctx=ctx;
    memset(calls,0,sizeof(calls));
    link.id=0; link.open=false; link.peer[0]=0;

    JS_SetPropertyStr(ctx,(JSValue)ns,"connect",
                      JS_NewCFunction(ctx,js_connect,"connect",2));
    return ESP_OK;
}

esp_err_t pocket_bridge_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&bridge_capability);
    return pocket_api_lazy(ctx,"bridge",build_bridge,NULL);
}
