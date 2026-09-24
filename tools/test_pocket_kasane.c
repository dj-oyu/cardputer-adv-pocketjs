// End-to-end host contract for pocket.kasane with the real QuickJS and the
// real fixed-storage DS core/cache/modal/renderer.
#include "pocket_kasane.h"
#include "pocket_clock.h"
#include "app_view_assets.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "text/ksn_font.h"
#include "pet/ksn_pet.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

void host_capabilities_clear(void);

static JSRuntime *rt;
static JSContext *ctx;
static unsigned failures;
static uint16_t strip_pixels[240*8];
static uint16_t panel_pixels[240*135],committed_pixels[240*135];
static unsigned transfers;
static bool fail_once;
static int fail_band=-1,invalidate_band=-1;
static int32_t test_player_id;
static pocket_av_ui_snapshot test_player_ui;
static bool test_clock_valid;
static sys_clock_state test_clock_ui;
static unsigned test_clock_reads;
typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    ksn_source_handle handle;
    ksn_schema_value field;
    char text[8];
    uint64_t revision,expires_at_us;
    unsigned acquired,released;
    bool allowed,fail;
} external_test_source;
static external_test_source external_test[2];
static const ksn_slot_type external_test_types[]={KSN_SLOT_TEXT};
static ksn_result external_test_acquire(void *context,uint64_t cursor,
                                        uint64_t now_us,ksn_source_snapshot *out){
    (void)cursor;(void)now_us;
    external_test_source *source=context;
    if(source->fail)return KSN_IO;
    source->field.data.text=(ksn_schema_text){source->text,
        (uint16_t)strlen(source->text)};
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.generation=source->handle.generation,
        .revision=source->revision,.expires_at_us=source->expires_at_us,
        .valid_fields=1,.changed_fields=1,
        .fields=&source->field};
    source->acquired++;
    return KSN_OK;
}
static void external_test_release(void *context,const ksn_source_snapshot *snapshot){
    (void)snapshot;((external_test_source *)context)->released++;
}
static bool external_test_allow(void *context,uint32_t consumer){
    return consumer!=0&&((external_test_source *)context)->allowed;
}
static bool external_test_open(unsigned index,const char *text){
    external_test_source *source=&external_test[index];
    memset(source,0,sizeof(*source));
    source->allowed=true;source->revision=1;
    strcpy(source->text,text);
    source->provider=(ksn_source_provider){.size=sizeof(source->provider),
        .version=KSN_SOURCE_ABI_VERSION,.field_count=1,
        .field_types=external_test_types,.context=source,
        .acquire=external_test_acquire,.release=external_test_release,
        .allow=external_test_allow};
    ksn_source_registry_init(&source->registry);
    return ksn_source_register(&source->registry,&source->provider,
                               &source->handle)==KSN_OK;
}

int32_t pocket_av_ui_current_player(void){return test_player_id;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){
    if(!out||!id||id!=test_player_id)return false;
    *out=test_player_ui;return true;
}
bool sys_device_clock_read(sys_clock_state *out){
    test_clock_reads++;
    if(!out||!test_clock_valid)return false;
    *out=test_clock_ui;return true;
}

/* Single-shot failures let QuickJS construct/catch its OOM exception. Track
 * every guest allocation so each fresh-runtime sweep also checks leaks. */
typedef union { max_align_t align; size_t size; } allocation_header;
static long fault_after=-1;
static unsigned fault_index;
static bool fault_hit,native_fault;
static size_t live_allocations;
static bool track_native;
static long native_after=-1;
static size_t native_bytes,native_max,native_calls;
static struct { void *ptr;size_t bytes; } native_blocks[16];

void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    for(unsigned i=0;i<16;i++) if(ptr&&native_blocks[i].ptr==ptr) {
        native_bytes-=native_blocks[i].bytes;native_blocks[i].ptr=NULL;break;
    }
    __real_free(ptr);
}
void *__wrap_calloc(size_t count,size_t size) {
    if(track_native) native_calls++;
    if(native_fault) { native_fault=false;return NULL; }
    if(track_native&&native_after>=0&&native_after--==0) return NULL;
    void *ptr=__real_calloc(count,size);
    if(track_native&&ptr) {
        unsigned i=0;while(i<16&&native_blocks[i].ptr)i++;
        if(i==16) abort();
        native_blocks[i].ptr=ptr;native_blocks[i].bytes=count*size;
        native_bytes+=count*size;if(count*size>native_max)native_max=count*size;
    }
    return ptr;
}
static bool allocation_fails(void) {
    if(fault_after<0) return false;
    if(fault_after--!=0) return false;
    fault_after=-1;fault_hit=true;return true;
}
static void *fault_malloc(void *opaque,size_t size) {
    (void)opaque;
    if(allocation_fails()||size>SIZE_MAX-sizeof(allocation_header)) return NULL;
    allocation_header *p=malloc(sizeof(*p)+size);
    if(!p) return NULL;
    p->size=size;live_allocations++;return p+1;
}
static void *fault_calloc(void *opaque,size_t count,size_t size) {
    if(size&&count>SIZE_MAX/size) return NULL;
    void *p=fault_malloc(opaque,count*size);
    if(p) memset(p,0,count*size);
    return p;
}
static void fault_free(void *opaque,void *ptr) {
    (void)opaque;
    if(ptr) { free((allocation_header *)ptr-1);live_allocations--; }
}
static void *fault_realloc(void *opaque,void *ptr,size_t size) {
    if(!ptr) return fault_malloc(opaque,size);
    if(!size) { fault_free(opaque,ptr);return NULL; }
    if(allocation_fails()||size>SIZE_MAX-sizeof(allocation_header)) return NULL;
    allocation_header *p=realloc((allocation_header *)ptr-1,sizeof(*p)+size);
    if(!p) return NULL;
    p->size=size;return p+1;
}
static size_t fault_size(const void *ptr) {
    return ptr?((const allocation_header *)ptr-1)->size:0;
}
static const JSMallocFunctions allocator={fault_calloc,fault_malloc,fault_free,
                                         fault_realloc,fault_size};
static JSValue arm_fault(JSContext *context,JSValueConst self,int argc,JSValueConst *argv) {
    (void)context;(void)self;(void)argc;(void)argv;
    fault_after=fault_index;return JS_UNDEFINED;
}
static JSValue disarm_fault(JSContext *context,JSValueConst self,int argc,JSValueConst *argv) {
    (void)context;(void)self;(void)argc;(void)argv;
    fault_after=-1;return JS_UNDEFINED;
}

static void check(bool ok,const char *what) {
    printf("%s %s\n",ok?"ok  ":"FAIL",what);
    if(!ok)failures++;
}
static bool run(const char *source) {
    JSValue value=JS_Eval(ctx,source,strlen(source),"<kasane-test>",JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(value);
    if(!ok) {
        JSValue error=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,error);
        printf("    threw: %s\n",text?text:"?");
        if(text)JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,error);
    }
    JS_FreeValue(ctx,value);return ok;
}
static uint16_t *get_strip(void *opaque) {(void)opaque;return strip_pixels;}
static ksn_result send_strip(void *opaque,uint16_t y,uint16_t rows,
                            const uint16_t *pixels) {
    (void)opaque;
    transfers++;
    memcpy(panel_pixels+y*240,pixels,rows*240*sizeof(*pixels));
    if(y/8==invalidate_band){invalidate_band=-1;pocket_kasane_invalidate();}
    if(fail_once||y/8==fail_band){fail_once=false;return KSN_IO;}
    return KSN_OK;
}
static ksn_result present(ksn_render_stats *stats) {
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                          .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    return pocket_kasane_present(&port,stats);
}

static void presenter_tests(void) {
    ksn_render_stats stats;bool blocked=false;
    check(run("globalThis.music=kasane.mount('music');"
              "globalThis.picture={title:'TRACK',status:'PLAYING  1s',positionMs:1000,"
              "durationMs:100000,playing:true,phase:0,help:false};"
              "music.update(picture);"),"native MUSIC presenter accepts a view model");
    check(pocket_kasane_active()&&pocket_kasane_has_submission(),
          "presenter submit activates the APP lease");
    check(run("picture.status='PLAYING  2s';picture.positionMs=2000;music.update(picture);"),
          "pending MUSIC update replaces latest plan without another transaction");
    check(present(&stats)==KSN_OK,"first MUSIC plan reaches display");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "acknowledging A submits newer B, not a false acknowledgement of B");
    check(present(&stats)==KSN_OK,"newer MUSIC plan reaches display");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "presenter becomes idle after latest plan is acknowledged");
    check(run("music.update(picture);"),"same MUSIC plan is accepted");
    check(!pocket_kasane_has_submission(),"equal plan does not submit a new bank");
    check(run("picture.help=true;music.update(picture);"),"help changes the native page");
    check(present(&stats)==KSN_OK,"help page presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "help acknowledgement is stable");
    check(run("let busy=false;try{kasane.replace(tx=>tx.background(0xff));}"
              "catch(e){busy=e.code==='BUSY'}if(!busy)throw Error('mixed owners');"),
          "direct replace cannot mix with a mounted presenter");
    pocket_kasane_reset();
    pocket_kasane_set_viewport(140,46,96,22);
    check(run("globalThis.clock=kasane.mount('clock');")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "a generic native source supports a small clock overlay viewport");
    check(run("(()=>{let stale=false;try{music.update(picture)}"
              "catch(e){stale=e.code==='CLOSED'}"
              "if(!stale)throw Error('old presenter controlled new lease')})()"),
          "a prior session presenter cannot control a newly mounted view");
    check(present(&stats)==KSN_OK,"clock presenter reaches display");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "clock presenter acknowledges its plan");
    pocket_kasane_reset();
}

