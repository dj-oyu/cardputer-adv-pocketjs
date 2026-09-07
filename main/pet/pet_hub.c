#include "pet_hub.h"
#include "pet_hub_core.h"
#include "pocket_api.h"
#include "paint.h"
#include "sound.h"
#include "solar_time.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static pet_hub_t hub;
static QueueHandle_t inbox;
static nvs_handle_t prefs;
static bool opened, changed;
static uint32_t clock_utc;
static uint64_t clock_ms, ringing, next_tone;
static char alert[PET_LABEL_CHARS+1];
#include "pet_assets.h"
#include "pet_pixels.h"
static uint64_t now_ms(void){return esp_timer_get_time()/1000;}
static uint32_t read32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t utc_now(void) {
    solar_time_sample_t t=solar_time_now(0);
    if(t.source==SOLAR_TIME_UTC)return (uint32_t)(t.days*86400.0+946728000.0);
    return clock_utc?clock_utc+(uint32_t)((now_ms()-clock_ms)/1000):0;
}
static bool persist(void) {
    if(opened&&nvs_set_blob(prefs,"state",&hub.saved,sizeof(hub.saved))==ESP_OK&&nvs_commit(prefs)==ESP_OK)return true;
    ESP_LOGW("pet","PET_SAVE_FAILED");return false;
}
void pet_hub_init(void) {
    pet_hub_defaults(&hub);opened=nvs_open("pet_hub",NVS_READWRITE,&prefs)==ESP_OK;
    if(opened) {
        pet_hub_saved_t s;size_t n=sizeof(s);
        if(nvs_get_blob(prefs,"state",&s,&n)==ESP_OK&&n==sizeof(s)&&s.magic==PET_HUB_MAGIC&&
           s.selected<12&&s.wake_minute>=-1&&s.wake_minute<1440&&s.utc_offset>=-50400&&s.utc_offset<=50400)hub.saved=s;
    }
    inbox=xQueueCreate(4,PET_WIRE_BYTES);
}
bool pet_hub_usb(uint8_t c) {
    // RS + 'P' + 96 hex digits + LF. Bad frames are swallowed, never keys.
    static unsigned state, length;
    static uint8_t bytes[PET_WIRE_BYTES];
    if(c==0x1e){state=1;length=0;return true;}
    if(!state)return false;
    if(c=='\n'){if(state==2&&length==96&&inbox)xQueueSend(inbox,bytes,0);state=0;return true;}
    if(state==1){state=c=='P'?2:3;return true;}
    if(state==3)return true;
    int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
    if(v<0||length>=96){state=3;return true;}
    if(!(length&1))bytes[length/2]=(uint8_t)(v<<4);else bytes[length/2]|=v;
    length++;return true;
}
bool pet_hub_pump(void) {
    uint64_t now=now_ms();
    if(pet_hub_tick(&hub,now,utc_now()))persist();
    uint8_t d[PET_WIRE_BYTES];
    while(inbox&&xQueueReceive(inbox,d,0)==pdTRUE) {
        if(d[0]==1&&d[1]==2&&pet_crc(d,44)==read32(d+44)) {
            int32_t offset=(int32_t)read32(d+36);uint32_t stamp=read32(d+40);
            if(offset>=-50400&&offset<=50400&&stamp>=1577836800u) {
                if(offset!=hub.saved.utc_offset){hub.saved.utc_offset=offset;persist();}
                clock_utc=stamp;clock_ms=now;changed=true;
                ESP_LOGI("pet","PET_ACK 2 %lu",(unsigned long)read32(d+4));
            }
            continue;
        }
        pet_hub_saved_t before=hub.saved;
        if(pet_hub_packet(&hub,d)) {
            if(!persist()){hub.saved=before;continue;}
            pet_usage_t *p=&hub.saved.usage[d[2]];
            clock_utc=read32(d+40);clock_ms=now;changed=true;
            ESP_LOGI("pet","PET_ACK %u %lu",d[2],(unsigned long)p->sequence);
        } else if(d[2]<2&&pet_crc(d,44)==((uint32_t)d[44]|(uint32_t)d[45]<<8|(uint32_t)d[46]<<16|(uint32_t)d[47]<<24)) {
            // A lost ACK is safe to retry. Invalid/newer frames get no ACK.
            pet_usage_t *p=&hub.saved.usage[d[2]];
            uint32_t seq=(uint32_t)d[4]|(uint32_t)d[5]<<8|(uint32_t)d[6]<<16|(uint32_t)d[7]<<24;
            uint32_t stream=(uint32_t)d[8]|(uint32_t)d[9]<<8|(uint32_t)d[10]<<16|(uint32_t)d[11]<<24;
            if(d[0]==1&&d[1]==1&&seq==p->sequence&&stream==p->stream)
                ESP_LOGI("pet","PET_ACK %u %lu",d[2],(unsigned long)seq);
        }
    }
    if(!alert[0]&&pet_hub_take(&hub,alert)){ringing=now;next_tone=now;changed=true;}
    if(alert[0]&&now-ringing<30000&&now>=next_tone) {
        sound_tone(1046,200,0.35f,NULL,NULL);next_tone=now+2000;
    }
    bool result=changed;changed=false;return result;
}
bool pet_hub_key(board_key_t key) {
    if(!alert[0]||key==KEY_NONE)return false;
    if(key==KEY_RIGHT) {
        if(!pet_hub_timer(&hub,"snooze",alert,now_ms()+300000))return true;
    } else if(key!=KEY_ENTER&&key!=KEY_BACK)return true;
    alert[0]=0;changed=true;return true;
}
void pet_hub_overlay(uint16_t *pixels, int y, int rows) {
    if(!alert[0]||y>=48)return;
    paint_begin(pixels,y,rows);paint_fill(0,0,240,48,board_rgb(8,12,32));
    paint_fill(0,46,240,2,board_rgb(62,220,208));
    paint_ascii(42,10,alert,board_rgb(240,247,230));
    paint_ascii(42,29,"ENTER OK > SNOOZE",board_rgb(99,214,221));
    pet_pixels_draw(pet_compact_start,hub.saved.selected,pixels,LCD_W,y,rows,5,5,2,3);
}
static void number(JSContext *c,JSValue o,const char *k,double n){JS_SetPropertyStr(c,o,k,JS_NewFloat64(c,n));}
// Both writers below roll the value back before they throw, so the caller learns
// the write did not land -- not merely that something went wrong.
static JSValue save_failed(JSContext *c,const char *op) {
    return pocket_api_throw(c,opened?POCKET_ERR_IO_ERROR:POCKET_ERR_NOT_AVAILABLE,op,
                            opened?"NVS write failed":"pet storage is not open",
                            opened,POCKET_OUTCOME_NOT_APPLIED);
}
// NULL when the string is acceptable, else the code to throw with: over the
// published length is LIMIT_EXCEEDED, anything not printable ASCII is
// INVALID_ARGUMENT. That split is what lets an app read maxLabelChars and act.
static const char *text_code(const char *s,unsigned max) {
    if(!s||!*s)return POCKET_ERR_INVALID_ARGUMENT;
    if(strlen(s)>max)return POCKET_ERR_LIMIT_EXCEEDED;
    for(;*s;s++)if((unsigned char)*s<32||(unsigned char)*s>126)return POCKET_ERR_INVALID_ARGUMENT;
    return NULL;
}
static JSValue select_pet(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;
    if(argc) {
        int32_t n;if(JS_ToInt32(c,&n,a[0]))return JS_EXCEPTION;
        if(n<0||n>=12)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.select",
                                             "pet index 0..11",false,NULL);
        uint32_t old=hub.saved.selected;hub.saved.selected=n;
        if(!persist()){hub.saved.selected=old;return save_failed(c,"pet.select");}
    }
    return JS_NewInt32(c,hub.saved.selected);
}
static JSValue rewards(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;int32_t n=hub.saved.selected;
    if(argc&&JS_ToInt32(c,&n,a[0]))return JS_EXCEPTION;
    if(n<0||n>=12)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.rewards",
                                         "pet index 0..11",false,NULL);
    return JS_NewFloat64(c,hub.saved.food[n]);
}
static JSValue clock_read(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;(void)argc;(void)a;
    uint32_t utc=utc_now();JSValue o=JS_NewObject(c);
    JS_SetPropertyStr(c,o,"utc",utc?JS_NewFloat64(c,utc):JS_NULL);
    JS_SetPropertyStr(c,o,"minute",utc?JS_NewInt32(c,((int64_t)utc+hub.saved.utc_offset)%86400/60):JS_NULL);
    number(c,o,"wakeMinute",hub.saved.wake_minute);return o;
}
static JSValue wake(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;int32_t hour,minute=0;
    if(!argc)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.wake","hour required",false,NULL);
    if(JS_ToInt32(c,&hour,a[0])||(argc>1&&JS_ToInt32(c,&minute,a[1])))return JS_EXCEPTION;
    if(hour < -1||hour>23||minute<0||minute>59)
        return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.wake","invalid alarm time",false,NULL);
    int32_t old=hub.saved.wake_minute;hub.saved.wake_minute=hour<0?-1:hour*60+minute;
    if(!persist()){hub.saved.wake_minute=old;return save_failed(c,"pet.wake");}
    return JS_UNDEFINED;
}
static JSValue alarm_set(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;
    if(argc<3)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.alarm",
                                      "id, seconds, label required",false,NULL);
    const char *id=JS_ToCString(c,a[0]),*label=JS_ToCString(c,a[2]);
    double seconds;
    const char *code=JS_ToFloat64(c,&seconds,a[1])==0&&isfinite(seconds)&&
                     (seconds==0||seconds>=1)&&seconds<=604800?NULL:POCKET_ERR_INVALID_ARGUMENT;
    if(!code)code=text_code(id,PET_ID_CHARS);
    if(!code)code=text_code(label,PET_LABEL_CHARS);
    // A full timer table is worth retrying: the four slots free themselves as
    // they fire, which is not true of any of the checks above it.
    bool busy=!code&&!pet_hub_timer(&hub,id,label,seconds?now_ms()+(uint64_t)(seconds*1000):0);
    JS_FreeCString(c,id);JS_FreeCString(c,label);
    if(busy)return pocket_api_throw(c,POCKET_ERR_LIMIT_EXCEEDED,"pet.alarm",
                                    "all four timers are busy",true,NULL);
    if(code)return pocket_api_throw(c,code,"pet.alarm",
                                    "id up to 16 and label up to 24 printable ASCII, 0 or 1..604800 seconds",
                                    false,NULL);
    return JS_UNDEFINED;
}
static JSValue notify(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;if(!argc)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.notify",
                                                "label required",false,NULL);
    const char *s=JS_ToCString(c,a[0]);
    const char *code=text_code(s,PET_LABEL_CHARS);
    // The queue drains as the user dismisses each alert, so a full one is a
    // "later", not a "never".
    bool full=!code&&!pet_hub_notify(&hub,s);
    JS_FreeCString(c,s);
    if(full)return pocket_api_throw(c,POCKET_ERR_LIMIT_EXCEEDED,"pet.notify",
                                    "eight notifications are queued",true,NULL);
    if(code)return pocket_api_throw(c,code,"pet.notify",
                                    "label: up to 24 printable ASCII characters",false,NULL);
    return JS_UNDEFINED;
}
static JSValue timer_read(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;if(!argc)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.timer",
                                                "timer id required",false,NULL);
    const char *id=JS_ToCString(c,a[0]);if(!id)return JS_EXCEPTION;
    JSValue result=JS_NULL;
    for(unsigned i=0;i<PET_MAX_TIMERS;i++)if(hub.timers[i].due&&!strcmp(hub.timers[i].id,id)) {
        uint64_t now=now_ms();result=JS_NewFloat64(c,hub.timers[i].due>now?(hub.timers[i].due-now)/1000.0:0);break;
    }
    JS_FreeCString(c,id);return result;
}
static JSValue usage(JSContext *c,JSValueConst self,int argc,JSValueConst *a) {
    (void)self;int32_t n=0;if(argc&&JS_ToInt32(c,&n,a[0]))return JS_EXCEPTION;
    if(n<0||n>1)return pocket_api_throw(c,POCKET_ERR_INVALID_ARGUMENT,"pet.usage",
                                        "provider 0=Codex, 1=Claude",false,NULL);
    pet_usage_t *p=&hub.saved.usage[n];JSValue o=JS_NewObject(c),windows=JS_NewArray(c);
    number(c,o,"observed",p->observed);
    JS_SetPropertyStr(c,o,"stale",JS_NewBool(c,!p->observed||utc_now()>p->observed+180));
    JS_SetPropertyStr(c,o,"tokens",p->baseline?JS_NewFloat64(c,(double)p->tokens):JS_NULL);
    for(unsigned w=0;w<2;w++) {
        JSValue v=JS_NewObject(c);
        JS_SetPropertyStr(c,v,"usedPercent",p->used[w]<0?JS_NULL:JS_NewFloat64(c,p->used[w]/100.0));
        JS_SetPropertyStr(c,v,"resetsAt",p->reset[w]?JS_NewFloat64(c,p->reset[w]):JS_NULL);
        JS_SetPropertyUint32(c,windows,w,v);
    }
    JS_SetPropertyStr(c,o,"windows",windows);return o;
}
// Every number here is enforced a few lines up or in pet_hub_core.c; nothing is
// published that the code does not refuse to exceed.
static const pocket_limit_t pet_limits[]={
    {.name="maxPets",         .kind=POCKET_LIMIT_INT,.number=12},
    {.name="maxNotifications",.kind=POCKET_LIMIT_INT,.number=PET_MAX_ALERTS},
    {.name="maxTimers",       .kind=POCKET_LIMIT_INT,.number=PET_MAX_TIMERS},
    {.name="maxLabelChars",   .kind=POCKET_LIMIT_INT,.number=PET_LABEL_CHARS},  // notify() and alarm()
    {.name="maxTimerIdChars", .kind=POCKET_LIMIT_INT,.number=PET_ID_CHARS},
    {.name="maxAlarmSeconds", .kind=POCKET_LIMIT_INT,.number=604800},
    {.name="maxSpeechChars",  .kind=POCKET_LIMIT_INT,.number=PET_SPEECH_CHARS}, // pet_assets.c say()
    {0},
};
// Without NVS the hub keeps nothing and drops every usage frame, so rewards()
// never moves and select()/wake() always throw: not available, even though
// clock() and place() still answer. opened never changes after pet_hub_init(),
// so there is no moment that would call pocket_api_capability_changed().
#define PET_REASON_NO_STORAGE "NO_STORAGE"
static void pet_probe(const pocket_capability_t *cap,bool *available,const char **reason) {
    (void)cap;*available=opened;*reason=opened?NULL:PET_REASON_NO_STORAGE;
}
static const pocket_capability_t capability={.name="pet.companion",.supported=true,
    .available=false,.reason=PET_REASON_NO_STORAGE,.limits=pet_limits,.probe=pet_probe};
// pet_assets.c contributes to the same namespace, second, so the eight methods
// here are defined before its four -- the order the object had when one file
// built it and the other reached in afterwards.
static esp_err_t build_pet(JSContext *ctx,JSValueConst ns,void *user) {
    (void)user;
    JSValue pet=(JSValue)ns;
#define FN(name,func,n) JS_SetPropertyStr(ctx,pet,name,JS_NewCFunction(ctx,func,name,n))
    FN("select",select_pet,1);FN("rewards",rewards,1);FN("clock",clock_read,0);
    FN("wake",wake,2);FN("alarm",alarm_set,3);FN("notify",notify,1);FN("usage",usage,1);FN("timer",timer_read,1);
#undef FN
    return ESP_OK;
}
esp_err_t pet_hub_install(JSContext *ctx,void *unused) {
    (void)unused;pocket_api_register(&capability);
    return pocket_api_lazy(ctx,"pet",build_pet,NULL);
}
