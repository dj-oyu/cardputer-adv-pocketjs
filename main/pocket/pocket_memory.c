#include "pocket_memory.h"
#include "pocket_api.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#endif

#define NATIVE_SAMPLE_US 100000u
#define RECOVERY_US 500000u
/* Initial device policy. Revisit against music/SD low-heap gates before
 * treating these observations as an allocation guarantee. */
#define GUEST_ENTER_BYTES 32768u
#define GUEST_EXIT_BYTES 49152u
#define FREE_ENTER_BYTES 24576u
#define FREE_EXIT_BYTES 36864u
#define LARGEST_ENTER_BYTES 16384u
#define LARGEST_EXIT_BYTES 24576u

typedef struct {
    JSContext *ctx;
    JSValue listener;
    uint32_t listener_id;
    uint32_t episode;
    uint32_t quota_failures, allocator_failures, native_failures;
    uint32_t callback_failures;
    size_t first_requested, first_used;
    size_t guest_used, guest_limit, internal_free, internal_largest;
    uint64_t sample_us, native_sample_us, last_failure_us;
    uint64_t healthy_since[3];
    uint8_t mask, episode_reasons, failure_reasons;
    bool open, pending, sampled, native_sampled;
} pressure_state;

static pressure_state pressure;
static uint32_t next_listener_id;
static atomic_bool native_enabled;
static atomic_uint native_failed;

static uint64_t now_us(void) {
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000u+(uint64_t)ts.tv_nsec/1000u;
#endif
}

static uint32_t sat_add(uint32_t a,uint32_t b) {
    return UINT32_MAX-a<b?UINT32_MAX:a+b;
}

static void set_mask(uint8_t mask) {
    if(mask==pressure.mask)return;
    if(!pressure.mask&&mask) {
        if(pressure.episode<UINT32_MAX)pressure.episode++;
        pressure.episode_reasons=0;
    }
    pressure.mask=mask;
    pressure.episode_reasons|=mask;
    if(pressure.open)pressure.pending=true;
}

static void threshold_bit(uint8_t bit,unsigned index,size_t value,
                          size_t enter,size_t exit,uint64_t now) {
    uint8_t mask=pressure.mask;
    if(value<enter) {
        pressure.healthy_since[index]=0;
        mask|=bit;
    } else if(mask&bit) {
        if(value>=exit) {
            if(!pressure.healthy_since[index])pressure.healthy_since[index]=now;
            if(now-pressure.healthy_since[index]>=RECOVERY_US)mask&=(uint8_t)~bit;
        } else pressure.healthy_since[index]=0;
    }
    set_mask(mask);
}

void pocket_memory_native_failure(size_t requested,uint32_t caps) {
    (void)requested;(void)caps;
    if(!atomic_load_explicit(&native_enabled,memory_order_relaxed))return;
    unsigned old=atomic_load_explicit(&native_failed,memory_order_relaxed);
    while(old<UINT32_MAX&&!atomic_compare_exchange_weak_explicit(
              &native_failed,&old,old+1,memory_order_relaxed,memory_order_relaxed)){}
}

#if defined(ESP_PLATFORM) && !defined(CONFIG_POCKET_VM_OOMPROBE)
static void alloc_failed(size_t size,uint32_t caps,const char *operation) {
    (void)operation;
    pocket_memory_native_failure(size,caps);
}
#endif

void pocket_memory_oom(const JSOOMCanary *canary,uint64_t now) {
    if(!pressure.ctx||!canary||!canary->count)return;
    pressure.quota_failures=sat_add(pressure.quota_failures,canary->quota_count);
    pressure.allocator_failures=sat_add(pressure.allocator_failures,canary->allocator_count);
    if(canary->quota_count)pressure.failure_reasons|=1u;
    if(canary->allocator_count)pressure.failure_reasons|=2u;
    pressure.first_requested=canary->first_req;
    pressure.first_used=canary->first_used;
    pressure.last_failure_us=now;
    set_mask(pressure.mask|POCKET_MEMORY_FAILURE);
}

bool pocket_memory_native_sample_due(uint64_t now) {
    return pressure.ctx&&(!pressure.native_sampled||
           now-pressure.native_sample_us>=NATIVE_SAMPLE_US);
}