static void app_presenter_tests(void){
    ksn_render_stats stats;bool blocked=false;
    char accounting[128];
    check(run("globalThis.helloView=kasane.mount('hello');helloView.set({counter:'KEY PRESSES: 0'});"),
          "hello mounts a native full-screen definition");
    check(present(&stats)==KSN_OK,"hello first frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "hello presenter acknowledges the frame");
    check(run("helloView.set({counter:'KEY PRESSES: 0'})")&&!pocket_kasane_has_submission(),
          "same hello counter skips a replacement");
    check(run("helloView.set({counter:'KEY PRESSES: 1'})")&&pocket_kasane_has_submission(),
          "hello counter update submits without a JS scene callback");
    check(present(&stats)==KSN_OK&&stats.bands!=0x1ffffu,
          "hello counter PATCH damages fewer than all 17 bands");
    pocket_kasane_reset();
    check(run("globalThis.imuView=kasane.mount('imucal');"
              "imuView.set({head:'START',live:'NO SAMPLE YET',stat:'WAIT',"
              "spin:'GYR OFF',foot:'HOLD STILL'});"),
          "IMU calibration mounts native text slots");
    check(present(&stats)==KSN_OK,"IMU first frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "IMU presenter acknowledges the frame");
    check(run("imuView.set({live:'X+100 Y+000 Z-100'})")&&pocket_kasane_has_submission(),
          "partial IMU slot update submits latest state");
    check(present(&stats)==KSN_OK&&stats.bands!=0x1ffffu,
          "IMU text PATCH keeps the rest of the screen clean");
    pocket_kasane_reset();
    check(run("globalThis.bridgeView=kasane.mount('bridge');"
              "bridgeView.set({st:'CONNECTING...',info:'',job:'',seen:''});"),
          "bridge mounts native text slots with empty optional fields");
    check(present(&stats)==KSN_OK,"bridge first frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "bridge presenter acknowledges the frame");
    check(run("bridgeView.set({info:'HOST READY'})")&&pocket_kasane_has_submission(),
          "bridge reveals an empty slot after an asynchronous event");
    pocket_kasane_reset();
    check(run("globalThis.companionArt=kasane.resource('pets');"
              "globalThis.companionView=kasane.mount('companion');"
              "companionView.set({resource:companionArt,variant:3,head:'< CODEX >',line0:'PC CONNECTED',"
              "foot:'UP/DOWN PET',hint:'ESC HOME'});"),
          "companion mounts a native pet image and text layout");
    check(present(&stats)==KSN_OK,"companion first frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "companion presenter acknowledges the frame");
    check(run("try{companionView.set({variant:12,head:'BAD'});throw Error('accepted')}"
              "catch(e){if(e.message==='accepted')throw e}"),
          "out-of-range image variant is rejected before changing any slot");
    check(run("companionView.set({variant:4})")&&pocket_kasane_has_submission(),
          "companion pet selection patches the registered image after invalid set");
    pocket_kasane_reset();
    check(run("globalThis.petArt=kasane.resource('pets');"),
          "pet image resource is registered before the presenter allocation");
    check(run("globalThis.petNativeBase=kasane.stats().nativeBytes;"),
          "native accounting baseline excludes an unmounted descriptor");
    native_max=0;
    track_native=true;
    check(run("globalThis.petView=kasane.mount('pet');petView.set({ready:false});"),
          "pet mounts without an image before storage is loaded");
    track_native=false;
    printf("pet static allocation: %zu bytes\n",native_max);
    check(native_max<=1280,"pet static presenter allocation stays within the prior 1280-byte budget");
    snprintf(accounting,sizeof(accounting),
             "if(kasane.stats().nativeBytes-petNativeBase!==%zu)throw Error('schema accounting')",
             native_max);
    check(run(accounting),
          "nativeBytes includes the pet schema, session, values, and text pool");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "pet loading model starts its initial transaction");
    check(present(&stats)==KSN_OK,"pet loading frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "pet loading frame acknowledges");
    check(run("try{petView.set({ready:true,resource:companionArt,title:'BAD'});"
              "throw Error('accepted')}catch(e){if(e.message==='accepted')throw e}"),
          "stale image handle is rejected before changing the pet model");
    check(run("petView.set({ready:true,resource:petArt,variant:8,frame:2,petY:31,"
              "bar0:80,bar1:60,bar2:40,title:'CHOOSE: GRAY',"
              "food:'FOOD 80',joy:'JOY 60',energy:'ENERGY 40'});")&&
          pocket_kasane_has_submission(),
          "pet domain values build a native image and three bars");
    check(present(&stats)==KSN_OK,"pet selected frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "pet selected frame acknowledges");
    check(run("petView.set({bubble:true,note:'YUM!',reveal:2})")&&
          pocket_kasane_has_submission(),
          "pet bubble reveal is owned by the presenter transaction");
    check(present(&stats)==KSN_OK,"pet bubble frame presents");
    pocket_kasane_reset();
    check(run("try{kasane.mount({version:1,slots:{x:{type:'u16'}},"
              "nodes:[{type:'rect',bounds:[0,0,20,20],color:0xffffffff,"
              "visible:{slot:'x'}}]});throw Error('accepted')}"
              "catch(e){if(e.message==='accepted')throw e}"),
          "runtime descriptor rejects a mismatched slot type before mounting");
    check(run("globalThis.runtimeNativeBase=kasane.stats().nativeBytes;"),
          "runtime descriptor starts from a known native accounting baseline");
    size_t runtime_before=native_bytes;
    track_native=true;
    check(run("globalThis.customView=kasane.mount({version:1,background:0x112233ff,"
              "slots:{label:{type:'text',capacity:24},fill:{type:'rect'},show:{type:'bool'}},"
              "nodes:[{type:'text',bounds:[8,8,220,24],text:{slot:'label'},color:0xe2f0ffff},"
              "{type:'rect',bounds:{slot:'fill'},visible:{slot:'show'},color:0x78c8ffff}]},"
              "{label:'EIGHTH APP',fill:[12,109,88,111],show:true});"),
          "an unknown eighth JS app compiles typed slots and nodes at mount");
    track_native=false;
    snprintf(accounting,sizeof(accounting),
             "if(kasane.stats().nativeBytes-runtimeNativeBase!==%zu)throw Error('runtime accounting')",
             native_bytes-runtime_before);
    check(run(accounting),
          "nativeBytes includes the runtime descriptor and mounted schema allocations");
    check(present(&stats)==KSN_OK,"runtime descriptor initial frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "runtime descriptor initial frame acknowledges");
    check(run("try{customView.set({label:'X'.repeat(100000)});throw Error('accepted')}"
              "catch(e){if(e.message==='accepted')throw e}"),
          "runtime text length is rejected before UTF-8 conversion");
    check(run("customView.set({label:'UPDATED'})")&&pocket_kasane_has_submission(),
          "runtime descriptor updates via the same exact PATCH path");
    check(present(&stats)==KSN_OK&&stats.bands!=0x1ffffu,
          "runtime descriptor PATCH avoids full-screen damage");
    pocket_kasane_reset();
    check(run("globalThis.offsetView=kasane.mount({version:1,backgroundSlot:'bg',"
              "slots:{bg:{type:'color'},extent:{type:'u16',initial:1,maximum:80}},"
              "nodes:[{type:'rect',bounds:[8,30,8,34],rectAdd:[null,null,'extent',null],"
              "color:0x78c8ffff}]},{bg:0x102030ff,extent:40});"),
          "runtime descriptor compiles dynamic background and numeric geometry");
    check(present(&stats)==KSN_OK,"runtime numeric geometry first frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "runtime numeric geometry frame acknowledges");
    check(run("offsetView.set({extent:41})")&&pocket_kasane_has_submission(),
          "runtime numeric geometry patches through the generic offset operator");
    pocket_kasane_reset();
    check(run("globalThis.wideView=kasane.mount({version:1,"
              "slots:{frames:{type:'u32',initial:3999999999,maximum:4000000000}},"
              "nodes:[{type:'rect',bounds:[0,0,8,8],color:0xffffffff}]});"),
          "runtime descriptor accepts full-width numeric source slots");
    check(run("wideView.set({frames:4000000000})"),
          "u32 base value preserves numbers above the u16 range");
    check(run("try{wideView.set({frames:4000000001});throw Error('accepted')}"
              "catch(e){if(e.message==='accepted')throw e}"),
          "u32 maximum rejects an excessive value without partial update");
    check(run("try{wideView.set({frames:4294967296});throw Error('accepted')}"
              "catch(e){if(e.message==='accepted')throw e}"),
          "u32 slot rejects values outside the 32-bit range");
    pocket_kasane_reset();
}

static void reactive_presenter_tests(void){
    ksn_render_stats stats;bool blocked=false;
    test_player_id=77;
    test_player_ui=(pocket_av_ui_snapshot){.state=POCKET_AV_UI_PLAYING,
        .position_ms=1000,.duration_ms=0,.underruns=0};
    pocket_kasane_set_animation_time(0);
    check(run("globalThis.live=kasane.mount('music');"
              "live.set({title:'TRACK',message:'OPENING'});live.bind('playback');"),
          "music binds a native playback snapshot without a JS frame model");
    check(present(&stats)==KSN_OK,"reactive initial MUSIC plan presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "initial reactive acknowledgement is stable");
    check(run("live.set({message:''})"),"app message reveals the playback summary");
    check(present(&stats)==KSN_OK,"playback summary presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "playback summary acknowledgement is stable");
    test_player_ui.position_ms=2000;
    pocket_kasane_set_animation_time(66667);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked&&
          pocket_kasane_has_submission(),
          "audio position changes the scene without a JS update");
    check(present(&stats)==KSN_OK,"native audio update presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "native audio update acknowledges");
    test_player_ui.underruns=42;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "diagnostic underruns do not redraw the music status");
    check(pocket_kasane_presenter_host_status("SYSTEM",6,200000)==KSN_OK,
          "host-owned status overrides the app and playback slots");
    check(present(&stats)==KSN_OK,"host status presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "host status acknowledges");
    check(run("live.set({message:'APP'})"),"app updates behind the host status");
    check(!pocket_kasane_has_submission(),"hidden app update does not submit");
    pocket_kasane_set_animation_time(200000);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "host expiry reveals the latest app message");
    check(present(&stats)==KSN_OK,"restored app message presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "restored app message acknowledges");
    check(run("(()=>{let rejected=false;try{live.set({message:'\\n'})}"
              "catch(e){rejected=e.code==='INVALID_ARGUMENT'}"
              "if(!rejected)throw Error('control accepted')})()"),
          "invalid text never enters the latest reactive plan");
    check(run("(()=>{let limited=false;try{live.set({title:'A'.repeat(257)})}"
              "catch(e){limited=e.code==='LIMIT_EXCEEDED'}"
              "if(!limited)throw Error('unbounded presenter input')})()"),
          "JS string conversion is bounded before UTF-8 allocation");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "invalid text leaves the native source step healthy");
    check(run("live.set({message:''})"),"app message clears without resetting playback binding");
    check(present(&stats)==KSN_OK,"cleared app message presents playback state");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "cleared message acknowledgement is stable");
    test_player_id=78;
    test_player_ui=(pocket_av_ui_snapshot){.state=POCKET_AV_UI_READY};
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "closed player ID cannot revive the old binding");
    check(present(&stats)==KSN_OK,"detached old player presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "detached player acknowledgement is stable");
    check(run("live.bind('playback')"),"a newly opened player explicitly rebinds");
    check(present(&stats)==KSN_OK,"new player binding presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "new player acknowledgement is stable");
    test_player_ui.duration_ms=500;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "subsecond known duration changes the visible status suffix");
    check(present(&stats)==KSN_OK,"subsecond duration presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "subsecond duration acknowledgement is stable");
    test_player_ui.duration_ms=0;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "removing the subsecond duration restores unknown-length status");
    check(present(&stats)==KSN_OK,"unknown duration presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "unknown duration acknowledgement is stable");
    test_player_ui.state=POCKET_AV_UI_PLAYING;test_player_ui.position_ms=3000;
    pocket_kasane_set_animation_time(266668);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "source A submits from native state");
    test_player_ui.position_ms=4000;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "source B coalesces while A is pending");
    check(present(&stats)==KSN_OK,"source A presents first");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "acknowledging A submits the latest native source B");
    check(present(&stats)==KSN_OK,"source B presents second");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "source B acknowledgement is stable");
    test_player_ui.duration_ms=100000;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "known duration changes the visible status and bar");
    check(present(&stats)==KSN_OK,"known duration presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "known duration acknowledgement is stable");
    test_player_ui.position_ms=4001;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "subpixel progress and same displayed second skip plan submission");
    check(run("live.toggleHelp()"),"reactive help opens");
    check(present(&stats)==KSN_OK,"reactive help presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "reactive help acknowledgement is stable");
    check(run("live.set({title:'HIDDEN TITLE',message:'HIDDEN MESSAGE'})"),
          "app slots update behind help");
    check(!pocket_kasane_has_submission(),
          "hidden app slots do not redraw the help page");
    check(run("live.dismissHelp()"),"reactive help closes");
    check(present(&stats)==KSN_OK,"latest app slots appear after help closes");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "revealed app slots acknowledgement is stable");
    check(run("live.set({title:'HIDDEN TITLE'})")&&
          !pocket_kasane_has_submission(),
          "setting an unchanged app slot does not advance its visible revision");
    pocket_kasane_reset();test_player_id=0;

    test_clock_valid=false;
    pocket_kasane_set_viewport(140,46,96,22);
    check(run("globalThis.liveClock=kasane.mount('clock');")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "clock asset supplies the registered wall-clock source");
    check(present(&stats)==KSN_OK,"unsynced native clock presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "unsynced clock acknowledgement is stable");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("liveClock.set({face:'BASE',tag:'BASE'})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission()&&
          !memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels)),
          "native clock overrides JS base without a redundant frame");
    test_clock_valid=true;
    test_clock_ui=(sys_clock_state){.seconds=45240,.source=SYS_CLOCK_SNTP};
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "clock source updates without a JS update");
    test_clock_ui.seconds=45300;
    unsigned reads_before_pending=test_clock_reads;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked&&
          pocket_kasane_has_submission()&&test_clock_reads==reads_before_pending,
          "pending ticket defers producer acquisition and coalesces the new minute");
    check(present(&stats)==KSN_OK,"first synced native clock presents");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "latest native minute submits after the old ticket presents");
    check(present(&stats)==KSN_OK&&
          memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels))!=0,
          "coalesced native minute changes the displayed clock");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "synced clock acknowledgement is stable");
    test_clock_ui.seconds=45359;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "same displayed minute makes no native submission");
    test_clock_ui.seconds=45420;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "new minute makes one native submission");
    check(present(&stats)==KSN_OK,"new native minute presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "new native minute acknowledgement is stable");
    pocket_kasane_reset();test_clock_valid=false;
}

