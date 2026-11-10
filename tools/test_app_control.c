#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
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
static bool blocked,stop_fails,write_fails,jump_on_tap,jump_on_accepted;
static uint64_t now_ms;
static bool test_native_suspend,running,guest_present,can_suspend,services_suspended;
static unsigned suspend_calls,suspend_runs,queue_resets,home_entries;
static esp_err_t suspend_result;
static uint32_t native_generation;
enum { SCREEN_HOME, SCREEN_RUN };
static int screen;
static void *keys;
static bool home_modal(void) { return false; }
static bool pocket_text_active(void) { return false; }
static bool app_can_suspend(void) { return can_suspend; }
static esp_err_t app_suspend(void) { suspend_calls++;services_suspended=true;return suspend_result; }
static bool app_guest_present(void) { return guest_present; }
static uint32_t app_guest_generation(void) { return guest_present?generation+(stop_fails?1:0):0; }
static void app_stop(void) { stops++;guest_present=false;services_suspended=false; }
static const char *app_dormant_id(void) { return ""; }
static const app_control_descriptor_t *app_registry_current(void) {
    static const app_control_descriptor_t app={"local.hello","HELLO WORLD",5};return &app;
}
static void xQueueReset(void *queue) { assert(queue==keys);queue_resets++; }
static void enter(int target) { assert(target==SCREEN_HOME);screen=target;home_entries++; }
static void suspend_run(void) { suspend_runs++;running=false;screen=SCREEN_HOME; }
#include "app_control_native.inc"
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
    native_generation=generation;running=guest_present=true;screen=SCREEN_RUN;
    starts++;return AC_OK;
}
static uint8_t tap(uint8_t action) {
    assert(action>=1 && action<=6);
    if(blocked)return AC_BLOCKED;
    if(jump_on_tap)now_ms+=90000;
    taps++;return AC_OK;
}
static uint8_t suspend_guest(void) { return test_native_suspend?native_suspend():AC_OK; }
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
    if(jump_on_accepted && body[32]==AC_ACCEPTED)now_ms+=2;
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
    blocked=stop_fails=write_fails=jump_on_tap=jump_on_accepted=false;now_ms=0;
    test_native_suspend=running=guest_present=services_suspended=false;can_suspend=true;
    suspend_calls=suspend_runs=queue_resets=home_entries=0;suspend_result=ESP_OK;
    native_generation=0;screen=SCREEN_HOME;
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