void pocket_memory_sample(uint64_t now,size_t used,size_t limit,
                          bool native_valid,size_t free_bytes,size_t largest) {
    if(!pressure.ctx)return;
    pressure.guest_used=used;pressure.guest_limit=limit;
    pressure.sample_us=now;pressure.sampled=true;
    unsigned native=atomic_exchange_explicit(&native_failed,0,memory_order_relaxed);
    if(native) {
        pressure.native_failures=sat_add(pressure.native_failures,native);
        pressure.failure_reasons|=4u;
        pressure.last_failure_us=now;
        set_mask(pressure.mask|POCKET_MEMORY_FAILURE);
    }
    if(limit)threshold_bit(POCKET_MEMORY_GUEST,0,limit>used?limit-used:0,
                           GUEST_ENTER_BYTES,GUEST_EXIT_BYTES,now);
    if(native_valid) {
        pressure.internal_free=free_bytes;pressure.internal_largest=largest;
        pressure.native_sample_us=now;pressure.native_sampled=true;
        threshold_bit(POCKET_MEMORY_FREE,1,free_bytes,
                      FREE_ENTER_BYTES,FREE_EXIT_BYTES,now);
        threshold_bit(POCKET_MEMORY_LARGEST,2,largest,
                      LARGEST_ENTER_BYTES,LARGEST_EXIT_BYTES,now);
    }
    if((pressure.mask&POCKET_MEMORY_FAILURE)&&
       now-pressure.last_failure_us>=RECOVERY_US)
        set_mask(pressure.mask&~POCKET_MEMORY_FAILURE);
}

static void close_listener(JSContext *ctx,uint32_t id) {
    if(!pressure.open||id!=pressure.listener_id)return;
    pressure.open=false;pressure.pending=false;
    JS_FreeValue(ctx,pressure.listener);
    pressure.listener=JS_UNDEFINED;
}

static JSValue js_close(JSContext *ctx,JSValueConst self,int argc,
                        JSValueConst *argv,int magic,JSValueConst *data) {
    (void)self;(void)argc;(void)argv;(void)magic;
    uint32_t id=0;
    JS_ToUint32(ctx,&id,data[0]);
    close_listener(ctx,id);
    return JS_UNDEFINED;
}

static JSValue js_pressure(JSContext *ctx,JSValueConst self,int argc,
                           JSValueConst *argv) {
    (void)ctx;(void)self;(void)argc;(void)argv;
    return JS_NewUint32(ctx,pressure.mask);
}

#define SET(name,value) do { \
    if(JS_SetPropertyStr(ctx,out,name,value)<0)goto fail; \
} while(0)
static JSValue js_info(JSContext *ctx,JSValueConst self,int argc,
                       JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    JSValue out=JS_NewObject(ctx);
    if(JS_IsException(out))return out;
    uint64_t age=pressure.sampled?(now_us()-pressure.sample_us)/1000u:0;
    SET("mask",JS_NewUint32(ctx,pressure.mask));
    SET("episode",JS_NewUint32(ctx,pressure.episode));
    SET("episodeReasons",JS_NewUint32(ctx,pressure.episode_reasons));
    SET("failureReasons",JS_NewUint32(ctx,pressure.failure_reasons));
    SET("guestUsedBytes",JS_NewInt64(ctx,(int64_t)pressure.guest_used));
    SET("guestLimitBytes",JS_NewInt64(ctx,(int64_t)pressure.guest_limit));
    SET("guestHeadroomBytes",JS_NewInt64(ctx,(int64_t)(
        pressure.guest_limit>pressure.guest_used?
        pressure.guest_limit-pressure.guest_used:0)));
    SET("internalFreeBytes",pressure.native_sampled?
        JS_NewInt64(ctx,(int64_t)pressure.internal_free):JS_NULL);
    SET("internalLargestBytes",pressure.native_sampled?
        JS_NewInt64(ctx,(int64_t)pressure.internal_largest):JS_NULL);
    SET("sampleAgeMs",pressure.sampled?JS_NewInt64(ctx,(int64_t)age):JS_NULL);
    SET("quotaFailures",JS_NewUint32(ctx,pressure.quota_failures));
    SET("allocatorFailures",JS_NewUint32(ctx,pressure.allocator_failures));
    SET("nativeFailures",JS_NewUint32(ctx,pressure.native_failures));
    SET("callbackFailures",JS_NewUint32(ctx,pressure.callback_failures));
    SET("firstRequestedBytes",JS_NewInt64(ctx,(int64_t)pressure.first_requested));
    SET("firstUsedBytes",JS_NewInt64(ctx,(int64_t)pressure.first_used));
    return out;
fail:
    JS_FreeValue(ctx,out);
    return JS_EXCEPTION;
}
#undef SET