#ifdef KSN_TEST_DUAL_SOURCE
static void dual_source_mount_tests(void){
    ksn_render_stats stats;
    bool blocked=false;
    unsigned acquired=0,released=0,reads_before=0;
    pocket_kasane_set_viewport(140,46,96,22);
    check(run("globalThis.dual=kasane.mount('dual-test')")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "two native sources compose in a mounted asset");
    check(present(&stats)==KSN_OK,"dual-source initial frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "both source revisions acknowledge after presentation");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    pocket_test_dual_set(0,"A1");pocket_test_dual_set(1,"B1");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "two source changes enter one submission");
    check(run("(()=>{let busy=false;try{dual.bind(0,{alt:0})}catch(e){busy=true}"
              "if(!busy)throw Error('bind while pending')})()"),
          "bind rejects a mapping change during an in-flight frame");
    pocket_test_dual_counts(&reads_before,NULL);
    pocket_test_dual_set(0,"A2");pocket_test_dual_set(1,"B2");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "pending dual-source frame defers new acquisition");
    pocket_test_dual_counts(&acquired,NULL);
    check(acquired==reads_before,"pending dual-source frame does not read producers");
    check(present(&stats)==KSN_OK,"first dual-source update presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "latest values coalesce after first presentation");
    check(present(&stats)==KSN_OK&&
          memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels))!=0,
          "coalesced dual-source values change displayed pixels");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "coalesced dual-source update acknowledges");
    pocket_test_dual_set(0,"A3");pocket_test_dual_set(1,"B3");
    pocket_test_dual_fail_second(true);
    check(pocket_kasane_presenter_step(&blocked)==KSN_IO&&
          !pocket_kasane_has_submission(),
          "second-source failure prevents partial submission");
    pocket_test_dual_counts(&acquired,&released);
    check(acquired==released,"failed composition releases first source pin");
    pocket_test_dual_fail_second(false);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "dual-source mount recovers after producer failure");
    check(present(&stats)==KSN_OK,"recovered dual-source frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "recovered dual-source frame acknowledges");
    pocket_test_dual_counts(&acquired,&released);
    check(acquired==released,"all dual-source leases released");
    check(run("dual.set({left:'BASE'})")&&!pocket_kasane_has_submission(),
          "native left value overrides the JS base without redrawing");
    pocket_test_dual_expire(0,1000);
    pocket_kasane_set_animation_time(999);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "source expiry metadata alone does not redraw early");
    check(pocket_kasane_source_wait_ticks(999,10,1000)==1,
          "native source deadline bounds owner wait to one tick");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    pocket_kasane_set_animation_time(1000);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "native deadline reveals JS base without a JS frame");
    check(pocket_kasane_source_wait_ticks(1000,10,1000)==10,
          "due source cannot cause a zero-tick wait spin");
    check(present(&stats)==KSN_OK&&
          memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels))!=0,
          "expired source changes visible left text");
    bool right_stable=true;
    for(unsigned row=46;row<62;row++)
        if(memcmp(committed_pixels+row*240+188,panel_pixels+row*240+188,
                  48*sizeof(uint16_t))!=0)right_stable=false;
    check(right_stable,"expiry leaves the other native source unchanged");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "expired source frame acknowledges without repeated redraw");
    check(run("dual.set({left:'NEXT'})")&&pocket_kasane_has_submission(),
          "JS base remains writable while native source has expired");
    check(present(&stats)==KSN_OK,"updated base presents while source expired");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "updated base acknowledges while source expired");
    pocket_test_dual_expire(0,0);
    pocket_kasane_set_animation_time(1001);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "renewed native source replaces latest JS base automatically");
    check(present(&stats)==KSN_OK,"renewed native value presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "renewed native value acknowledges");
    check(run("(()=>{let overlap=false;try{dual.bind(0,{right:0})}"
              "catch(e){overlap=true}if(!overlap)throw Error('overlap')})()"),
          "public bind rejects a slot already owned by another source");
    check(run("(()=>{let wrong=false;try{dual.bind(0,{alt:1})}"
              "catch(e){wrong=true}if(!wrong)throw Error('field type')})()"),
          "public bind validates the source field index");
    check(run("(()=>{let wrong=false;try{dual.bind(0,{tint:0})}"
              "catch(e){wrong=true}if(!wrong)throw Error('slot type')})()"),
          "public bind rejects a field-to-slot type mismatch");
    pocket_test_dual_deny(0,true);
    check(run("(()=>{let denied=false;try{dual.bind(0,{alt:0})}"
              "catch(e){denied=true}if(!denied)throw Error('source denied')})()"),
          "public bind enforces producer authorization");
    pocket_test_dual_deny(0,false);
    check(run("(()=>{let busy=false;try{dual.bind(0,{get alt(){"
              "dual.set({alt:'JS'});return 0}})}catch(e){busy=true}"
              "if(!busy)throw Error('getter submitted')})()")&&
          pocket_kasane_has_submission(),
          "public bind rechecks pending work after a binding getter");
    check(present(&stats)==KSN_OK,"binding getter submission presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "binding getter submission acknowledges");
    check(run("dual.bind(0,{left:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "reentrant rejected bind leaves the old mapping intact");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("dual.bind(0,{alt:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "public bind remaps a mount-owned source without JS frame state");
    check(present(&stats)==KSN_OK&&
          memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels))!=0,
          "rebinding restores old base slot and fills new source slot");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "rebound source acknowledges its new subscription");
    check(run("dual.bind(0,{alt:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "identical public bind does not redraw");
    check(external_test_open(0,"C0"),"third service source registers for mixed mount");
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue mixed=pocket_kasane_source_capability(ctx,&external_test[0].registry,
                                                   external_test[0].handle);
    check(!JS_IsException(mixed)&&
          JS_SetPropertyStr(ctx,global,"mixedCap",mixed)>=0,
          "mixed mount receives a separate service capability");
    JS_FreeValue(ctx,global);
    check(run("dual.bind(mixedCap,{left:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "two mount-owned and one service source compose in one frame");
    check(present(&stats)==KSN_OK,"mixed three-source frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "mixed three-source frame acknowledges");
    check(external_test[0].acquired==external_test[0].released,
          "mixed-source lease does not outlive its owner turn");
    check(run("dual.unbind(0)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "unbind drops one static source but keeps the other two");
    check(present(&stats)==KSN_OK,"static unbind restores alt base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "static unbind frame acknowledges");
    check(run("dual.bind(0,{alt:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "static source can rebind after unbind");
    check(present(&stats)==KSN_OK,"rebound static source presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "rebound static source acknowledges");
    check(run("dual.unbind(mixedCap)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "external unbind keeps both static subscriptions");
    check(present(&stats)==KSN_OK,"external unbind restores left base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "external unbind frame acknowledges");
    pocket_kasane_reset();
    check(pocket_kasane_source_wait_ticks(1001,10,1000)==10,
          "unmounted source does not shorten owner waits");
    check(run("(()=>{let stale=false;try{dual.bind(0,{alt:0})}"
              "catch(e){stale=true}if(!stale)throw Error('stale bind')})()"),
          "old mount cannot rebind a source after reset");
}
#endif

static void external_source_mount_tests(void){
    ksn_render_stats stats;
    bool blocked=false;
    check(external_test_open(0,"A0")&&external_test_open(1,"B0"),
          "two service-owned source registries register independently");
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue first=pocket_kasane_source_capability(ctx,&external_test[0].registry,
                                                   external_test[0].handle);
    JSValue second=pocket_kasane_source_capability(ctx,&external_test[1].registry,
                                                    external_test[1].handle);
    check(!JS_IsException(first)&&!JS_IsException(second)&&
          JS_SetPropertyStr(ctx,global,"externalCap0",first)>=0&&
          JS_SetPropertyStr(ctx,global,"externalCap1",second)>=0,
          "C services publish opaque session-scoped capabilities");
    JS_FreeValue(ctx,global);
    pocket_kasane_set_viewport(140,46,96,22);
    check(run("globalThis.externalView=kasane.mount({version:1,"
              "slots:{label:{type:'text',capacity:7},right:{type:'text',capacity:7}},"
              "nodes:[{type:'text',bounds:[0,0,44,16],text:{slot:'label'},"
              "color:0xffffffff},{type:'text',bounds:[48,0,95,16],"
              "text:{slot:'right'},color:0xffffffff}]},"
              "{label:'BASE',right:'BASE'})"),
          "runtime descriptor mounts without built-in source state");
    check(present(&stats)==KSN_OK,"runtime base frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "runtime base frame acknowledges");
    check(run("globalThis.externalNativeBefore=kasane.stats().nativeBytes"),
          "external view records allocation before first bind");
    check(run("(()=>{let forged=false;try{externalView.bind({}, {label:0})}"
              "catch(e){forged=true}if(!forged)throw Error('forged source')})()"),
          "plain JS objects cannot forge source capabilities");
    external_test[0].allowed=false;
    check(run("(()=>{let denied=false;try{externalView.bind(externalCap0,{label:0})}"
              "catch(e){denied=true}if(!denied)throw Error('not authorized')})()"),
          "external bind enforces service authorization");
    external_test[0].allowed=true;
    check(run("globalThis.externalBindings={label:0};"
              "globalThis.externalBindExercise=()=>externalView.bind("
              "externalCap0,externalBindings)"),
          "external bind OOM fixture is compiled before injection");
    global=JS_GetGlobalObject(ctx);
    JSValue exercise=JS_GetPropertyStr(ctx,global,"externalBindExercise");
    JS_FreeValue(ctx,global);
    native_fault=true;
    JSValue failed=JS_Call(ctx,exercise,JS_UNDEFINED,0,NULL);
    check(!native_fault&&JS_IsException(failed)&&
          !pocket_kasane_has_submission(),
          "external bind allocation failure leaves mount unchanged");
    JSValue error=JS_GetException(ctx);
    JS_FreeValue(ctx,error);JS_FreeValue(ctx,failed);JS_FreeValue(ctx,exercise);
    check(run("externalView.bind(externalCap0,{label:0});"
              "externalView.bind(externalCap1,{right:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "runtime view composes two external source registries");
    check(run("(()=>{let busy=false;try{externalView.unbind(externalCap0)}"
              "catch(e){busy=true}if(!busy)throw Error('pending unbind')})()"),
          "unbind rejects a subscription change while a frame is pending");
    check(run("(()=>{let added=kasane.stats().nativeBytes-externalNativeBefore;"
              "if(added<=0||added>1024)throw Error('external allocation')})()"),
          "external subscription uses one bounded lazy allocation");
    check(present(&stats)==KSN_OK,"external source frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "both external source revisions acknowledge");
    memcpy(external_test[0].text,"A1",3);external_test[0].revision++;
    memcpy(external_test[1].text,"B1",3);external_test[1].revision++;
    external_test[1].fail=true;
    check(pocket_kasane_presenter_step(&blocked)==KSN_IO&&
          !pocket_kasane_has_submission()&&
          external_test[0].acquired==external_test[0].released,
          "later external failure releases earlier registry pin atomically");
    external_test[1].fail=false;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "external subscriptions recover with latest complete values");
    check(present(&stats)==KSN_OK,"recovered external frame presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "recovered external frame acknowledges");
    check(run("externalView.set({label:'LATEST'})")&&
          !pocket_kasane_has_submission(),
          "native external value overrides JS base without redraw");
    external_test[0].expires_at_us=2000;external_test[0].revision++;
    pocket_kasane_set_animation_time(1999);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          pocket_kasane_source_wait_ticks(1999,10,1000)==1,
          "external source deadline shortens owner wait without early redraw");
    pocket_kasane_set_animation_time(2000);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "expired external capability reveals latest JS base");
    check(present(&stats)==KSN_OK,"expired external source presents base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "expired external source acknowledges");
    external_test[0].expires_at_us=0;external_test[0].revision++;
    pocket_kasane_set_animation_time(2001);
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "renewed external source overrides base again");
    check(present(&stats)==KSN_OK,"renewed external source presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "renewed external source acknowledges");
    check(external_test[0].acquired==external_test[0].released&&
          external_test[1].acquired==external_test[1].released,
          "external leases are released after every owner step");
    check(run("externalView.unbind(externalCap0)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "explicit unbind restores latest base with second source retained");
    check(present(&stats)==KSN_OK,"first external unbind presents base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "first external unbind acknowledges");
    check(run("externalView.unbind(externalCap1)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "last external unbind falls back to source-free schema path");
    check(present(&stats)==KSN_OK,"last external unbind presents base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "last external unbind acknowledges");
    check(run("if(kasane.stats().nativeBytes!==externalNativeBefore)"
              "throw Error('external allocation retained')"),
          "last unbind releases the optional subscription allocation");
    check(run("externalView.unbind(externalCap1)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "external unbind is idempotent");
    check(run("externalView.bind(externalCap0,{label:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "external source rebinds after full detach");
    check(present(&stats)==KSN_OK,"rebound external source presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "rebound external source acknowledges");
    check(run("externalView.bind(externalCap1,{right:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "second external source rebinds before service revocation");
    check(present(&stats)==KSN_OK,"second external source presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "second external source acknowledges");
    memcpy(external_test[0].text,"A2",3);external_test[0].revision++;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "source update submits before service revocation");
    check(ksn_source_unregister(&external_test[0].registry,
                                external_test[0].handle)==KSN_OK,
          "service can unregister after the owner releases its source pin");
    unsigned reads_before_revoke=external_test[0].acquired;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked&&
          external_test[0].acquired==reads_before_revoke,
          "pending submitted frame does not reacquire a revoked service");
    check(present(&stats)==KSN_OK,
          "submitted core-owned pixels present after source revocation");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "stale service handle automatically reveals latest JS base");
    check(present(&stats)==KSN_OK,"automatic stale-source detach presents base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "automatic stale-source detach acknowledges");
    check(run("if(kasane.stats().nativeBytes<=externalNativeBefore)"
              "throw Error('other subscription dropped')"),
          "automatic detach preserves the other external subscription");
    check(run("externalView.unbind(externalCap1)")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "remaining external source can detach after stale peer");
    check(present(&stats)==KSN_OK,"remaining external unbind presents base");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "remaining external unbind acknowledges");
    check(run("if(kasane.stats().nativeBytes!==externalNativeBefore)"
              "throw Error('stale subscription retained')"),
          "last detach frees the external subscription allocation");
    pocket_kasane_reset();
    check(external_test_open(0,"N0"),
          "service registers a new generation after prior handle removal");
    global=JS_GetGlobalObject(ctx);
    JSValue fresh=pocket_kasane_source_capability(ctx,&external_test[0].registry,
                                                   external_test[0].handle);
    check(!JS_IsException(fresh)&&
          JS_SetPropertyStr(ctx,global,"externalCapFresh",fresh)>=0,
          "same service can issue a new-session capability");
    JS_FreeValue(ctx,global);
    check(run("globalThis.externalView2=kasane.mount({version:1,"
              "slots:{label:{type:'text',capacity:7}},"
              "nodes:[{type:'text',bounds:[0,0,95,16],text:{slot:'label'},"
              "color:0xffffffff}]})"),
          "new runtime descriptor mounts after reset");
    check(run("(()=>{let stale=false;try{externalView2.bind(externalCap0,{label:0})}"
              "catch(e){stale=true}if(!stale)throw Error('old capability')})()"),
          "old source capability cannot cross guest reset");
    check(run("externalView2.bind(externalCapFresh,{label:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "fresh capability subscribes the new runtime view");
    check(present(&stats)==KSN_OK,"fresh external capability presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "fresh external capability acknowledges");
    bool recycled=true;
    for(unsigned i=0;i<6;i++){
        if(ksn_source_unregister(&external_test[0].registry,
                                 external_test[0].handle)!=KSN_OK||
           ksn_source_register(&external_test[0].registry,
                               &external_test[0].provider,
                               &external_test[0].handle)!=KSN_OK){recycled=false;break;}
        JSValue cap=pocket_kasane_source_capability(ctx,&external_test[0].registry,
                                                    external_test[0].handle);
        if(JS_IsException(cap))recycled=false;
        JS_FreeValue(ctx,cap);
        if(!recycled)break;
    }
    check(recycled,"stale capability records recycle across source generations");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "generation replacement detaches the old subscription");
    check(present(&stats)==KSN_OK,"generation replacement presents base");
    pocket_kasane_reset();
}

static void wall_source_service_tests(void){
    ksn_render_stats stats;
    bool blocked=false;
    test_clock_valid=false;
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue cap=pocket_clock_wall_source(ctx,JS_UNDEFINED,0,NULL);
    check(!JS_IsException(cap)&&JS_SetPropertyStr(ctx,global,"wallCap",cap)>=0,
          "wall-clock service issues a real native source capability");
    JS_FreeValue(ctx,global);
    check(run("globalThis.wallView=kasane.mount({version:1,"
              "slots:{face:{type:'text',capacity:5},tag:{type:'text',capacity:7}},"
              "nodes:[{type:'text',bounds:[0,0,48,12],text:{slot:'face'},"
              "color:0xffffffff},{type:'text',bounds:[50,0,96,12],"
              "text:{slot:'tag'},color:0xffffffff}]},"
              "{face:'BASE',tag:'BASE'})")&&
          present(&stats)==KSN_OK&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "arbitrary app schema mounts before a service bind");
    check(run("wallView.bind(wallCap,{face:0,tag:1})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked,
          "arbitrary app schema subscribes to the clock service without JS polling");
    check(present(&stats)==KSN_OK,"unsynced service clock presents");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "unsynced service clock acknowledges");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    test_clock_valid=true;
    test_clock_ui=(sys_clock_state){.seconds=45240,.source=SYS_CLOCK_SNTP};
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked&&
          present(&stats)==KSN_OK&&
          memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels))!=0,
          "clock service updates native text when the minute changes");
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "clock service acknowledgement is stable");
    test_clock_ui.seconds=45259;
    check(pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked&&
          !pocket_kasane_has_submission(),
          "unchanged minute does not redraw a subscribed app");
    check(pocket_kasane_reset(),"clock service subscriber detaches at session reset");
    pocket_clock_reset();
    global=JS_GetGlobalObject(ctx);
    JSValue next=pocket_clock_wall_source(ctx,JS_UNDEFINED,0,NULL);
    check(!JS_IsException(next)&&JS_SetPropertyStr(ctx,global,"wallCapNext",next)>=0,
          "clock service can issue a fresh capability after reset");
    JS_FreeValue(ctx,global);
    check(run("globalThis.wallView2=kasane.mount({version:1,"
              "slots:{face:{type:'text',capacity:5}},"
              "nodes:[{type:'text',bounds:[0,0,48,12],text:{slot:'face'},"
              "color:0xffffffff}]},{face:'BASE'})")&&
          present(&stats)==KSN_OK&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&!blocked,
          "fresh session mounts after clock service reset");
    check(run("(()=>{let stale=false;try{wallView2.bind(wallCap,{face:0})}"
              "catch(e){stale=true}if(!stale)throw Error('old clock capability')})()"),
          "old clock capability cannot bind to a new service generation");
    check(run("wallView2.bind(wallCapNext,{face:0})")&&
          pocket_kasane_presenter_step(&blocked)==KSN_OK&&blocked&&
          present(&stats)==KSN_OK,
          "fresh clock capability binds and presents after reset");
    check(pocket_kasane_reset(),"fresh clock service subscriber detaches");
    pocket_clock_reset();
    test_clock_valid=false;
}

static void atomicity_tests(void) {
    ksn_render_stats stats;
    /* Keep an old JS draw wrapper alive while its native arena is replaced. */
    check(run("globalThis.beforeReset=newRef"),"retain an old-session reference");
    pocket_kasane_reset();
    check(run("globalThis.shape={bounds:[0,0,10,10],color:0xff0000ff};"
              "globalThis.tpl=kasane.cache.create([shape]);"
              "kasane.replace(tx=>{globalThis.expiredTx=tx;globalThis.expiredModal=tx.modal;"
              "tx.background(0x000000ff);globalThis.baseRef=tx.rect(shape);"
              "globalThis.baseInst=tx.instantiate(tpl);});"),"atomicity baseline builds");
    check(present(&stats)==KSN_OK,"atomicity baseline presents");
    check(run("globalThis.expectAbort=(action,expected)=>{"
              "let inner=false,outer=false;try{kasane.replace(tx=>{"
              "tx.background(0xffffffff);globalThis.candidate=tx.rect(shape);"
              "tx.instantiate(tpl);try{action(tx,candidate)}catch(e){"
              "inner=e!==undefined&&(!expected||e===expected);}"
              "});}catch(e){outer=true}"
              "if(!inner||!outer)throw Error('caught failure submitted');"
              "let s=kasane.stats();if(s.displayed.commands!==2||s.cache.instances!==1||"
              "s.cache.templates!==1||s.cache.commands!==1)throw Error('quota leaked');};"
              "globalThis.badActions=["
              "tx=>tx.background(),tx=>tx.background(NaN),tx=>tx.rect(),"
              "tx=>tx.rect({bounds:[0,0,1],color:1}),"
              "tx=>tx.rect({bounds:[0,0,1,1],color:-1}),"
              "tx=>tx.rect({bounds:[0,0,1,1],color:1,opacity:256}),"
              "tx=>tx.group(),(tx,r)=>tx.group(r,0,255),tx=>tx.group({},1,255),"
              "tx=>tx.instantiate({}),tx=>tx.instantiate(tpl,{offset:[1]}),"
              "tx=>tx.instantiate(tpl,{visible:1}),(tx,r)=>r.setRect(tx),"
              "(tx,r)=>r.setClip(tx,[2,0,1,1]),(tx,r)=>r.setColor(tx),"
              "(tx,r)=>r.setVisible(tx,1),tx=>beforeReset.setColor(tx,1),"
              "tx=>baseRef.setColor.call({},tx,1),tx=>baseInst.place(tx,{offset:[0,NaN]}),"
              "tx=>baseInst.place.call({},tx),tx=>baseInst.setVisible(tx,1),"
              "tx=>tx.modal.open(),tx=>tx.modal.open({backdrop:'bad'}),"
              "tx=>tx.modal.open({color:-1}),tx=>tx.modal.open({focus:-1})];"
              "for(const action of badActions)expectAbort(action);"),
          "caught validation and stale draw/instance errors abort all changes and recover quotas");
    check(!pocket_kasane_has_submission(),"caught errors leave no submitted state");
    check(run("globalThis.marker=Error('getter marker');"
              "globalThis.trap=(key,base={})=>Object.defineProperty(base,key,{get(){throw marker}});"
              "for(const key of ['bounds','clip','opacity','color'])"
              "expectAbort(tx=>tx.rect(trap(key,{...shape})),marker);"
              "for(let i=0;i<4;i++)expectAbort(tx=>tx.rect({"
              "bounds:trap(i,[0,0,10,10]),color:1}),marker);"
              "for(const key of ['offset','clip','opacity','visible']){"
              "expectAbort(tx=>tx.instantiate(tpl,trap(key)),marker);"
              "expectAbort(tx=>baseInst.place(tx,trap(key)),marker);}"
              "for(let i=0;i<2;i++)expectAbort(tx=>tx.instantiate(tpl,{"
              "offset:trap(i,[0,0])}),marker);"
              "for(const key of ['backdrop','color','focus'])"
              "expectAbort(tx=>tx.modal.open(trap(key)),marker);"
              "expectAbort((tx,r)=>r.setRect(tx,trap(0,[0,0,10,10])),marker);"
              "let threw=false;try{kasane.replace(tx=>{tx.background(0xffffffff);"
              "tx.instantiate(tpl);return trap('then')})}catch(e){threw=e===marker}"
              "if(!threw||kasane.stats().cache.instances!==1)throw Error('then getter');"),
          "throwing getters retain the original exception and roll back candidates");
    check(run("globalThis.nestedBusy=false;expectAbort(tx=>{try{tx.rect()}catch(e){}"
              "try{kasane.replace(inner=>inner.background(0xffffffff))}"
              "catch(e){nestedBusy=e.code==='BUSY'}tx.background(0x000000ff);});"
              "if(!nestedBusy)throw Error('reentrant build');"
              "kasane.patch(tx=>{baseRef.setColor(tx,0x00ff00ff);"
              "for(const action of [()=>expiredTx.background(0xffffffff),"
              "()=>expiredModal.close(),()=>baseRef.setColor(expiredTx,1),"
              "()=>baseInst.setVisible({},true)]){let closed=false;try{action()}"
              "catch(e){closed=e.code==='CLOSED'}if(!closed)throw Error('foreign tx');}});"),
          "aborted callbacks cannot reenter a build; foreign transaction tokens preserve the owner");
    check(present(&stats)==KSN_OK,"valid owner still presents after foreign token failures");
    check(run("globalThis.deadRefs=[];for(let n=0;n<100;n++){"
              "let failed=false;try{kasane.replace(tx=>{tx.background(0x000000ff);"
              "for(let i=0;i<32;i++)deadRefs.push(tx.rect(shape));"
              "try{tx.rect()}catch(e){};});}catch(e){failed=true}"
              "if(!failed)throw Error('accepted');}"
              "kasane.replace(tx=>{tx.background(0x000000ff);"
              "for(let i=0;i<32;i++)tx.rect(shape);});"),
          "100 aborted full-reference builds reclaim native slots while dead wrappers survive");
    check(present(&stats)==KSN_OK,"full reference quota remains usable after repeated aborts");
    check(run("Object.defineProperty(Object.prototype,'modal',{configurable:true,"
              "set(){throw Error('prototype setter')}});"
              "try{kasane.patch(()=>{});kasane.cancel(kasane.poll().ticket)}"
              "finally{delete Object.prototype.modal;}"
              "Object.defineProperty(Object.prototype,'status',{configurable:true,"
              "set(){throw Error('prototype setter')}});"
              "try{if(kasane.poll().status!=='DISCARDED')throw Error('own status')}"
              "finally{delete Object.prototype.status;}"),
          "adapter properties do not invoke inherited setters");
}

static void repair_tests(void) {
    pocket_kasane_reset();pocket_kasane_invalidate();
    check(!pocket_kasane_needs_present()&&!pocket_kasane_active(),
          "invalidation before APP ownership does not allocate or paint");
    check(run("globalThis.tpl=kasane.cache.create([shape]);"
              "kasane.replace(tx=>{tx.background(0x102030ff);"
              "globalThis.baseRef=tx.rect(shape);globalThis.baseInst=tx.instantiate(tpl);"
              "tx.modal.open({backdrop:'dim-live',color:0x00000080,focus:7});});"),
          "repair baseline builds cached content and modal");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK,"repair baseline presents");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    bool all_bands=true;
    for(unsigned band=0;band<17;band++) {
        if(!run("globalThis.ticket=kasane.replace(tx=>{tx.background(0xff00ffff);"
                "tx.modal.close();globalThis.candidate=tx.rect(shape);tx.instantiate(tpl);});")) {
            all_bands=false;break;
        }
        fail_band=(int)band;transfers=0;
        if(present(&stats)!=KSN_IO||transfers!=band+1||!pocket_kasane_has_submission())
            all_bands=false;
        if(!run("kasane.cancel(ticket);if(kasane.poll().status!=='DISCARDED'||"
                "kasane.poll().reason!=='CANCELLED')throw Error('cancel');")) all_bands=false;
        if(pocket_kasane_has_submission()||!pocket_kasane_needs_present()||
           pocket_kasane_input_scope(false)!=KSN_INPUT_BLOCKED) all_bands=false;
        /* No JS callback/update from cancel through failed repair and retry. */
        transfers=0;
        if(present(&stats)!=KSN_IO||transfers!=band+1||pocket_kasane_has_submission())
            all_bands=false;
        fail_band=-1;transfers=0;
        if(present(&stats)!=KSN_OK||transfers!=17||stats.transferred_bytes!=64800||
           memcmp(panel_pixels,committed_pixels,sizeof(panel_pixels))||
           pocket_kasane_needs_present()||pocket_kasane_input_scope(false)!=KSN_INPUT_MODAL)
            all_bands=false;
        if(!run("if(kasane.poll().status!=='DISCARDED'||kasane.poll().reason!=='CANCELLED')"
                "throw Error('repair changed poll');var s=kasane.stats();"
                "if(s.displayed.commands!==3||s.cache.templates!==1||s.cache.instances!==1)"
                "throw Error('repair changed quota');"
                "kasane.patch(tx=>{baseRef.setColor(tx,0x00ff00ff);baseInst.place(tx,{offset:[4,4]})});"
                "kasane.cancel(kasane.poll().ticket);")) all_bands=false;
        if(!all_bands)break;
    }
    fail_band=-1;
    check(all_bands,"all 17 failed bands cancel and repair without JS; old refs/cache/modal/poll survive");
    /* Static apps still repaint external screen damage and indicator changes. */
    memset(panel_pixels,0x5a,sizeof(panel_pixels));pocket_kasane_invalidate();transfers=0;
    check(!pocket_kasane_has_submission()&&pocket_kasane_needs_present()&&
          present(&stats)==KSN_OK&&transfers==17&&
          memcmp(panel_pixels,committed_pixels,sizeof(panel_pixels))==0,
          "host invalidation restores a static committed screen without JS");
    pocket_kasane_invalidate();invalidate_band=16;transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&pocket_kasane_needs_present(),
          "invalidation during final transfer survives current acknowledgement");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&!pocket_kasane_needs_present(),
          "deferred invalidation triggers exactly one more full redraw");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers==0&&stats.bands==0&&stats.transferred_bytes==0,
          "idle present returns empty stats after repair");
}

static void close_fault_runtime(void) {
    fault_after=-1;pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    ctx=NULL;rt=NULL;
}

static bool open_fault_runtime(const char *exercise) {
    rt=JS_NewRuntime2(&allocator,NULL);ctx=rt?JS_NewContext(rt):NULL;
    if(!ctx) return false;
    host_capabilities_clear();
    if(pocket_kasane_install(ctx,NULL)!=ESP_OK) return false;
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"armFault",JS_NewCFunction(ctx,arm_fault,"armFault",0));
    JS_SetPropertyStr(ctx,global,"disarmFault",JS_NewCFunction(ctx,disarm_fault,"disarmFault",0));
    JS_FreeValue(ctx,global);
    /* Materialize QuickJS's lazy namespace functions before faulting adapter
     * work; a failed engine-level lazy property read occurs before C entry. */
    if(!run("kasane.features();kasane.stats();kasane.poll();"
            "globalThis.shape={bounds:[0,0,10,10],color:0xff0000ff};"
            "globalThis.placement={offset:[1,1]};"
            "globalThis.tpl=kasane.cache.create([shape]);"
            "kasane.replace(tx=>{tx.background(0x000000ff);"
            "globalThis.baseRef=tx.rect(shape);globalThis.baseInst=tx.instantiate(tpl)});"))
        return false;
    ksn_render_stats stats;
    return present(&stats)==KSN_OK&&run(exercise);
}

static void fault_sweep(const char *label,const char *exercise,bool arm_inside) {
    unsigned injected=0,recovered=0;bool passed=true,finished=false;
    for(fault_index=0;fault_index<256;fault_index++) {
        if(!open_fault_runtime(exercise)) { passed=false;close_fault_runtime();break; }
        JSValue global=JS_GetGlobalObject(ctx);
        JSValue function=JS_GetPropertyStr(ctx,global,"exercise");
        JS_FreeValue(ctx,global);
        fault_hit=false;fault_after=arm_inside?-1:(long)fault_index;
        JSValue result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
        fault_after=-1;
        if(fault_hit&&!JS_IsException(result)&&!JS_HasException(ctx)){
            /* QuickJS's optional shape-hash resize may fail without failing
             * object creation. A complete successful operation is valid. */
            injected++;recovered++;
            ksn_render_stats stats;
            if(pocket_kasane_has_submission()&&present(&stats)!=KSN_OK)passed=false;
        } else if(fault_hit) {
            injected++;
            if(!JS_IsException(result)||!JS_HasException(ctx)||pocket_kasane_has_submission()){
                printf("    fault index=%u exception=%d pendingException=%d submission=%d\n",fault_index,
                       JS_IsException(result),JS_HasException(ctx),pocket_kasane_has_submission());
                passed=false;
            }
            JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
            if(!run("let s=kasane.stats();if(s.displayed.commands!==2||s.cache.instances!==1||"
                    "s.cache.templates!==1||s.cache.commands!==1)throw Error('fault quota');"
                    "kasane.patch(tx=>baseRef.setColor(tx,0x00ff00ff));"
                    "kasane.cancel(kasane.poll().ticket);")) passed=false;
        } else {
            if(JS_IsException(result)||JS_HasException(ctx)) {
                JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
                printf("    non-injected failure index=%u: %s\n",fault_index,message?message:"?");
                JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);passed=false;
            }
            finished=true;
        }
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);close_fault_runtime();
        if(live_allocations){printf("    leaked=%zu at fault=%u\n",live_allocations,fault_index);passed=false;}
        if(finished||!passed) break;
    }
    printf("    %s: %u allocation failures injected (%u engine recoveries)\n",label,injected,recovered);
    check(passed&&finished&&injected>0,label);
}

static void allocator_tests(void) {
    fault_sweep("animation spec and wrapper failures abort the whole scene",
                "globalThis.asset=kasane.resource('pets');globalThis.exercise=()=>kasane.replace(tx=>{tx.background(255);"
                "tx.image({resource:asset,bounds:[0,0,32,32]}).animate(tx,{from:{bounds:[0,0,32,32],rotation:0},"
                "to:{bounds:[20,20,84,84],rotation:720},durationMs:1200,easing:'ease-in-out',repeat:'once'})});",false);
    fault_sweep("image resource allocation failures reserve no native slot",
                "globalThis.exercise=()=>kasane.resource('pets');",false);
    fault_sweep("image draw allocation failures abort the candidate",
                "globalThis.asset=kasane.resource('pets');globalThis.exercise=()=>kasane.replace(tx=>{tx.background(255);"
                "return tx.image({resource:asset,bounds:[0,0,32,32],scale:0.5})});",false);
    fault_sweep("text conversion and wrapper allocation failures reclaim the candidate",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(255);"
                "tx.text({bounds:[0,0,100,16],text:'日本語',capacity:24,color:0xffffffff})});",false);
    fault_sweep("ticket, transaction, modal and property allocation failures abort",
                "globalThis.exercise=()=>kasane.replace(tx=>tx.background(0x000000ff));",false);
    fault_sweep("draw wrapper allocation failures abort and recover refs",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);tx.rect(shape)});",false);
    fault_sweep("instance wrapper allocation failures abort and recover instance quota",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);tx.instantiate(tpl,placement)});",false);
    fault_sweep("caught draw allocation failure rolls back an earlier native instance",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);"
                "tx.instantiate(tpl);armFault();try{tx.rect(shape)}catch(e){}finally{disarmFault()}});",true);
    fault_sweep("caught instance allocation failure rolls back earlier commands",
                "globalThis.exercise=()=>kasane.replace(tx=>{tx.background(0x000000ff);"
                "tx.rect(shape);armFault();try{tx.instantiate(tpl,placement)}catch(e){}finally{disarmFault()}});",true);
    fault_sweep("template wrapper failures preserve template and command quotas",
                "globalThis.definition=[shape];globalThis.exercise=()=>kasane.cache.create(definition);",false);
    fault_sweep("poll allocation failures return an exception without partial properties",
                "globalThis.exercise=()=>kasane.poll();",false);
    fault_sweep("features allocation failures clean all nested objects",
                "globalThis.exercise=()=>kasane.features();",false);
    fault_sweep("stats allocation failures clean all nested objects",
                "globalThis.exercise=()=>kasane.stats();",false);

    check(open_fault_runtime("globalThis.exercise=()=>kasane.replace(tx=>tx.background(0x000000ff));"),
          "native arena fault fixture opens");
    pocket_kasane_reset();
    JSValue global=JS_GetGlobalObject(ctx),function=JS_GetPropertyStr(ctx,global,"exercise");
    JS_FreeValue(ctx,global);native_fault=true;
    JSValue result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
    check(!native_fault&&JS_IsException(result)&&!pocket_kasane_active()&&
          !pocket_kasane_has_submission(),"native arena OOM publishes no update");
    JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);JS_FreeValue(ctx,result);
    result=JS_Call(ctx,function,JS_UNDEFINED,0,NULL);
    check(!JS_IsException(result)&&pocket_kasane_has_submission(),"native arena retries after allocation recovery");
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,function);close_fault_runtime();
    check(live_allocations==0,"all fault-test runtimes release every guest allocation");
}

static void base_block_tests(void) {
    check(open_fault_runtime(""),"base block fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_render_stats stats;
    /* One allocation: runtime control, both banks and the adapter state. */
    for(int fault=0;fault<1;fault++) {
        native_after=fault;native_max=0;
        check(run("var failed=false;try{kasane.replace(tx=>tx.rect(shape))}"
                  "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"
                  "if(kasane.stats().nativeBytes!==0||kasane.stats().active)throw Error('partial state');"),
              "each base allocation failure leaves no published state");
        native_after=-1;size_t calls_before=native_calls;
        check(native_bytes==0&&native_max<=KSN_RUNTIME_BASE_BUDGET+KSN_RUNTIME_TAIL_BUDGET&&
              !pocket_kasane_has_submission(),"failed base blocks reclaimed and bounded");
        check(run("kasane.replace(tx=>{tx.background(0x000000ff);globalThis.r=tx.rect(shape)});"),
              "base allocation retries successfully");
        check(present(&stats)==KSN_OK,"retried base presents");
        char accounting[160];
        snprintf(accounting,sizeof(accounting),
                 "if(kasane.stats().nativeBytes!==%zu)throw Error('base accounting');",native_bytes);
        check(run(accounting),"base stats match allocated block sizes");
        check(native_max<=KSN_RUNTIME_BASE_BUDGET+KSN_RUNTIME_TAIL_BUDGET&&native_calls==calls_before+1,
              "the base arena is one bounded allocation");
        size_t calls=native_calls;
        check(run("kasane.patch(tx=>r.setColor(tx,0xabcdef80));"),"patch uses reserved blocks");
        check(present(&stats)==KSN_OK,"patch presents from reserved blocks");
        check(native_calls==calls,"patch and present allocate no native blocks");
        pocket_kasane_reset();check(native_bytes==0,"reset releases all base blocks");
    }
    /* prepare(): the same block before any guest call, silent on failure. */
    native_after=0;pocket_kasane_prepare();native_after=-1;
    check(native_bytes==0&&!pocket_kasane_active(),"prepare under OOM reserves nothing and stays silent");
    size_t calls=native_calls;
    pocket_kasane_prepare();pocket_kasane_prepare();
    check(native_calls==calls+1&&native_bytes>0&&!pocket_kasane_active()&&!pocket_kasane_has_submission(),
          "prepare reserves the arena once without claiming the display");
    char prepared[160];
    snprintf(prepared,sizeof(prepared),"if(kasane.stats().nativeBytes!==%zu||kasane.stats().active)"
             "throw Error('prepared accounting');",native_bytes);
    check(run(prepared),"prepared arena is reported before first use");
    calls=native_calls;
    check(run("kasane.resource('pets');kasane.replace(tx=>{tx.background(0x000000ff);tx.rect(shape)});")&&
          native_calls==calls&&pocket_kasane_active(),"first Kasane calls reuse the prepared arena");
    check(present(&stats)==KSN_OK,"prepared arena presents");
    pocket_kasane_reset();check(native_bytes==0,"reset releases a prepared arena");
    track_native=false;close_fault_runtime();
}

static void lazy_cache_tests(void) {
    check(open_fault_runtime(""),"lazy cache fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_render_stats stats;
    for(int fault=0;fault<3;fault++) {
        check(run("kasane.replace(tx=>{tx.background(0x000000ff);globalThis.r=tx.rect(shape)});"),
              "ordinary drawing builds without cache");
        check(present(&stats)==KSN_OK,"ordinary drawing presents without cache");
        size_t base=native_bytes;
        check(run("globalThis.baseBytes=kasane.stats().nativeBytes;"
                  "if(kasane.stats().cache.reservedBytes!==0)throw Error('eager cache');"
                  "globalThis.oldTicket=kasane.poll().ticket;"),"cache reservation is zero before use");
        native_max=0;native_after=fault;
        check(run("var failed=false;try{kasane.cache.create([shape])}"
                  "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"
                  "if(kasane.stats().nativeBytes!==baseBytes||kasane.stats().cache.reservedBytes!==0)"
                  "throw Error('reservation leak');"),"each cache allocation failure rolls back reservation");
        native_after=-1;
        check(native_bytes==base&&native_max<=3072,"partial native blocks reclaimed and bounded");
        pocket_kasane_invalidate();
        check(present(&stats)==KSN_OK,"committed frame repairs after cache OOM");
        check(run("kasane.patch(tx=>r.setColor(tx,0xabcdef80));"
                  "kasane.cancel(kasane.poll().ticket);"
                  "globalThis.newTpl=kasane.cache.create([shape]);"
                  "var s=kasane.stats();if(s.cache.reservedBytes<=0||"
                  "s.nativeBytes!==baseBytes+s.cache.reservedBytes)throw Error('accounting');"
                  "kasane.replace(tx=>{tx.background(0x000000ff);tx.instantiate(newTpl)});"),
              "old refs survive OOM and cache retries successfully");
        check(present(&stats)==KSN_OK,"retried cache instance presents");
        check(native_max<=3072,"successful cache allocations are bounded");
        if(fault==2){
            pocket_kasane_invalidate();fail_band=1;
            check(present(&stats)==KSN_IO,"committed cache repair fails before guest exit");fail_band=-1;
        }
        pocket_kasane_reset();check(native_bytes==0,"reset frees base and all cache blocks");
    }
    track_native=false;close_fault_runtime();
}

static void notice_lifetime_tests(void){
    check(open_fault_runtime(""),"notice fixture opens");
    pocket_kasane_reset();track_native=true;ksn_render_stats stats;
    for(unsigned i=0;i<10;i++){
        check(run("kasane.replace(tx=>{tx.background(0x0000ffff);tx.rect(shape)});"),"notice APP starts");
        sys_notice notice={.id=i+1,.owner=1,.phase=NOTICE_ACTIVE,.label="SYSTEM NOTICE"};
        check(pocket_kasane_update_notice(&notice,0)==KSN_BUSY,"APP submission defers SYSTEM notice");
        check(present(&stats)==KSN_OK,"notice APP presents");
        size_t before=native_bytes;
        check(pocket_kasane_update_notice(&notice,0)==KSN_OK&&pocket_kasane_system_pending(),"notice submits SYSTEM");
        fail_once=true;check(present(&stats)==KSN_IO,"notice transfer failure retained");
        if(i==9){
            pocket_kasane_reset();check(native_bytes==0,"APP exit frees a failed pending SYSTEM notice");
            continue;
        }
        check(pocket_kasane_update_notice(NULL,0)==KSN_BUSY,"notice removal waits for frozen candidate");
        check(present(&stats)==KSN_OK,"notice retry presents");
        check(pocket_kasane_update_notice(&notice,0)==KSN_OK&&pocket_kasane_notice_composited(),"notice acknowledged independently");
        check(ksn_runtime_stats(KSN_SYSTEM).displayed.commands==5&&native_bytes==before,"notice fits SYSTEM quota without extra allocation");
        if(i%2==0){
            check(pocket_kasane_update_notice(NULL,0)==KSN_OK,"notice removal submits");
            check(present(&stats)==KSN_OK,"notice removal presents");
            check(pocket_kasane_update_notice(NULL,0)==KSN_OK&&!pocket_kasane_notice_composited(),"legacy overlay can resume after removal");
        }
        pocket_kasane_reset();check(native_bytes==0,"notice resources do not pin APP runtime after exit");
    }
    track_native=false;close_fault_runtime();
}
static void system_lifetime_tests(void) {
    check(open_fault_runtime(""),"SYSTEM coexistence fixture opens");
    pocket_kasane_reset();track_native=true;
    ksn_view *system=NULL;ksn_tx tx;ksn_ref ref;ksn_render_stats stats;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,8,8},.clip={0,0,240,135},.opacity=255,
                .data.shape={0x00ff00ff,0,0}};
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"native SYSTEM acquires without APP");
    check(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK&&
          ksn_view_add(system,tx,&d,&ref)==KSN_OK&&ksn_view_submit(system,tx)==KSN_OK,
          "SYSTEM builds without guest submission");
    check(present(&stats)==KSN_OK&&panel_pixels[0]==0x07e0,"SYSTEM presents without active APP");
    size_t base=native_bytes;
    native_after=0;
    check(run("var failed=false;try{kasane.replace(tx=>tx.rect(shape))}"
              "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('missing OOM');"),
          "APP adapter OOM preserves acquired SYSTEM");
    native_after=-1;
    check(native_bytes==base,"APP failure releases only its allocation");
    for(unsigned i=0;i<4;i++) {
        check(run("kasane.replace(tx=>{tx.background(0x102030ff);globalThis.oldR=tx.rect(shape)});"),
              "APP attaches beside native SYSTEM");
        sys_notice notice={.id=1,.owner=1,.phase=NOTICE_ACTIVE,.label="NOTICE"};
        check(pocket_kasane_update_notice(&notice,0)==KSN_BUSY&&!pocket_kasane_notice_composited(),
              "APP-scoped notice does not overwrite an explicit native SYSTEM owner");
        if(i==0||i==3)check(present(&stats)==KSN_OK,"APP commits beside SYSTEM");
        if(i==2){fail_band=1;check(present(&stats)==KSN_IO,"APP partially transfers before exit");fail_band=-1;}
        if(i==3){pocket_kasane_invalidate();fail_band=1;
            check(present(&stats)==KSN_IO,"committed APP/SYSTEM repair fails before exit");fail_band=-1;}
        pocket_kasane_reset();
        check(native_bytes==base&&ksn_runtime_stats(KSN_SYSTEM).displayed.commands==1,
              "APP detach preserves SYSTEM reservation and commands");
        check(present(&stats)==KSN_OK,"SYSTEM repairs after APP exit without JS");
        bool pixels=true;
        for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++)
            if(panel_pixels[y*240+x]!=(x<8&&y<8?0x07e0:0))pixels=false;
        check(pixels,"APP pixels vanish while all SYSTEM pixels survive");
        check(run("var failed=false;try{kasane.patch(tx=>oldR.setColor(tx,0xffffffff))}"
                  "catch(e){failed=e.code==='CLOSED'}if(!failed)throw Error('revived ref');"),
              "old JS DrawRef cannot affect reattached APP");
        pocket_kasane_reset();
    }
    close_fault_runtime();
    check(live_allocations==0,"guest heap fully released while SYSTEM survives");
    ksn_change change={.property=KSN_SET_COLOR,.value.color=0xff0000ff};
    check(ksn_view_begin(system,KSN_PATCH,&tx)==KSN_OK&&
          ksn_view_change(system,tx,ref,&change)==KSN_OK&&ksn_view_submit(system,tx)==KSN_OK,
          "SYSTEM reference remains usable after QuickJS destruction");
    check(present(&stats)==KSN_OK&&panel_pixels[0]==0xf800,"SYSTEM renders with no QuickJS runtime");
    check(ksn_runtime_shutdown()==KSN_OK&&native_bytes==0,"native shutdown releases host blocks");
    track_native=false;
}

static void primitive_tests(void) {
    check(open_fault_runtime(""),"primitive fixture opens");
    ksn_view *system=NULL;
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect primitive descriptors through native owner");
    check(run("var f=kasane.features();if(!f.roundRect||!f.strokeRect||!f.gradient)throw Error('features');"
              "kasane.replace(tx=>{tx.background(0x000000ff);"
              "globalThis.pr=tx.roundRect({bounds:[0.5,0.5,20.5,20.5],radius:8,color:0xff0000ff});"
              "tx.strokeRect({bounds:[24,1,44,21],width:2,color:0x00ff00ff});"
              "globalThis.pg=tx.gradient({bounds:[50,0,60,8],axis:'x',from:0xff0000ff,to:0x0000ffff});"
              "tx.rect({bounds:[-2.5,-1.5,-0.5,0.49],color:0xffffffff});});"),
          "JS exposes rounded rectangle, stroke and gradient with signed rounding");
    ksn_frame frame;ksn_frame_command cmd;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK,"primitive submission is readable");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_ROUND_RECT&&cmd.draw.bounds.x0==1&&cmd.draw.bounds.x1==21&&
          cmd.draw.data.shape.radius==8,"positive half ties round away from zero");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,1,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_STROKE&&cmd.draw.data.shape.width==2,"stroke width reaches native descriptor");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,2,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_GRADIENT&&cmd.draw.data.gradient.axis==0&&
          cmd.draw.data.gradient.to==0x0000ffff&&!cmd.draw.data.gradient.dither,
          "gradient colors, axis and default dither reach native descriptor");
    check(ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,3,&cmd)==KSN_OK&&
          cmd.draw.bounds.x0==-3&&cmd.draw.bounds.y0==-2&&cmd.draw.bounds.x1==-1,
          "negative half ties round away from zero");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK&&panel_pixels[50]==0xf800&&panel_pixels[59]==0x001f,
          "JS gradient renders both exact endpoint colors");
    check(run("kasane.patch(tx=>{pr.setRect(tx,[1.4,1.4,22.5,22.5]);"
              "pg.setClip(tx,[52.5,0,57.5,8]);});"),"new primitives support existing PATCH references");
    check(present(&stats)==KSN_OK,"primitive patch presents");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("var bad=["
              "tx=>tx.roundRect({bounds:[0,0,10,10],radius:6,color:255}),"
              "tx=>tx.strokeRect({bounds:[0,0,20,20],width:3,color:255}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,to:255,axis:'z'}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,to:255,dither:1}),"
              "tx=>tx.gradient({bounds:[0,0,20,20],from:255,get to(){throw Error('getter')}}),"
              "tx=>tx.rect({bounds:[NaN,0,1,1],color:255}),"
              "tx=>tx.rect({bounds:[0,0,32767.5,1],color:255})];"
              "for(var badDraw of bad){let failed=false,inner=false;try{kasane.replace(tx=>{"
              "tx.background(0xffffffff);try{badDraw(tx)}catch(e){inner=true;}})}catch(e){failed=true}"
              "if(!failed||!inner)throw Error('caught invalid primitive submitted');}"),
          "invalid primitive fields, getters and coordinate overflow poison transaction");
    pocket_kasane_invalidate();
    check(present(&stats)==KSN_OK&&!memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels)),
          "rejected primitives leave committed pixels intact");
    check(run("var t=kasane.replace(tx=>{tx.background(255);tx.gradient({bounds:[0,0,20,20],from:255,to:255,"
              "radius:4,dither:true});});kasane.cancel(t);"),"rounded dither gradient can be cancelled");
    close_fault_runtime();
    check(ksn_runtime_shutdown()==KSN_OK,"primitive native owner shuts down");
}

static void viewport_tests(void) {
    check(open_fault_runtime(""),"overlay viewport fixture opens");
    pocket_kasane_set_viewport(140,46,96,22);
    ksn_view *system=NULL;
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect viewport through native owner");
    check(run("kasane.replace(tx=>{tx.background(255);tx.rect({"
              "bounds:[0,0,96,22],color:0xffffffff});});"),
          "overlay submits region-local coordinates");
    ksn_frame frame;ksn_frame_command command;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK&&
          command.draw.bounds.x0==140&&command.draw.bounds.y0==46&&
          command.draw.bounds.x1==236&&command.draw.bounds.y1==68&&
          command.draw.clip.x0==140&&command.draw.clip.y0==46&&
          command.draw.clip.x1==236&&command.draw.clip.y1==68,
          "overlay viewport translates and confines every draw");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK,"overlay viewport baseline presents");
    check(run("globalThis.viewportTemplate=kasane.cache.create(["
              "{bounds:[-10,0,10,10],color:0xffffffff}]);"
              "kasane.replace(tx=>{tx.background(255);"
              "tx.instantiate(viewportTemplate);"
              "tx.instantiate(viewportTemplate,{offset:[20,0]});});"),
          "overlay cache keeps local pixels until placement");
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK&&
          command.draw.bounds.x0==130&&command.draw.bounds.x1==150&&
          command.draw.clip.x0==140&&command.draw.clip.x1==150&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,1,&command)==KSN_OK&&
          command.draw.bounds.x0==150&&command.draw.bounds.x1==170&&
          command.draw.clip.x0==150&&command.draw.clip.x1==170&&
          command.draw.bounds.y0==46&&command.draw.bounds.y1==56,
          "overlay cache translates once and clips after placement");
    check(present(&stats)==KSN_OK,"overlay cache placement presents");
    check(run("if(kasane.features().modal)throw Error('overlay modal advertised');"
              "let inner=false,outer=false;try{kasane.replace(tx=>{try{"
              "tx.modal.open({backdrop:'dim-live',color:0xff0000ff})"
              "}catch(e){inner=e.code==='UNSUPPORTED'}})}catch(e){outer=true}"
              "if(!inner||!outer)throw Error('overlay modal escaped');"),
          "overlay profile rejects viewport-bypassing modal atomically");
    close_fault_runtime();
    check(ksn_runtime_shutdown()==KSN_OK,"overlay viewport owner shuts down");

    check(open_fault_runtime(""),"foreground cache regression fixture opens");
    system=NULL;
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect foreground cache placement");
    check(run("if(!kasane.features().modal)throw Error('foreground modal lost');"
              "var foregroundTemplate=kasane.cache.create(["
              "{bounds:[-10,0,10,10],color:0xffffffff}]);"
              "kasane.replace(tx=>{tx.background(255);"
              "tx.instantiate(foregroundTemplate,{offset:[20,0]});});"),
          "foreground cache may move off-screen template pixels on-screen");
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK&&
          command.draw.bounds.x0==10&&command.draw.bounds.x1==30&&
          command.draw.clip.x0==10&&command.draw.clip.x1==30,
          "foreground cache no longer clips before placement");
    close_fault_runtime();
    check(ksn_runtime_shutdown()==KSN_OK,"foreground cache owner shuts down");
}

