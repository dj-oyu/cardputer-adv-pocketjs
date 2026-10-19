#include "derby_measure.h"
#include "esp_err.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int64_t clock_us;
static unsigned writes;
static int refuse, wide_snapshot, expect_overflow;
int __real_dm_read(dm_snapshot *out);
int __wrap_dm_read(dm_snapshot *out){
    if(wide_snapshot){memset(out,0xff,sizeof *out);return 1;}
    return __real_dm_read(out);
}
int64_t esp_timer_get_time(void){return clock_us;}
esp_err_t usb_console_frame(const void *data,size_t n){
    assert(n<=1024&&n>0&&((const char *)data)[n-1]=='\n');
    assert(!memcmp(data,"DERBY_MEASURE v=1 ",18));
    if(expect_overflow)assert(strstr((const char *)data," overflow=1\n"));
    writes++;return refuse?ESP_FAIL:ESP_OK;
}
int main(void){
    dm_snapshot wide;memset(&wide,0xff,sizeof wide);
    char line[1024];int overflow=0;
    size_t n=dm_format_record(line,sizeof line,UINT32_MAX,UINT64_MAX,&wide,&overflow);
    assert(overflow&&n<=1024&&n>0&&line[n-1]=='\n'&&strstr(line,"overflow=1"));
    dm_snapshot zero={0};
    n=dm_format_record(line,sizeof line,300,UINT64_MAX,&zero,&overflow);
    assert(!overflow&&n<=1024&&n>0&&line[n-1]=='\n');
    char short_line[16];
    assert(!dm_format_record(short_line,sizeof short_line,300,UINT64_MAX,&wide,&overflow)&&overflow);
    dm_reset();dm_output_reset();
    for(unsigned i=0;i<100;i++)dm_emit(12,100);
    assert(!writes);
    for(unsigned i=0;i<350;i++){
        clock_us+=1000000;dm_emit(12,100);dm_emit(12,100);
    }
    assert(writes==300);
    // The cap was reached 50 seconds ago; a new session may emit once.
    dm_reset();dm_output_reset();dm_emit(12,100);assert(writes==301);
    // Rapid re-evaluation must preserve the boot-global last attempt.
    dm_reset();dm_output_reset();dm_emit(12,100);assert(writes==301);
    clock_us+=999999;dm_emit(12,100);assert(writes==301);
    clock_us+=1;refuse=1;dm_emit(12,100);
    dm_snapshot s;assert(dm_read(&s)&&s.output_refused==1&&writes==302);
    assert(s.stage[DM_OBSERVER].calls==1&&s.stage[DM_OUTPUT].calls==1);
    dm_reset();dm_output_reset();refuse=0;wide_snapshot=expect_overflow=1;
    clock_us+=1000000;dm_emit(12,100);
    wide_snapshot=expect_overflow=0;
    assert(dm_read(&s)&&s.output_refused==1&&writes==303);
    puts("PASS USB <=1024, maximum-width overflow, short buffer, 1Hz/300 cap, reset/refusal/nesting");
}
