// I-cache auto-preload A/B probe: icache_ab.h.
//
// The ESP32-S3's ICache can fetch the blocks after the one it hits or misses
// on (TRM v1.8 p.406, "Auto-Preload"), but only inside one of two address
// sections, and the boot leaves it off. The JS turn waits on the flash for
// most of its cycles (docs/vm/turn-cpi.md), so this measures whether turning
// it on over the firmware's flash text shortens the turn. One binary rotates
// the modes window by window: builds move the same code by up to 15% through
// instruction-cache alignment, so the comparison has to stay in one image.
//
// The ROM's autoload calls are documented as internal to ESP-IDF. They only
// write the EXTMEM autoload registers; the cache itself stays enabled.
#include "icache_ab.h"
#ifdef CONFIG_POCKET_ICACHE_AB
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "soc/soc.h"
#include "soc/extmem_reg.h"
#include "esp32s3/rom/cache.h"

extern int _instruction_reserved_start,_instruction_reserved_end;

typedef struct { const char *name; uint8_t ena,trigger,size; } ab_mode;
/* size: the autoload length field (2 bits, CACHE_AUTOLOAD_STEP). */
static const ab_mode MODES[]={
    {"off",  0,CACHE_AUTOLOAD_MISS_TRIGGER,0},
    {"miss0",1,CACHE_AUTOLOAD_MISS_TRIGGER,0},
    {"miss3",1,CACHE_AUTOLOAD_MISS_TRIGGER,3},
    {"both0",1,CACHE_AUTOLOAD_BOTH_TRIGGER,0},
};
#define NMODES (sizeof MODES/sizeof *MODES)
static unsigned cur,windows;
static bool ready;
static uint32_t resets;

static void log_regs(const char *why){
    ESP_LOGI("icache","ICACHE_AB %s ctrl=0x%03x sct0=0x%08x/%u sct1=0x%08x/%u text=0x%08x..0x%08x",why,
             (unsigned)REG_READ(EXTMEM_ICACHE_AUTOLOAD_CTRL_REG),
             (unsigned)REG_READ(EXTMEM_ICACHE_AUTOLOAD_SCT0_ADDR_REG),
             (unsigned)REG_READ(EXTMEM_ICACHE_AUTOLOAD_SCT0_SIZE_REG),
             (unsigned)REG_READ(EXTMEM_ICACHE_AUTOLOAD_SCT1_ADDR_REG),
             (unsigned)REG_READ(EXTMEM_ICACHE_AUTOLOAD_SCT1_SIZE_REG),
             (unsigned)(uintptr_t)&_instruction_reserved_start,
             (unsigned)(uintptr_t)&_instruction_reserved_end);
}
/* In IRAM: reconfiguring the preloader while fetching through it is what
 * the ROM's own callers avoid. */
static IRAM_ATTR void apply(const ab_mode *m){
    Cache_Disable_ICache_Autoload();
    if(!m->ena)return;
    const struct autoload_config c={1,CACHE_AUTOLOAD_POSITIVE,m->trigger,m->size};
    Cache_Config_ICache_Autoload(&c);
    Cache_Enable_ICache_Autoload();
}
static void setup(void){
    log_regs("boot");
    const uint32_t a=(uint32_t)(uintptr_t)&_instruction_reserved_start;
    const uint32_t n=(uint32_t)(uintptr_t)&_instruction_reserved_end-a;
    const struct autoload_region_config r={CACHE_AUTOLOAD_REGION0,1,a,n};
    const int rc=Cache_Config_ICache_Region_Autoload(&r);
    ESP_LOGI("icache","ICACHE_AB region rc=%d",rc);
    apply(&MODES[cur]);
    log_regs("set");
    ready=true;
}
void icache_ab_turn(void){
    if(!ready)setup();
    const bool on=(REG_READ(EXTMEM_ICACHE_AUTOLOAD_CTRL_REG)&EXTMEM_ICACHE_AUTOLOAD_ENA)!=0;
    if(on!=(MODES[cur].ena!=0)){resets++;apply(&MODES[cur]);}
}
void icache_ab_window(void){
    if(!ready)return;
    ESP_LOGI("icache","ICACHE_AB win=%u mode=%s resets=%u",windows,MODES[cur].name,(unsigned)resets);
    windows++;
    cur=(cur+1u)%NMODES;
    apply(&MODES[cur]);
}
#endif