static void text_tests(void){
    check(open_fault_runtime(""),"text fixture opens");
    ksn_view *system=NULL;check(ksn_runtime_system_acquire(&system)==KSN_OK,"inspect text through native owner");
    check(run("if(!kasane.features().text)throw Error('feature');"
              "kasane.replace(tx=>{tx.background(255);globalThis.label=tx.text({bounds:[0,7,240,23],"
              "text:'Aあ😀B',capacity:32,font:'caption',color:0xffffffff});label.setReveal(tx,3)});"),
          "JS text submits counted UTF-8 and scalar reveal");
    ksn_frame frame;ksn_frame_command cmd;ksn_render_stats stats;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,0,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_TEXT&&cmd.draw.data.text.bytes==9&&cmd.draw.data.text.capacity==32&&
          cmd.draw.data.text.font==KSN_CAPTION&&cmd.reveal==3&&!memcmp(cmd.text,"Aあ😀B",9),
          "native text snapshot owns bytes, capacity and reveal");
    check(present(&stats)==KSN_OK&&panel_pixels[7*240+1]==0xffff&&panel_pixels[7*240+22]==0,
          "JS text reaches coverage renderer and hides unrevealed scalar");
    check(run("kasane.patch(tx=>{label.setText(tx,'AA');label.setColor(tx,0xff0000ff)});"),
          "setText restores full reveal and accepts color patch");
    check(present(&stats)==KSN_OK&&panel_pixels[7*240+7]==0xf800,"text patch renders new bytes and color");
    memcpy(committed_pixels,panel_pixels,sizeof(panel_pixels));
    check(run("var invalid=['\\ud800','\\udc00','a\\n','a\\0',42,'あ'.repeat(43),'x'.repeat(129)];"
              "for(var value of invalid){let inner=false,outer=false;try{kasane.patch(tx=>{"
              "label.setColor(tx,0x00ff00ff);try{label.setText(tx,value)}catch(e){inner=true}})}"
              "catch(e){outer=true}if(!inner||!outer)throw Error('invalid text committed');}"
              "var mutations=[tx=>label.setReveal(tx,3),tx=>label.setReveal(tx,1.5),"
              "tx=>label.setText(tx,'x'.repeat(33))];for(var mutate of mutations){"
              "let failed=false;try{kasane.patch(mutate)}catch(e){failed=true}if(!failed)throw Error('limit');}"
              "var specs=[{text:'A',capacity:0},{text:'A',capacity:129},{text:'あ',capacity:2},"
              "{text:'A',font:'body\\0'},{text:'A',get capacity(){throw Error('capacity getter')}},"
              "{get text(){throw Error('text getter')}}];for(var spec of specs){let a=false,b=false;"
              "spec.bounds=[0,0,50,20];spec.color=0xffffffff;"
              "try{kasane.replace(tx=>{tx.background(255);try{tx.text(spec)}catch(e){a=true}})}catch(e){b=true}"
              "if(!a||!b)throw Error('bad spec');}"),
          "lone surrogates, controls, byte capacity, bad fonts/reveal and caught getters abort atomically");
    pocket_kasane_invalidate();check(present(&stats)==KSN_OK&&!memcmp(committed_pixels,panel_pixels,sizeof(panel_pixels)),
          "failed text changes preserve committed pixels");
    check(run("globalThis.oldLabel=label;kasane.replace(tx=>{tx.background(255);"
              "globalThis.label=tx.text({bounds:[0,7,240,23],text:'',capacity:128,color:0xffffffff})});"),
          "empty text reserves explicit update capacity");
    check(present(&stats)==KSN_OK,"empty text presents");
    check(run("let failed=false;try{kasane.patch(tx=>oldLabel.setText(tx,'A'))}catch(e){failed=e.code==='CLOSED'}"
              "if(!failed)throw Error('stale');globalThis.sequence=0;"),"stale text reference rejected");
    size_t live=live_allocations;uint32_t native=ksn_runtime_reserved_bytes();
    for(unsigned i=0;i<300;i++){
        if(!run("kasane.patch(tx=>{label.setText(tx,(++sequence&1)?'日本語':'A😀');label.setReveal(tx,1)});")||
           present(&stats)!=KSN_OK){check(false,"repeated text PATCH");break;}
    }
    JS_RunGC(rt);
    check(ksn_runtime_reserved_bytes()==native&&live_allocations<=live+2,"300 text updates keep native reservation and JS allocations bounded");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"text teardown frees guest and native owner");
}

