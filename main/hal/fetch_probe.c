#include "sdkconfig.h"
#include "fetch_probe.h"

#if CONFIG_POCKET_FETCH_PROBE
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp32s3/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Sizes must match tools/gen_fetch_probe.py.
#define SEQ_INSNS    512
#define CHAIN_LINES  48
#define CHAIN_INSNS  (CHAIN_LINES*9)       // 8 add + 1 j per line

extern void fp_seq_iram(void), fp_seq_rtc(void), fp_seq_flash(void);
extern void fp_chain_iram(void), fp_chain_rtc(void), fp_chain_flash(void);

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Interrupts off for the measurement: a tick landing inside would be counted.
static void measure(const char *kind, const char *mem, void (*volatile f)(void),
                    unsigned insns, unsigned lines) {
    uint32_t cold, warm=0xffffffffu;
    portENTER_CRITICAL(&mux);
    Cache_Invalidate_ICache_All();
    uint32_t t0=esp_cpu_get_cycle_count();
    f();
    cold=esp_cpu_get_cycle_count()-t0;
    for(int i=0;i<4;i++) {
        t0=esp_cpu_get_cycle_count();
        f();
        uint32_t d=esp_cpu_get_cycle_count()-t0;
        if(d<warm) warm=d;
    }
    portEXIT_CRITICAL(&mux);
    ESP_LOGI("fetchprobe","FETCHPROBE kind=%s mem=%s addr=%p insns=%u lines=%u cold=%u warm=%u "
             "warm_x100_per_insn=%u miss_x10_per_line=%d",
             kind,mem,(void*)f,insns,lines,(unsigned)cold,(unsigned)warm,
             (unsigned)(warm*100u/insns),
             (int)(((int)cold-(int)warm)*10/(int)lines));
}

void fetch_probe_run(void) {
    // Three rounds in a different order each time, so that a layout effect
    // shows up as a difference between rounds and not between memories.
    for(int round=0;round<3;round++) {
        measure("seq","iram",fp_seq_iram,SEQ_INSNS,SEQ_INSNS*3/32);
        measure("seq","rtc",fp_seq_rtc,SEQ_INSNS,SEQ_INSNS*3/32);
        measure("seq","flash",fp_seq_flash,SEQ_INSNS,SEQ_INSNS*3/32);
        measure("chain","iram",fp_chain_iram,CHAIN_INSNS,CHAIN_LINES);
        measure("chain","rtc",fp_chain_rtc,CHAIN_INSNS,CHAIN_LINES);
        measure("chain","flash",fp_chain_flash,CHAIN_INSNS,CHAIN_LINES);
    }
    ESP_LOGI("fetchprobe","FETCHPROBE_DONE");
}
#else
void fetch_probe_run(void) {}
#endif
