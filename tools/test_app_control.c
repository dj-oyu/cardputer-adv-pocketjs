#include "app_control.h"
#include "pocket_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

enum { ESP_OK=0, ESP_ERR_INVALID_SIZE=1, ESP_ERR_INVALID_STATE=2, ESP_ERR_TIMEOUT=3 };
typedef int esp_err_t;
#define POCKET_BRIDGE_VERSION 1
#define POCKET_BRIDGE_HEADER 12
#define POCKET_BRIDGE_MAX_FRAME 496
#define POCKET_BRIDGE_MAX_WIRE 995
static uint8_t emitted[995], incoming[995], replies[256][132];
static size_t emitted_size,incoming_size,incoming_at;
static unsigned reply_count,starts,taps,stops;
static unsigned inject_remaining;
static void inject_status(void);
static uint32_t generation;
static bool blocked,stop_fails,write_fails,jump_on_tap;
static uint64_t now_ms;
bool usb_serial_jtag_is_driver_installed(void) { return true; }
int usb_serial_jtag_read_bytes(void *data,size_t size,unsigned wait) {
    assert(size==1 && wait==0);
    if(incoming_at==incoming_size)return 0;
    *(uint8_t *)data=incoming[incoming_at++];return 1;
}
int usb_console_frame(const void *data,size_t length) {
    assert(length<=sizeof(emitted));memcpy(emitted,data,length);emitted_size=length;return ESP_OK;
}
#include "app_control_transport.inc"
static unsigned count(void) { return 4; }
static bool catalog(unsigned i,app_control_descriptor_t *out) {
    static const app_control_descriptor_t apps[]={
        {"local.hello","HELLO WORLD",5},{"local.stress","STRESS TEST",5},
        {"local.pet","PET",7},{"local.vmprobe","VM PROBE",0},
    };
    if(i>=count())return false;
    *out=apps[i];return true;
}
static uint8_t start(const char *id,bool resume) {
    assert(!strcmp(id,"local.hello") || !strcmp(id,"local.stress") || !strcmp(id,"local.pet"));
    if(!resume)generation++;
    starts++;return AC_OK;
}
static uint8_t tap(uint8_t action) {
    assert(action>=1 && action<=6);
    if(blocked)return AC_BLOCKED;
    if(jump_on_tap)now_ms+=90000;
    taps++;return AC_OK;
}
static uint8_t suspend_guest(void) { return AC_OK; }
static bool stop_guest(void) { stops++;return !stop_fails; }
static uint32_t instance(void) { return generation; }
static uint64_t clock_ms(void) { return now_ms; }
static void fingerprint(uint8_t hash[32],char version[48]) {
    memset(hash,0xa5,32);memcpy(version,"host-fixture",13);
}
static bool emit(const uint8_t *body,size_t length) {
    assert(length==132 && reply_count<256);
    if(write_fails)return false;
    memcpy(replies[reply_count++],body,length);
    if(inject_remaining) { inject_remaining--;inject_status(); }
    return true;
}
static void reset(void) {
    const app_control_hooks_t hooks={.catalog_count=count,.catalog=catalog,.start=start,
        .tap=tap,.suspend=suspend_guest,.stop=stop_guest,.emit=emit,
        .fingerprint=fingerprint,.instance=instance,.clock_ms=clock_ms};
    app_control_init(UINT64_C(0x123456780000002a),&hooks);
    reply_count=starts=taps=stops=0;generation=0;
    inject_remaining=0;
    blocked=stop_fails=write_fails=jump_on_tap=false;now_ms=0;
    atomic_store(&inbox_head,0);atomic_store(&inbox_tail,0);rx_state=RX_IDLE;
}
static void make(uint8_t body[72],uint8_t op,uint32_t id,uint32_t seq,uint32_t run,uint8_t arg,const char *app) {
    memset(body,0,72);body[0]=1;body[1]=op;body[2]=60;
    pocket_wire_write32(body+4,42);pocket_wire_write32(body+8,id);
    pocket_wire_write32(body+12,42);pocket_wire_write32(body+16,0x12345678);
    pocket_wire_write32(body+20,run);pocket_wire_write32(body+24,seq);
    pocket_wire_write32(body+28,7);body[68]=arg;
    if(app)memcpy(body+36,app,strlen(app));
}
static void feed(const uint8_t *body,size_t length) {
    assert(pocket_bridge_emit_namespace('C',body,length)==ESP_OK);
    memcpy(incoming,emitted,emitted_size);incoming_size=emitted_size;incoming_at=1;
    assert(pocket_bridge_usb(incoming[0]));assert(incoming_at==incoming_size);
}
static void inject_status(void) {
    uint8_t body[72];make(body,AC_STATUS,0,0,1,0,NULL);
    app_control_receive(body,sizeof(body));
}
static void send(uint8_t op,uint32_t id,uint32_t seq,uint32_t run,uint8_t arg,const char *app) {
    uint8_t body[72];make(body,op,id,seq,run,arg,app);feed(body,72);app_control_pump(now_ms);
}
static uint8_t result(void) { assert(reply_count);return replies[reply_count-1][32]; }
static uint8_t state(void) { assert(reply_count);return replies[reply_count-1][33]; }
static uint32_t run(void) { return pocket_wire_read32(replies[reply_count-1]+20); }
static uint32_t next(void) { return pocket_wire_read32(replies[reply_count-1]+36); }
static void open_session(void) {
    uint8_t body[72];make(body,AC_OPEN,0,0,0,0,NULL);
    pocket_wire_write32(body+32,90000);feed(body,72);app_control_pump(now_ms);
    assert(result()==AC_OK && state()==AC_READY && next()==1);
}
int main(void) {
    reset();uint8_t body[72];memset(body,0,72);body[0]=1;body[1]=AC_CATALOG;body[2]=60;
    feed(body,72);app_control_pump(now_ms);
    assert(result()==AC_OK && pocket_wire_read32(replies[0]+4)==42);
    assert(!strcmp((char *)replies[0]+52,"local.hello"));
    pocket_wire_write32(body+24,0xffff);feed(body,72);app_control_pump(now_ms);
    assert(result()==AC_OK && replies[1][52]==0xa5 && !strcmp((char *)replies[1]+84,"host-fixture"));
    open_session();now_ms=1000;open_session();
    assert(pocket_wire_read32(replies[reply_count-1]+40)==89000); /* no TTL extension */
    send(AC_START,1,1,0,0,"missing");assert(result()==AC_UNKNOWN_APP && starts==0);
    send(AC_START,2,2,0,0,"local.vmprobe");assert(result()==AC_UNSUPPORTED && starts==0);
    send(AC_START,3,3,0,0,"local.hello");assert(result()==AC_OK && state()==AC_RUNNING && run()==1 && starts==1);
    send(AC_START,3,3,0,0,"local.hello");assert(result()==AC_OK && starts==1);
    send(AC_START,3,3,0,0,"local.stress");assert(result()==AC_CONFLICT && starts==1);
    send(AC_STATUS,3,3,1,0,NULL);assert(result()==AC_OK && replies[reply_count-1][34]==AC_START);
    unsigned before=reply_count;send(AC_TAP,4,4,1,1,NULL);
    assert(reply_count==before+2 && replies[before][32]==AC_ACCEPTED && result()==AC_CONSUMED && taps==1);
    send(AC_TAP,4,4,1,1,NULL);assert(result()==AC_CONSUMED && taps==1);
    send(AC_TAP,4,4,1,2,NULL);assert(result()==AC_CONFLICT && taps==1);
    blocked=true;send(AC_TAP,5,5,1,1,NULL);assert(result()==AC_BLOCKED && taps==1);blocked=false;
    send(AC_TAP,6,6,99,1,NULL);assert(result()==AC_STALE_RUN && next()==6);
    make(body,AC_TAP,6,6,1,1,NULL);body[12]^=1;feed(body,72);app_control_pump(now_ms);assert(result()==AC_STALE_BOOT);
    make(body,AC_TAP,6,6,1,1,NULL);body[28]^=1;feed(body,72);app_control_pump(now_ms);assert(result()==AC_STALE_LEASE);
    send(AC_TAP,7,7,1,1,NULL);assert(result()==AC_OUT_OF_ORDER);
    send(AC_SUSPEND,6,6,1,0,NULL);assert(result()==AC_OK && state()==AC_SUSPENDED);
    send(AC_START,7,7,1,1,"local.hello");assert(result()==AC_OK && state()==AC_RUNNING && run()==1 && generation==1);
    send(AC_SUSPEND,8,8,1,0,NULL);assert(result()==AC_OK);
    send(AC_START,9,9,0,0,"local.stress");assert(result()==AC_OK && run()==2 && generation==2);
    now_ms=2100;write_fails=true;send(AC_TAP,10,10,2,1,NULL);assert(taps==2);write_fails=false;
    send(AC_STATUS,10,10,2,0,NULL);assert(result()==AC_CONSUMED && taps==2);
    send(AC_STATUS,500,10,2,0,NULL);assert(result()==AC_OUTCOME_UNKNOWN && taps==2);
    send(AC_CLOSE,11,11,2,0,NULL);assert(result()==AC_BUSY);
    send(AC_STOP,12,12,2,0,NULL);assert(result()==AC_OK && state()==AC_STOPPED);
    send(AC_CLOSE,13,13,2,0,NULL);assert(result()==AC_OK && state()==AC_NO_SESSION);
    send(AC_CLOSE,13,13,2,0,NULL);assert(result()==AC_OK);
    make(body,AC_OPEN,0,0,0,0,NULL);pocket_wire_write32(body+32,90000);feed(body,72);app_control_pump(now_ms);
    assert(result()==AC_STALE_SESSION); /* retired OPEN cannot clear dedup */
    send(AC_START,14,14,0,0,"local.hello");assert(result()==AC_STALE_SESSION && starts==3);

    reset();open_session();send(AC_START,1,1,0,0,"local.pet");
    /* Full native inbox has a dedicated stable queue_full receipt. */
    for(unsigned i=2;i<=6;i++){make(body,AC_TAP,i,i,1,1,NULL);feed(body,72);}
    app_control_pump(now_ms);assert(result()==AC_QUEUE_FULL && taps==4 && next()==7);
    send(AC_STATUS,6,6,1,0,NULL);assert(result()==AC_QUEUE_FULL);
    send(AC_TAP,6,6,1,1,NULL);assert(result()==AC_QUEUE_FULL && taps==4);
    generation++;app_control_observe(true,"","local.pet",generation);
    send(AC_TAP,7,7,1,1,NULL);assert(result()==AC_BLOCKED && taps==4); /* same ID, different guest */
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");
    now_ms=90000;app_control_pump(now_ms);assert(stops==1);
    send(AC_TAP,2,2,1,1,NULL);assert(result()==AC_STALE_SESSION && taps==0);
    make(body,AC_OPEN,0,0,0,0,NULL);pocket_wire_write32(body+32,90000);feed(body,72);app_control_pump(now_ms);assert(result()==AC_STALE_SESSION);
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");stop_fails=true;
    now_ms=90000;app_control_pump(now_ms);send(AC_STATUS,0,0,1,0,NULL);assert(state()==AC_NEEDS_REVIEW);
    stop_fails=false;app_control_pump(now_ms);send(AC_STATUS,0,0,1,0,NULL);assert(state()==AC_STOPPED);
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");jump_on_tap=true;
    make(body,AC_TAP,2,2,1,1,NULL);feed(body,72);
    make(body,AC_TAP,3,3,1,1,NULL);feed(body,72);app_control_pump(now_ms);
    assert(taps==1 && stops==1 && result()==AC_STALE_SESSION); /* TTL across a blocking turn */
    /* Fixed user rate and lifetime budget, with lifecycle cleanup reserved. */
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");
    for(unsigned i=2;i<=10;i++)send(AC_TAP,i,i,1,1,NULL);
    send(AC_TAP,11,11,1,1,NULL);assert(result()==AC_LIMIT && taps==9);
    for(unsigned i=12;i<=61;i++){now_ms+=1000;send(AC_TAP,i,i,1,1,NULL);}
    send(AC_TAP,62,62,1,1,NULL);assert(result()==AC_LIMIT && taps==59);
    send(AC_STOP,63,63,1,0,NULL);assert(result()==AC_OK);
    send(AC_CLOSE,64,64,1,0,NULL);assert(result()==AC_OK);
    /* A producer that replenishes during every blocking reply must not keep
     * the UI owner in this pump indefinitely. Each turn drains its snapshot. */
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");
    inject_remaining=100;unsigned before_flood=reply_count;
    send(AC_STATUS,0,0,1,0,NULL);
    assert(reply_count==before_flood+1 && inject_remaining==99);
    app_control_pump(now_ms);
    assert(reply_count==before_flood+2 && inject_remaining==98);
    inject_remaining=0;app_control_pump(now_ms);

    /* B and C parser isolation: fill guest inbox; native catalog still works. */
    reset();uint8_t legacy[12]={1,6};
    for(unsigned i=0;i<2;i++) {
        assert(pocket_bridge_emit(legacy,12)==ESP_OK);memcpy(incoming,emitted,emitted_size);
        incoming_size=emitted_size;incoming_at=1;assert(pocket_bridge_usb(incoming[0]));
    }
    assert(atomic_load(&inbox_head)==2);
    memset(body,0,72);body[0]=1;body[1]=AC_CATALOG;body[2]=60;
    feed(body,72);app_control_pump(now_ms);assert(result()==AC_OK && atomic_load(&inbox_head)==2);
    assert(!pocket_bridge_usb('a'));
    unsigned recorded=reply_count;
    assert(pocket_bridge_emit_namespace('C',body,72)==ESP_OK);
    emitted[emitted_size-3]^=1;memcpy(incoming,emitted,emitted_size);incoming_size=emitted_size;incoming_at=1;
    assert(pocket_bridge_usb(incoming[0]));app_control_pump(now_ms);assert(reply_count==recorded);
    /* A trailing odd nibble is malformed, rather than silently ignored. */
    assert(pocket_bridge_emit_namespace('C',body,72)==ESP_OK);
    memcpy(incoming,emitted,emitted_size-1);incoming[emitted_size-1]='a';incoming[emitted_size]='\n';
    incoming_size=emitted_size+1;incoming_at=1;assert(pocket_bridge_usb(incoming[0]));
    app_control_pump(now_ms);assert(reply_count==recorded);
    assert(atomic_load(&inbox_dropped)==0); /* malformed C does not disturb B */
    printf("APP_CONTROL PASS: identity/lifecycle/order/dedup/ACK loss/queue/rate/TTL/legacy CRC; static_bytes=%zu\n",app_control_static_bytes());
}