static uint16_t image_over_black(uint16_t rgb,uint8_t alpha){
    unsigned r=rgb>>11,g=(rgb>>5)&63,b=rgb&31;
    r=((r*8+r/4)*alpha+127)/255;
    g=((g*4+g/16)*alpha+127)/255;
    b=((b*8+b/4)*alpha+127)/255;
    return (uint16_t)((r/8)*2048+(g/4)*32+b/8);
}
static void image_tests(void){
    check(open_fault_runtime(""),"image fixture opens");
    ksn_render_stats stats;ksn_image_port port;
    check(ksn_pet_builtin_image(&port)==KSN_OK,"real embedded PPT2 provider validates");
    check(run("globalThis.asset=kasane.resource('pets');globalThis.sprite=null;"
              "if(!kasane.features().image||asset.width!==64||asset.frames!==6||asset.variants!==12)throw Error('metadata');"
              "for(let i=0;i<100;i++)kasane.resource('pets');"
              "kasane.replace(tx=>{tx.background(255);sprite=tx.image({resource:asset,bounds:[0,0,64,64],clip:[0,0,240,135]})});"),
          "JS exposes a borrowed image and repeated handles");
    check(present(&stats)==KSN_OK,"JS image presents");
    bool pixels=true;uint16_t rgb[64];uint8_t alpha[64];
    for(unsigned y=0;y<64;y++){
        port.read_span(port.ctx,0,0,y,0,64,rgb,alpha);
        for(unsigned x=0;x<64;x++){
            if(panel_pixels[y*240+x]!=image_over_black(rgb[x],alpha[x])){
                if(pixels)printf("    pixel %u,%u actual=%04x expected=%04x rgb=%04x alpha=%u\n",x,y,
                    panel_pixels[y*240+x],image_over_black(rgb[x],alpha[x]),rgb[x],alpha[x]);
                pixels=false;
            }
        }
    }
    check(pixels,"JS image pixels match real PPT2 span");
    check(run("kasane.patch(tx=>sprite.setImageFrame(tx,11,5));"),"JS frame PATCH submitted");
    fail_band=1;check(present(&stats)==KSN_IO,"image partial transfer retains snapshot");fail_band=-1;
    check(run("for(let i=0;i<20;i++)kasane.resource('pets');"),"borrowing existing resource while pending does not mutate source");
    check(present(&stats)==KSN_OK,"image repair presents fixed variant and frame");
    pixels=true;
    for(unsigned y=0;y<64;y++){
        port.read_span(port.ctx,11,5,y,0,64,rgb,alpha);
        for(unsigned x=0;x<64;x++)if(panel_pixels[y*240+x]!=image_over_black(rgb[x],alpha[x]))pixels=false;
    }
    check(pixels,"repaired image uses submitted mood");
    for(unsigned size=1;size<=135;size+=7){
        char js[160];
        /* Signed formatting is important for the one-pixel destination. */
        snprintf(js,sizeof(js),"kasane.patch(tx=>sprite.setRect(tx,[-3,7,%d,%d]));",(int)size-3,(int)size+7);
        check(run(js)&&present(&stats)==KSN_OK,"JS setRect stretches the same source without REPLACE");
        pixels=true;
        for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
            uint16_t want=0;
            if((int)x<(int)size-3&&y>=7&&y<size+7){
                unsigned sx=(unsigned)((2ull*(x+3)+1)*64/(2*size));
                unsigned sy=(unsigned)((2ull*(y-7)+1)*64/(2*size));
                port.read_span(port.ctx,11,5,sy,sx,1,rgb,alpha);want=image_over_black(rgb[0],alpha[0]);
            }
            if(panel_pixels[y*240+x]!=want)pixels=false;
        }
        check(pixels,"stretched image and old footprint match pixel-center reference");
    }
    check(run("kasane.patch(tx=>{sprite.setRect(tx,[20,20,84,84]);sprite.setRotation(tx,90)});"),
          "JS rotates the image around its destination center");
    check(present(&stats)==KSN_OK,"rotated image presents");
    pixels=true;
    for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
        uint16_t want=0;
        if(x>=20&&x<84&&y>=20&&y<84){
            port.read_span(port.ctx,11,5,83-x,y-20,1,rgb,alpha);want=image_over_black(rgb[0],alpha[0]);
        }
        if(panel_pixels[y*240+x]!=want)pixels=false;
    }
    check(pixels,"90-degree JS rotation matches independent transposed PPT2 coordinates");
    check(run("for(const bad of [{rotation:NaN},{rotation:Infinity},{rotation:40000},{scale:1,rotation:45},"
              "{sourceWidth:257},{variant:12},{frame:6},{sourceX:33,scale:0.5},{scale:3},{sourceY:-1},{resource:{}},"
              "{get sourceX(){throw Error('getter')}}]){let failed=false;try{kasane.replace(tx=>{"
              "try{tx.image(Object.assign({resource:asset,bounds:[0,0,32,32]},bad))}catch(e){};"
              "tx.rect(shape)})}catch(e){failed=true}if(!failed)throw Error('accepted bad image')}"
              "let failed=false;try{kasane.patch(tx=>sprite.setImageFrame(tx,0,6))}catch(e){failed=true}"
              "if(!failed)throw Error('bad mood');"),"image validation and caught getter errors abort whole update");
    ksn_view *system;ksn_resource resources[15];
    check(ksn_runtime_system_acquire(&system)==KSN_OK,"SYSTEM image owner acquired");
    bool quota=true;for(unsigned i=0;i<15;i++)
        if(ksn_view_host_register_image(system,&port,&resources[i])!=KSN_OK)quota=false;
    ksn_resource extra;
    check(quota&&ksn_view_host_register_image(system,&port,&extra)==KSN_LIMIT,
          "120 JS handles consume exactly one of 16 native resources");
    pocket_kasane_reset();
    check(run("failed=false;try{kasane.replace(tx=>tx.image({resource:asset,bounds:[0,0,64,64]}))}"
              "catch(e){failed=e.code==='CLOSED'}if(!failed)throw Error('stale asset revived');"
              "asset=kasane.resource('pets');"),"APP reset invalidates old resource and reclaims its slot");
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={0,0,32,32},.clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resources[14],.variant=4,.frame=3,.scale=KSN_IMAGE_HALF}};
    ksn_tx tx;ksn_ref ref;
    check(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK&&ksn_view_add(system,tx,&draw,&ref)==KSN_OK&&
          ksn_view_submit(system,tx)==KSN_OK&&present(&stats)==KSN_OK,"SYSTEM images survive APP reset");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"image owners and guest teardown release all storage");
}

