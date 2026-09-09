// The card-touching half of fs.volume.sd. Everything that can be decided
// without a card lives in sd_path.c and is host-tested; this file is kept thin
// on purpose, because it is the part no test can reach.
//
// Measured on the board 2026-09-08, with the probe that established a card
// clocks here at all: a mount costs 7,032 bytes and two open handles a further
// 1,640, all of it heap, none of it charged to an app that never touches the
// card. unmount returns all of it (leak 0 across three cycles).
#include "sd_media.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

// docs/hardware-constraints.md:45. The bus itself belongs to board.c; this
// file only ever adds a device to it.
#define SD_CS 12

static const char *TAG = "sd";
static sd_media_t media;
static sdmmc_card_t *card;

void sd_media_init(void) { sd_media_reset(&media); }

const sd_media_t *sd_media(void) { return &media; }

bool sd_media_mount(void) {
    if (media.state == SD_MEDIA_READY) return true;
    if (board_spi3_acquire() != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 unavailable");
        sd_media_failed(&media);
        return false;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS;
    slot.host_id = SPI3_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    // 400 kHz was the IDENTIFICATION clock, kept as the transfer clock. That
    // was never a considered choice about this card: it is the frequency every
    // SD card is required to answer at before the host raises it, and nothing
    // here ever raised it. The arithmetic that used to be written out in this
    // comment was therefore describing a card running fifty times slower than
    // it can, and drawing conclusions from it:
    //
    //   400 kbit/s is about 50,000 bytes a second before per-command overhead,
    //   so ONE 2,048-byte refill takes roughly 41 ms -- longer than the 33 ms
    //   frame that asked for it. Streamed playback off the card could not have
    //   worked at any bitrate, and it did not: it came out in pieces, which is
    //   the shape a ring starved once per refill makes. The old comment read
    //   that as "the card is too slow for realtime PCM". The card was fine.
    //
    // 20 MHz IS THE CEILING HERE, and the reason is not the SPI clock. Both
    // 30 MHz and 40 MHz were tried on the board on 2026-09-09 and both failed
    // the same way, before any data moved:
    //
    //   E sdmmc_sd: sdmmc_enable_hs_mode_and_check: send_csd returned 0x108
    //   W sd: mount failed: ESP_ERR_INVALID_RESPONSE
    //
    // sdmmc_sd.c:522 skips the high-speed switch only when max_freq_khz is
    // <= SDMMC_FREQ_DEFAULT, so ANY value above 20 MHz makes the driver attempt
    // HS mode, and here the CMD9 that follows the switch does not answer. The
    // failure is returned rather than mapped to ESP_ERR_NOT_SUPPORTED, so the
    // driver's own "card has no HS mode, fall back to 20 MHz" path never runs
    // and the whole mount fails. 20,001 kHz would fail exactly like 40,000.
    //
    // The faster grades sdmmc.h names (SDR50, DDR50, 52M) are not candidates at
    // all: they belong to the 4-bit SD interface and cannot come out of a
    // one-wire SPI link.
    //
    // Asking was safe in a way it would not have been on a write-only bus. SPI
    // mode carries a CRC16 on every data block and the driver checks it, so a
    // clock this wiring could not hold showed up as a refusal rather than as
    // quiet corruption -- and the refusal surfaced correctly all the way out,
    // as DISCONNECTED from fs.requestFolder. MISO being wired here is what
    // makes that possible; the LCD has no such check available to it.
    //
    // card->max_freq_khz in the mount log below is what was actually agreed.
    // Read that line rather than this constant when the number matters.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        // docs/filesystem-api.md:56 -- never format on mount failure. A card
        // that does not mount is a card with someone's data on it until proven
        // otherwise, and this API has no way to ask.
        .format_if_mount_failed = false,
        .max_files = SD_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
    };

    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
    if (e != ESP_OK) {
        card = NULL;
        // ESP_ERR_TIMEOUT / NOT_FOUND is an empty slot; ESP_FAIL is a card that
        // answered and would not mount. Section 3 keeps those apart, because
        // one of them is worth telling the person about and the other is just
        // "no card".
        if (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT) sd_media_removed(&media);
        else sd_media_failed(&media);
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(e));
        return false;
    }

    sd_media_mounted(&media);
    ESP_LOGI(TAG, "mounted generation=%u %llu MB sector=%u %d kHz",
             (unsigned)media.generation,
             ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20,
             (unsigned)card->csd.sector_size, card->max_freq_khz);
    return true;
}

void sd_media_unmount(void) {
    if (card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        card = NULL;
    }
    // Removal, not failure: the grant goes with it, so a remount cannot silently
    // reconnect an app to a folder the person authorised on a different card.
    sd_media_removed(&media);
}

bool sd_media_grant_folder(const char *folder, size_t len) {
    return sd_media_grant(&media, folder, len);
}

void sd_media_note_error(int err) {
    // There is NO card-detect pin in the pin map, so removal is not an event
    // this firmware can receive -- it is only ever the shape of a failed
    // command. That is why available is an observation made when something is
    // attempted, and why nothing here polls: polling would invent a liveness
    // the hardware cannot report.
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "card stopped answering (%d); volume disconnected", err);
        if (card) { esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card); card = NULL; }
        sd_media_removed(&media);
    }
}

bool sd_media_space(uint64_t *capacity, uint64_t *freebytes) {
    if (media.state != SD_MEDIA_READY || !card) return false;
    FATFS *fs = NULL;
    DWORD clusters = 0;
    // f_getfree touches the medium, which section 3 permits for space() and
    // forbids for volumes().
    if (f_getfree(SD_MOUNT_POINT, &clusters, &fs) != FR_OK || !fs) {
        sd_media_note_error(ESP_ERR_TIMEOUT);
        return false;
    }
    uint64_t sector = card->csd.sector_size;
    *capacity  = (uint64_t)(fs->n_fatent - 2) * fs->csize * sector;
    *freebytes = (uint64_t)clusters * fs->csize * sector;
    return true;
}