static diagnostic_control diag;
static diagnostic_session diag_session=DIAGNOSTIC_SESSION_INIT;
static diagnostic_wire diag_wire;
static pthread_barrier_t diag_barrier;
static unsigned diag_work,diag_restore,diag_release;
static bool diag_raw_emit(void *ctx,const uint8_t *p,size_t n){(void)ctx;return usb_console_frame(p,n)==ESP_OK;}
static void diag_send(const char *s){
    incoming_size=strlen(s);assert(incoming_size<=sizeof incoming);
    memcpy(incoming,s,incoming_size);incoming_at=1;
    assert(pocket_bridge_usb(incoming[0]));assert(incoming_at==incoming_size);
    assert(emitted_size<=80);emitted[emitted_size]=0;
}
static void diag_wait(void){int e=pthread_barrier_wait(&diag_barrier);assert(e==0||e==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *diag_acquire(void *ctx){return ctx;}
static bool diag_drain(void *ctx){(void)ctx;return true;}
static bool diag_body(void *ctx,void *owner){assert(ctx==owner);diag_work++;diag_wait();diag_wait();return true;}
static bool diag_home(void *ctx){(void)ctx;diag_restore++;return true;}
static void diag_free(void *ctx,void *owner){assert(ctx==owner);diag_release++;}
static void *diag_owner(void *arg){
    (void)arg;diagnostic_lifecycle_ops ops={&diag,diag_acquire,diag_drain,diag_body,diag_home,diag_free};
    assert(diagnostic_control_pump(&diag,&ops,true)==DC_OWNER_SAFE_END);return NULL;
}
static void diagnostic_transport_tests(void){
    /* No UI/control pump is involved in D replies, even during live work. */
    assert(diagnostic_control_init(&diag,UINT64_C(0x123456789abcdef0),false,NULL));
    diagnostic_wire_init(&diag_wire,&diag,diag_raw_emit,NULL);pocket_bridge_bind_diagnostic(&diag_wire);
    diag_send("\x1d" "D1 STATUS 00000000000000000000000000000000 0000000000000000\n");
    assert(strstr((char *)emitted,"REFUSED 00 123456789abcdef0 0"));
    diag_send("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"REFUSED 20")&&!atomic_load(&diag.used));
    unsigned char malformed[]={0x1d,'D','2',' ','q',27,'\n'};
    for(unsigned i=0;i<sizeof malformed;i++)assert(pocket_bridge_usb(malformed[i]));
    assert(!pocket_bridge_usb('q')&&!pocket_bridge_usb(27));
    assert(pocket_bridge_usb(0x1d));assert(pocket_bridge_usb('D'));
    for(unsigned i=0;i<1000;i++)assert(pocket_bridge_usb('q'));
    assert(pocket_bridge_usb(27)&&pocket_bridge_usb('\n'));
    assert(!pocket_bridge_usb('q'));
    assert(pocket_bridge_usb(0x1d)&&pocket_bridge_usb('D')&&pocket_bridge_usb('1'));
    /* New GS resynchronizes a truncated D frame into a complete D frame. */
    diag_send("\x1d" "D1 STATUS 00000000000000000000000000000000 0000000000000000\n");
    assert(strstr((char *)emitted,"REFUSED 00"));
    assert(diagnostic_control_init(&diag,UINT64_C(0x123456789abcdef0),true,&diag_session));
    diag_send("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"PENDING 20"));
    assert(!pthread_barrier_init(&diag_barrier,NULL,2));pthread_t owner;
    assert(!pthread_create(&owner,NULL,diag_owner,NULL));diag_wait();
    diag_send("\x1d" "D1 STATUS 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"RUNNING 00"));
    diag_send("\x1d" "D1 CANCEL 22222222222222222222222222222222 123456789abcdef0\n");
    assert(strstr((char *)emitted,"REFUSED 00"));
    diag_send("\x1d" "D1 CANCEL 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"CANCEL_REQUESTED 00"));
    diag_wait();assert(!pthread_join(owner,NULL));assert(!pthread_barrier_destroy(&diag_barrier));
    diag_send("\x1d" "D1 STATUS 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"CANCELLED 1f")&&diag_work==1&&diag_restore==1&&diag_release==1);
    /* Retried REQUEST is a receipt read; another owner pump does no work. */
    diag_send("\x1d" "D1 REQUEST 11111111111111111111111111111111 123456789abcdef0\n");
    assert(strstr((char *)emitted,"CANCELLED 1f")&&diag_work==1);
    assert(diagnostic_control_pump(&diag,NULL,true)==DC_OWNER_NONE);
    pocket_bridge_bind_diagnostic(NULL);
    printf("DIAGNOSTIC_TRANSPORT PASS: reserved D/disabled/no key leakage/status+cancel while UI blocked/receipt replay\n");
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
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");
    now_ms=89999;jump_on_accepted=true;before=reply_count;
    send(AC_TAP,2,2,1,1,NULL);
    assert(reply_count==before+2 && replies[before][32]==AC_ACCEPTED);
    assert(result()==AC_EXPIRED && state()==AC_STOPPED && taps==0 && stops==1);
    send(AC_STATUS,2,2,1,0,NULL);assert(result()==AC_EXPIRED && taps==0 && stops==1);
    send(AC_TAP,3,3,1,1,NULL);assert(result()==AC_STALE_SESSION && taps==0);

    /* Actual main.c hooks: unsupported preflight keeps the guest; an attempted
     * suspend that fails after releasing services must end it and return HOME. */
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");test_native_suspend=true;
    can_suspend=false;send(AC_SUSPEND,2,2,1,0,NULL);
    assert(result()==AC_UNSUPPORTED && state()==AC_RUNNING && running && guest_present);
    assert(!suspend_calls && !stops && !services_suspended);
    can_suspend=true;suspend_result=ESP_ERR_TIMEOUT;send(AC_SUSPEND,3,3,1,0,NULL);
    assert(result()==AC_UNSUPPORTED && state()==AC_STOPPED && !running && !guest_present);
    assert(suspend_calls==1 && stops==1 && !services_suspended && !suspend_runs);
    assert(screen==SCREEN_HOME && queue_resets==1 && home_entries==1);
    send(AC_TAP,4,4,1,1,NULL);assert(result()==AC_BLOCKED && !taps);
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");test_native_suspend=true;
    suspend_result=ESP_ERR_TIMEOUT;stop_fails=true;send(AC_SUSPEND,2,2,1,0,NULL);
    assert(result()==AC_TEARDOWN_FAILED && state()==AC_NEEDS_REVIEW && guest_present && !stops);
    send(AC_TAP,3,3,1,1,NULL);assert(result()==AC_BLOCKED && !taps);
    reset();open_session();send(AC_START,1,1,0,0,"local.hello");test_native_suspend=true;
    send(AC_SUSPEND,2,2,1,0,NULL);
    assert(result()==AC_OK && state()==AC_SUSPENDED && !running && guest_present);
    assert(suspend_calls==1 && suspend_runs==1 && !stops);
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
    diagnostic_transport_tests();
    printf("APP_CONTROL PASS: identity/lifecycle/order/dedup/ACK loss/queue/rate/TTL/legacy CRC; static_bytes=%zu\n",app_control_static_bytes());
}