static void animation_tests(void){
    check(open_fault_runtime("globalThis.asset=kasane.resource('pets');globalThis.sprite=null;globalThis.motion=null;"
          "globalThis.motionSpec={from:{bounds:[10,20,42,52],rotation:0},to:{bounds:[110,30,206,126],rotation:720},"
          "durationMs:1000,easing:'linear',repeat:'once'};"),"animation fixture opens");
    ksn_render_stats stats;size_t before=ksn_runtime_reserved_bytes();
    native_fault=true;
    check(run("let failed=false;try{kasane.replace(tx=>{tx.background(255);"
              "sprite=tx.image({resource:asset,bounds:[0,0,32,32]});sprite.animate(tx,motionSpec)})}"
              "catch(e){failed=e.code==='OUT_OF_MEMORY'}if(!failed)throw Error('native allocation');"),
          "native track allocation OOM aborts scene");
    check(ksn_runtime_reserved_bytes()==before&&!pocket_kasane_has_submission(),"native track allocation rollback preserves reservation");
    check(run("globalThis.animationTicket=kasane.replace(tx=>{tx.background(255);"
              "sprite=tx.image({resource:asset,bounds:[0,0,32,32],clip:[0,0,240,135]});motion=sprite.animate(tx,motionSpec)});"
              "if(motion.poll()!=='pending')throw Error('premature running');"),"JS submits a declarative multi-turn animation");
    check(ksn_runtime_reserved_bytes()-before<=1024,"optional animation banks stay within 1 KiB");
    check(present(&stats)==KSN_OK,"start pose presents");pocket_kasane_animations_presented(1000000);
    check(run("if(motion.poll()!=='running')throw Error('not started')"),"animation starts on presentation acknowledgement");
    JS_RunGC(rt);size_t live=live_allocations,native=ksn_runtime_reserved_bytes();transfers=0;
    for(uint64_t now=1040000;now<=2040000;now+=40000){
        if(pocket_kasane_advance(now)!=KSN_OK||!pocket_kasane_animation_pending()||present(&stats)!=KSN_OK){
            check(false,"native automatic animation tick");break;
        }
        pocket_kasane_animations_presented(now);
        if(now==2000000)break;
    }
    check(transfers>0&&live_allocations==live&&ksn_runtime_reserved_bytes()==native,
          "movement scaling and rotation advance with no JS calls or allocations");
    check(run("if(motion.poll()!=='finished'||kasane.poll().status!=='PRESENTED')throw Error('completion');"
              "kasane.patch(tx=>sprite.setImageFrame(tx,3,2));"),"completion keeps guest outcome and DrawRef usable");
    check(present(&stats)==KSN_OK,"ordinary JS patch follows automatic completion");
    check(run("kasane.patch(tx=>{motion= sprite.animate(tx,Object.assign({},motionSpec,{repeat:'ping-pong'}))});"),"ping-pong can restart on same DrawRef");
    check(present(&stats)==KSN_OK,"restart presents");pocket_kasane_animations_presented(3000000);
    check(pocket_kasane_advance(3400000)==KSN_OK,"automatic sample prepares");
    fail_once=true;check(present(&stats)==KSN_IO,"automatic transfer failure retained");
    check(pocket_kasane_advance(3700000)==KSN_BUSY,"no sampling over failed automatic frame");
    check(present(&stats)==KSN_OK,"automatic repair succeeds");
    pocket_kasane_set_animation_time(3800000);
    check(run("kasane.patch(tx=>sprite.setImageFrame(tx,4,3));")&&!pocket_kasane_animation_pending(),
          "guest update includes due animation sample in one submission");
    check(present(&stats)==KSN_OK&&ksn_runtime_animation_deadline()==3833334,"coalesced guest frame advances animation clock");
    check(run("let t=kasane.patch(tx=>motion.stop(tx));kasane.cancel(t);"
              "if(motion.poll()!=='running')throw Error('cancelled stop');"
              "kasane.patch(tx=>{motion.stop(tx);sprite.setRect(tx,[1,2,33,34])});"),"stop cancellation and explicit manual takeover");
    check(present(&stats)==KSN_OK&&ksn_runtime_animation_deadline()==UINT64_MAX,"stopped animation schedules no further wake");
    check(run("if(motion.poll()!=='stopped')throw Error('stop state');"
              "kasane.patch(tx=>motion.finish(tx));"),"finish moves stopped animation to its explicit endpoint");
    check(present(&stats)==KSN_OK,"finish presents endpoint");
    check(run("kasane.patch(tx=>{motion=sprite.animate(tx,motionSpec)});"),"hidden animation fixture restarts");
    check(present(&stats)==KSN_OK,"hidden start presents");pocket_kasane_animations_presented(6000000);
    ksn_runtime_set_hidden(true);
    check(pocket_kasane_advance(6800000)==KSN_OK&&!pocket_kasane_has_submission()&&ksn_runtime_animation_deadline()==UINT64_MAX,
          "hidden animation neither samples nor schedules a display wake");
    ksn_runtime_set_hidden(false);
    check(pocket_kasane_advance(6800000)==KSN_OK&&present(&stats)==KSN_OK,"visible animation catches up to current time");
    ksn_runtime_set_reduce_motion(true);
    check(pocket_kasane_advance(6800001)==KSN_OK&&present(&stats)==KSN_OK,"reduce-motion advances to endpoint");
    check(run("if(motion.poll()!=='finished')throw Error('reduce motion')"),"reduce-motion completion commits");
    pocket_kasane_reset();check(run("if(motion.poll()!=='discarded')throw Error('old animation')"),"APP exit invalidates animation wrapper");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"animation teardown releases all guest and native storage");
}

