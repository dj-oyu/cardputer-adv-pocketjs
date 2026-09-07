#include "skk_session.h"
#include "skk_core.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

// skk_t is 1.2 KB and ime_t wraps it; both live here as statics rather than on
// a task stack, which the engine's contract allows (it allocates nothing and
// the caller places the struct wherever it likes).
static ime_t     session;
static skk_dict_t dict;
static bool      ready;
static const char *status = "NOT INITIALISED";
static int64_t   open_us, probe_us;

bool skk_session_ready(void) { return ready; }
ime_t *skk_session(void) { return &session; }
const char *skk_session_status(void) { return status; }
// Boot logs scroll past before a host can attach over USB Serial/JTAG, so the
// open and first-lookup costs are kept for whoever asks later.
void skk_session_timing(int64_t *open, int64_t *probe) { *open=open_us; *probe=probe_us; }

bool skk_session_init(void) {
    ime_init(&session);
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "skk_dict");
    if(!p) { status="NO PARTITION"; ESP_LOGW("skk","no skk_dict partition"); return false; }

    const void *base=NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err=esp_partition_mmap(p,0,p->size,ESP_PARTITION_MMAP_DATA,&base,&handle);
    if(err!=ESP_OK) {
        status="MMAP FAILED";
        ESP_LOGW("skk","mmap failed: %s",esp_err_to_name(err));
        return false;
    }

    // The image may be shorter than the partition; skk_dict_open compares
    // against its own header length, so the whole window can be handed over.
    //
    // VERIFY walks the whole payload: 36 ms for M's 303 KB here, and SKK-JISYO.ML
    // is six times that. It earns its cost once, right after the partition is
    // written, and nothing after that changes the bytes — so the image_len and
    // payload_crc32 out of the header are remembered, and a boot that finds the
    // same pair skips the walk. Without VERIFY the open still checks magic,
    // version, alignment, section bounds and the alphabet stamp, so a corrupt
    // image is refused rather than searched.
    uint32_t image_len, payload_crc;
    memcpy(&image_len,(const uint8_t*)base+8,4);
    memcpy(&payload_crc,(const uint8_t*)base+12,4);

    nvs_handle_t prefs;
    bool have_prefs=nvs_open("skk",NVS_READWRITE,&prefs)==ESP_OK;
    uint32_t seen_len=0, seen_crc=0;
    if(have_prefs) {
        nvs_get_u32(prefs,"len",&seen_len);
        nvs_get_u32(prefs,"crc",&seen_crc);
    }
    bool verified = have_prefs && seen_len==image_len && seen_crc==payload_crc;

    int64_t t0=esp_timer_get_time();
    skk_blob_t blob={.base=base,.len=p->size};
    int rc=skk_dict_open(&dict,blob,verified?0:SKK_OPEN_VERIFY);
    open_us=esp_timer_get_time()-t0;

    if(have_prefs) {
        if(rc==SKK_OK && !verified) {
            nvs_set_u32(prefs,"len",image_len);
            nvs_set_u32(prefs,"crc",payload_crc);
            nvs_commit(prefs);
        }
        nvs_close(prefs);
    }
    if(rc!=SKK_OK) {
        status="DICT INVALID";
        ESP_LOGW("skk","skk_dict_open failed: %d",rc);
        return false;
    }

    ime_attach(&session,&dict);
    ready=true;
    status="READY";
    ESP_LOGI("skk","dict open in %lld us (%s); nasi=%u ari=%u",
             open_us,verified?"cached":"verified",
             (unsigned)dict.blk[SKK_BLK_NASI].count,
             (unsigned)dict.blk[SKK_BLK_ARI].count);

    // One lookup on a known reading, so a dictionary that opens but searches
    // wrong is visible in the boot log rather than at the first keystroke.
    skk_cand_t probe[4];
    size_t n=0;
    t0=esp_timer_get_time();
    if(skk_lookup(&dict,SKK_BLK_NASI,"かんじ",9,probe,4,&n)==SKK_OK && n) {
        probe_us=esp_timer_get_time()-t0;
        size_t len=0;
        const char *text=skk_cand_text(&dict,&probe[0],&len);
        ESP_LOGI("skk","probe かんじ -> %.*s (%u cands, %lld us)",
                 (int)len,text,(unsigned)n,probe_us);
    } else {
        ESP_LOGW("skk","probe かんじ found nothing");
    }
    return true;
}
