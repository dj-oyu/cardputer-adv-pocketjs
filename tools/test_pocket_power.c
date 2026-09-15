#include "pocket_power.h"
#include "pocket_api.h"
#include "system/sys_device.h"
#include "board.h"
#include "esp_app_desc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int64_t now;
static unsigned reads;
static int mv=4000;
static bool available=true;
int64_t esp_timer_get_time(void){return now;}
bool board_battery_read(board_battery_t *out){reads++;*out=(board_battery_t){mv,now};return available;}
static const esp_app_desc_t description={.version="host"};
const esp_app_desc_t *esp_app_get_description(void){return &description;}
static void eval(JSContext *ctx,const char *script){
    JSValue v=JS_Eval(ctx,script,strlen(script),"power-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"%s\n",s?s:"exception");JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);assert(0);}
    JS_FreeValue(ctx,v);
}
int main(void){
    for(unsigned session=0;session<30;session++){
        now+=2000000;mv=4000;available=true;
        JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);assert(rt&&ctx);
        assert(pocket_api_install(ctx,NULL)==ESP_OK&&pocket_power_install(ctx)==ESP_OK);
        unsigned before=reads;pocket_power_pump();assert(reads==before);
        eval(ctx,"function check(v){if(!v)throw Error('power contract');}"
            "let n=0,m=0,last; let a=pocket.power.onChange(v=>{n++;last=v;});"
            "let b=pocket.power.onChange(v=>m++);"
            "try{pocket.power.onChange(()=>{});throw Error('accepted third');}catch(e){check(e.code==='LIMIT_EXCEEDED');}");
        assert(sys_device_state()->power_users==1);
        pocket_power_pump();assert(reads==before+1);eval(ctx,"check(n===1&&m===1&&last.millivolts===4000&&last.percent===null&&last.charging===null);");
        before=reads;for(int i=0;i<100;i++)pocket_power_pump();assert(reads==before);
        now+=1000000;mv=4010;pocket_power_pump();eval(ctx,"check(n===1&&m===1);");
        now+=1000000;mv=4020;pocket_power_pump();eval(ctx,"check(n===2&&m===2);");
        eval(ctx,"a.close();a=pocket.power.onChange(v=>{n++;last=v;});");
        before=reads;pocket_power_pump();assert(reads==before);eval(ctx,"check(n===3&&m===2);");
        eval(ctx,"a.close();b.close();");assert(!sys_device_state()->power_users&&sys_power_deadline(sys_device_state())==SYS_NEVER);
        now+=60000000;before=reads;sys_device_step();pocket_power_pump();assert(reads==before);
        eval(ctx,"let bad=pocket.power.onChange(()=>{throw Error('listener');});");pocket_power_pump();
        assert(!sys_device_state()->power_users);
        sys_sub occupied[8];
        for(int i=0;i<8;i++)assert(sys_subscribe(sys_device_state(),SYS_POWER,&occupied[i])==SYS_OK);
        eval(ctx,"try{pocket.power.onChange(()=>{});throw Error('accepted full');}catch(e){check(e.code==='LIMIT_EXCEEDED');}");
        for(int i=0;i<8;i++)assert(sys_unsubscribe(sys_device_state(),occupied[i])==SYS_OK);
        eval(ctx,"let live=pocket.power.onChange(()=>{}); check(pocket.power.status().millivolts===4020);");
        sys_sub native;assert(sys_subscribe(sys_device_state(),SYS_POWER,&native)==SYS_OK);
        pocket_power_reset();assert(sys_device_state()->power_users==1);
        assert(sys_unsubscribe(sys_device_state(),native)==SYS_OK);
        pocket_api_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
        assert(!sys_device_state()->power_users&&sys_power_deadline(sys_device_state())==SYS_NEVER);
    }
    puts("POCKET_POWER PASS: shared subscription, ADC cadence, fanout, close/throw, 30 sessions");
}