int main(void) {
    rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
    check(pocket_kasane_install(ctx,NULL)==ESP_OK,"namespace installs");
    check(run("if(kasane.features().capacity.refs!==32)throw Error('features');"
              "if(kasane.stats().active||kasane.stats().nativeBytes!==0)throw Error('lazy')"),
          "features do not allocate the native arena");
    check(!pocket_kasane_active(),"native display remains inactive after feature test");

    presenter_tests();
    app_presenter_tests();
    reactive_presenter_tests();
#ifdef KSN_TEST_DUAL_SOURCE
    dual_source_mount_tests();
#endif
    external_source_mount_tests();
    wall_source_service_tests();

    check(run("globalThis.tpl=kasane.cache.create(["
              "{bounds:[0,0,10,10],color:0xff0000ff},"
              "{bounds:[2,2,8,8],color:0x00ff00aa,opacity:180}]);"
              "globalThis.oldRef=null;globalThis.inst=null;"
              "globalThis.ticket=kasane.replace(tx=>{tx.background(0x000010ff);"
              "oldRef=tx.rect({bounds:[3,3,20,20],color:0xffffffff,opacity:200});"
              "let second=tx.rect({bounds:[8,8,25,25],color:0x2080ffff});"
              "tx.group(oldRef,2,190);inst=tx.instantiate(tpl,{offset:[30,20]});});"),
          "replace builds rect, group, and cached instance");
    check(pocket_kasane_active()&&pocket_kasane_has_submission(),
          "first submit claims display ownership");
    ksn_render_stats stats;transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&stats.transferred_bytes==240*135*2,
          "initial replace presents all display bands");
    check(run("if(kasane.poll().status!=='PRESENTED')throw Error('poll')"),
          "poll reports presented state");

    check(run("ticket=kasane.patch(tx=>{oldRef.setRect(tx,[4,12,24,28]);"
              "oldRef.setColor(tx,0xf08020ff);inst.place(tx,{offset:[38,24],opacity:170});});"),
          "patch mutates refs without rebuilding topology");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers>0&&transfers<17,
          "patch transfers only dirty bands");

    check(run("globalThis.modalRef=null;ticket=kasane.replace(tx=>{"
              "tx.background(0x001020ff);tx.rect({bounds:[0,0,240,135],color:0x103050ff});"
              "tx.modal.open({backdrop:'dim-live',color:0x00000088,focus:7});"
              "modalRef=tx.rect({bounds:[40,25,200,110],color:0x80c0eedd,opacity:230});});"),
          "dim-live modal builds in a replace");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_MODAL,
          "modal input scope commits with presentation");
    check(run("let stale=false;try{kasane.patch(tx=>oldRef.setColor(tx,1))}"
              "catch(e){stale=e.code==='CLOSED'}if(!stale)throw Error('old ref live')"),
          "successful replace invalidates old draw refs");

    check(run("ticket=kasane.replace(tx=>{tx.background(0x101820ff);"
              "globalThis.newRef=tx.rect({bounds:[2,2,20,20],color:0xffffffff});"
              "tx.modal.close();});"),"modal close rebuilds app state");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "modal returns input to app after presentation");

    check(run("ticket=kasane.patch(tx=>newRef.setColor(tx,0x00ffffff));kasane.cancel(ticket);"
              "if(kasane.poll().status!=='DISCARDED')throw Error('cancel')"),
          "explicit cancel preserves the displayed state");
    check(!pocket_kasane_has_submission(),"cancel leaves no pending submission");

    check(run("ticket=kasane.patch(tx=>newRef.setRect(tx,[5,5,22,22]))"),
          "retry test submits a patch");
    fail_once=true;transfers=0;
    check(present(&stats)==KSN_IO&&pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED,
          "LCD failure retains work and blocks app input");
    check(present(&stats)==KSN_OK&&!pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "full repair retry commits and restores input");

    check(run("let rejected=false;try{kasane.patch(tx=>Promise.resolve())}"
              "catch(e){rejected=e.code==='INVALID_ARGUMENT'}"
              "if(!rejected)throw Error('thenable accepted')"),
          "thenable builders are rejected and aborted");
    check(!pocket_kasane_has_submission(),"thenable leaves no builder or submission");

    check(run("let limited=false;try{kasane.replace(tx=>{tx.background(0x000000ff);"
              "globalThis.refs=[];for(let i=0;i<33;i++)refs.push(tx.rect({"
              "bounds:[i,0,i+1,1],color:0xffffffff}));});}"
              "catch(e){limited=e.code==='LIMIT_EXCEEDED'}"
              "if(!limited)throw Error('ref limit')"),
          "draw-reference exposure is capped at 32");
    check(!pocket_kasane_has_submission(),"limit failure aborts the whole replace");

    check(run("if(kasane.stats().nativeBytes<=kasane.stats().cache.reservedBytes)throw Error('native accounting')"),
          "stats reports the allocated native arena");
    atomicity_tests();
    repair_tests();
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    allocator_tests();
    base_block_tests();
    lazy_cache_tests();
    system_lifetime_tests();
    notice_lifetime_tests();
    viewport_tests();
    primitive_tests();
    text_tests();
    image_tests();
    animation_tests();
    printf("%s: %u failure(s)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
