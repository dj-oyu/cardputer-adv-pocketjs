// MEASUREMENT HARNESS -- worktree only, never committed, not the mount.
//
// v2. The first version reported free heap at four points during app_main and
// produced identical numbers for two configurations, with free heap RISING
// 17,316 bytes across the mount. Free heap going up across an allocation is not
// a mount cost; boot was still settling underneath the readings. Two separate
// faults, and it is worth naming both because only one was the instrument:
//
//   1. The readings were taken while other subsystems were still freeing.
//   2. The two builds genuinely had the same FF_MAX_SS, so identical numbers
//      were CORRECT. FF_MAX_SS is MAX(FF_SS_SDCARD=512, CONFIG_WL_SECTOR_SIZE),
//      and CONFIG_FATFS_SECTOR_* -- which is what I changed -- only configures
//      the partition image generator.
//
// So: quiesce first, measure the mount call and nothing else, prove the mount
// returns the memory on unmount, and print a fingerprint that makes two builds
// distinguishable in the FIRST line rather than after two flashes.
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"

// docs/hardware-constraints.md:45. Shared with the EXT bus; LCD is wired apart.
#define SD_CS   12
#define SD_MOSI 14
#define SD_CLK  40
#define SD_MISO 39
#define SD_MOUNT "/sdprobe"
#define SD_MAX_FILES 2          // matches FS_MAX_HANDLES

static unsigned freeb(void) {
    return (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Boot keeps freeing for a while after app_main returns. Wait for free heap to
// stop moving instead of guessing a delay, and report how long that took so the
// next reader knows whether the wait was real.
static unsigned quiesce(void) {
    unsigned last = freeb(), stable = 0, ms = 0;
    while (stable < 8 && ms < 8000) {
        vTaskDelay(pdMS_TO_TICKS(100)); ms += 100;
        unsigned now = freeb();
        stable = (now > last ? now - last : last - now) < 64 ? stable + 1 : 0;
        last = now;
    }
    ESP_LOGI("sdprobe", "SDPROBE quiesced after %u ms free=%u", ms, last);
    return last;
}

static void sd_probe_task(void *arg) {
    (void)arg;
    // FINGERPRINT FIRST. Two builds that differ in sector sizing must be
    // distinguishable here, before any measurement, or an identical result is
    // ambiguous between "no effect" and "same build twice".
    ESP_LOGI("sdprobe",
             "SDPROBE build FF_MAX_SS=%d FF_MIN_SS=%d WL_SECTOR=%d "
             "sizeof_FATFS=%u sizeof_FIL=%u max_files=%d",
             (int)FF_MAX_SS, (int)FF_MIN_SS, (int)CONFIG_WL_SECTOR_SIZE,
             (unsigned)sizeof(FATFS), (unsigned)sizeof(FIL), SD_MAX_FILES);

    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI, .miso_io_num = SD_MISO, .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE("sdprobe", "SDPROBE bus_init failed"); vTaskDelete(NULL); return;
    }

    unsigned base = quiesce();

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS; slot.host_id = SPI3_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST; host.max_freq_khz = 400;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,     // filesystem-api.md:56
        .max_files = SD_MAX_FILES, .allocation_unit_size = 0,
    };

    // Three cycles. The mount cost is A-B with NOTHING between the two reads;
    // A-C says whether unmount gives it all back, which is the check that says
    // the instrument is trustworthy at all.
    for (int i = 0; i < 3; i++) {
        sdmmc_card_t *card = NULL;
        unsigned a = freeb();
        esp_err_t e = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &cfg, &card);
        unsigned b = freeb();
        if (e != ESP_OK) {
            ESP_LOGE("sdprobe", "SDPROBE mount err=0x%x (%s)", e, esp_err_to_name(e));
            break;
        }
        if (i == 0) {
            ESP_LOGI("sdprobe", "SDPROBE card name=%s size_MB=%llu sector=%u speed_kHz=%d",
                     card->cid.name,
                     ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20,
                     (unsigned)card->csd.sector_size, card->max_freq_khz);
            DIR *d = opendir(SD_MOUNT);
            int n = 0;
            if (d) { struct dirent *de;
                while ((de = readdir(d)) && n < 8) {
                    ESP_LOGI("sdprobe", "SDPROBE entry %s type=%d", de->d_name, de->d_type); n++; }
                closedir(d); }
            ESP_LOGI("sdprobe", "SDPROBE listed=%d", n);
        }
        // The half the first test never exercised. FIL.buf[] is where FF_MAX_SS
        // would show if it shows anywhere, and two open handles is the state the
        // firmware will actually be in (FS_MAX_HANDLES == 2). Files are created
        // and removed again so the card is left as it was found.
        unsigned f_before = freeb();
        FILE *fp[SD_MAX_FILES] = {0};
        char path[64];
        for (int k = 0; k < SD_MAX_FILES; k++) {
            snprintf(path, sizeof path, SD_MOUNT "/probe%d.tmp", k);
            fp[k] = fopen(path, "wb");
            if (fp[k]) fwrite("pocketjs", 1, 8, fp[k]);
        }
        unsigned f_open2 = freeb();
        ESP_LOGI("sdprobe", "SDPROBE open2 cost=%d opened=%d%d",
                 (int)f_before - (int)f_open2, fp[0] != NULL, fp[1] != NULL);
        for (int k = 0; k < SD_MAX_FILES; k++) {
            if (!fp[k]) continue;
            fclose(fp[k]);
            snprintf(path, sizeof path, SD_MOUNT "/probe%d.tmp", k);
            remove(path);
        }
        ESP_LOGI("sdprobe", "SDPROBE after_close=%d", (int)f_before - (int)freeb());

        unsigned during = freeb();
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
        unsigned c = freeb();
        ESP_LOGI("sdprobe",
                 "SDPROBE cycle=%d mount_cost=%d after_list=%d unmount_leak=%d",
                 i, (int)a - (int)b, (int)a - (int)during, (int)a - (int)c);
    }
    ESP_LOGI("sdprobe", "SDPROBE base=%u end=%u", base, freeb());
    vTaskDelete(NULL);
}

void sd_probe(void) {
    xTaskCreatePinnedToCore(sd_probe_task, "sdprobe", 5120, NULL, 4, NULL, 0);
}