static JSValue js_on_pressure(JSContext *ctx,JSValueConst self,int argc,
                              JSValueConst *argv) {
    (void)self;
    if(argc<1||!JS_IsFunction(ctx,argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                "memory.onPressure","listener must be a function",false,NULL);
    if(pressure.open)
        return pocket_api_throw(ctx,POCKET_ERR_BUSY,
                                "memory.onPressure","one listener per guest",false,NULL);
    JSValue sub=JS_NewObject(ctx);
    if(JS_IsException(sub))return sub;
    uint32_t id=next_listener_id+1u;
    if(!id)id=1;
    JSValue data=JS_NewUint32(ctx,id);
    JSValue close=JS_NewCFunctionData2(ctx,js_close,"close",0,0,1,&data);
    if(JS_IsException(close)) { JS_FreeValue(ctx,sub);return close; }
    if(JS_SetPropertyStr(ctx,sub,"close",close)<0) {
        JS_FreeValue(ctx,sub);return JS_EXCEPTION;
    }
    pressure.listener=JS_DupValue(ctx,argv[0]);
    pressure.listener_id=next_listener_id=id;
    pressure.open=true;pressure.pending=pressure.mask!=0;
    return sub;
}

void pocket_memory_pump(bool leaving) {
    if(leaving||!pressure.ctx||!pressure.open||!pressure.pending)return;
    JSContext *ctx=pressure.ctx;
    uint32_t id=pressure.listener_id;
    pressure.pending=false;
    JSValue fn=JS_DupValue(ctx,pressure.listener);
    JSValue args[2]={JS_NewUint32(ctx,pressure.mask),
                     JS_NewUint32(ctx,pressure.episode)};
    JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,2,(JSValueConst *)args);
    if(JS_IsException(result)) {
        JSValue error=JS_GetException(ctx);
        JS_FreeValue(ctx,error);
        pressure.callback_failures=sat_add(pressure.callback_failures,1);
        /* A thrown/OOM listener may have performed side effects. Do not retry
         * it or stringify its error under pressure. */
        close_listener(ctx,id);
    }
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,fn);
}

static esp_err_t build_memory(JSContext *ctx,JSValueConst ns,void *user) {
    (void)user;
    JSValue f=JS_NewCFunction(ctx,js_pressure,"pressure",0);
    if(JS_IsException(f))return ESP_ERR_NO_MEM;
    if(JS_SetPropertyStr(ctx,ns,"pressure",f)<0)return ESP_ERR_NO_MEM;
    f=JS_NewCFunction(ctx,js_info,"info",0);
    if(JS_IsException(f))return ESP_ERR_NO_MEM;
    if(JS_SetPropertyStr(ctx,ns,"info",f)<0)return ESP_ERR_NO_MEM;
    f=JS_NewCFunction(ctx,js_on_pressure,"onPressure",1);
    if(JS_IsException(f))return ESP_ERR_NO_MEM;
    if(JS_SetPropertyStr(ctx,ns,"onPressure",f)<0)return ESP_ERR_NO_MEM;
    JS_SetPropertyStr(ctx,ns,"GUEST",JS_NewInt32(ctx,POCKET_MEMORY_GUEST));
    JS_SetPropertyStr(ctx,ns,"FREE",JS_NewInt32(ctx,POCKET_MEMORY_FREE));
    JS_SetPropertyStr(ctx,ns,"LARGEST",JS_NewInt32(ctx,POCKET_MEMORY_LARGEST));
    JS_SetPropertyStr(ctx,ns,"FAILURE",JS_NewInt32(ctx,POCKET_MEMORY_FAILURE));
    return JS_HasException(ctx)?ESP_ERR_NO_MEM:ESP_OK;
}

esp_err_t pocket_memory_install(JSContext *ctx,void *user) {
    (void)user;
    pressure=(pressure_state){.ctx=ctx,.listener=JS_UNDEFINED};
    atomic_store(&native_failed,0);
    atomic_store(&native_enabled,true);
#if defined(ESP_PLATFORM) && !defined(CONFIG_POCKET_VM_OOMPROBE)
    static bool hook_registered;
    if(!hook_registered) {
        if(heap_caps_register_failed_alloc_callback(alloc_failed)!=ESP_OK) {
            pocket_memory_reset();
            return ESP_FAIL;
        }
        hook_registered=true;
    }
#endif
    esp_err_t err=pocket_api_lazy(ctx,"memory",build_memory,NULL);
    if(err!=ESP_OK)pocket_memory_reset();
    return err;
}

void pocket_memory_reset(void) {
    atomic_store(&native_enabled,false);
    if(pressure.ctx&&pressure.open)close_listener(pressure.ctx,pressure.listener_id);
    pressure=(pressure_state){.listener=JS_UNDEFINED};
    atomic_store(&native_failed,0);
}
