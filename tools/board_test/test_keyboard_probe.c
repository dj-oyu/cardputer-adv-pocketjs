#include "keyboard_probe.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    esp_err_t errors[3];
    int64_t duration[3];
    unsigned calls, waits;
    esp_err_t result;
} scenario;
static const scenario cases[] = {
    {"ready", {ESP_OK}, {0}, 1, 0, ESP_OK},
    {"transient NACK then timeout", {ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_OK},
        {1000,1000,1000}, 3, 2, ESP_OK},
    {"persistent NACK", {ESP_ERR_NOT_FOUND,ESP_ERR_NOT_FOUND,ESP_ERR_NOT_FOUND},
        {1000,1000,1000}, 3, 2, ESP_ERR_NOT_FOUND},
    {"persistent timeout", {ESP_ERR_TIMEOUT,ESP_ERR_TIMEOUT,ESP_ERR_TIMEOUT},
        {1000,1000,1000}, 3, 2, ESP_ERR_TIMEOUT},
    {"invalid argument", {ESP_ERR_INVALID_ARG}, {0}, 1, 0, ESP_ERR_INVALID_ARG},
    {"unexpected error", {0x7abc}, {0}, 1, 0, 0x7abc},
    {"window reached by call", {ESP_ERR_TIMEOUT}, {250000}, 1, 0, ESP_ERR_TIMEOUT},
    {"window reached by gap", {ESP_ERR_NOT_FOUND}, {240000}, 1, 1, ESP_ERR_NOT_FOUND},
    {"one us before window", {ESP_ERR_NOT_FOUND,ESP_OK}, {239999,0}, 2, 1, ESP_OK},
};
static const scenario *current;
static int64_t now, began;
static unsigned probes, waits, warnings, successes, failures;
static char logs[4096];
static char failure_summary[512];

int64_t esp_timer_get_time(void) { return now; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout) {
    assert(bus==(void*)0x1234 && addr==0x34 && timeout==50);
    assert(probes<3 && probes<current->calls);
    assert(now-began<250000);
    now+=current->duration[probes];
    return current->errors[probes++];
}
void vTaskDelay(TickType_t ticks) {
    assert(ticks>0);
    assert((uint64_t)ticks*1000000/configTICK_RATE_HZ==10000);
    ++waits; now+=(uint64_t)ticks*1000000/configTICK_RATE_HZ;
}
int gpio_get_level(int gpio) { assert(gpio==8||gpio==9); return gpio==9; }
void test_log(char level,const char *tag,const char *format,...) {
    assert(!strcmp(tag,"board"));
    char row[512]; va_list ap; va_start(ap,format);
    vsnprintf(row,sizeof row,format,ap); va_end(ap);
    assert(strlen(logs)+strlen(row)+2<sizeof logs);
    strcat(logs,row);strcat(logs,"\n");
    if(level=='W') {
        ++warnings;
        char expected[32];snprintf(expected,sizeof expected,"(0x%x)",
                                  (unsigned)current->errors[probes-1]);
        assert(strstr(row,expected) && strstr(row,"sda=0 scl=1"));
    } else if(level=='I') ++successes;
    else { assert(level=='E');++failures;snprintf(failure_summary,sizeof failure_summary,"%s",row); }
}
int main(void) {
    for(unsigned i=0;i<sizeof cases/sizeof *cases;++i) {
        current=&cases[i];now=began=1000000;
        probes=waits=warnings=successes=failures=0;logs[0]=0;failure_summary[0]=0;
        esp_err_t got=keyboard_probe_startup((void*)0x1234);
        assert(got==current->result && probes==current->calls && waits==current->waits);
        assert(warnings==probes-(got==ESP_OK));
        assert(successes==(got==ESP_OK) && failures==(got!=ESP_OK));
        assert(warnings+successes+failures<=4);
        if(got!=ESP_OK) {
            char expected[64];snprintf(expected,sizeof expected,"error=%s(0x%x)",
                                      esp_err_to_name(got),(unsigned)got);
            assert(strstr(failure_summary,expected));
        }
        printf("PASS %s probes=%u waits=%u result=0x%x\n",current->name,
               probes,waits,(unsigned)got);
    }
    return 0;
}
