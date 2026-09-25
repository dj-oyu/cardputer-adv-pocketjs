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
#include "esp_heap_caps.h"
#include <string.h>
#include <errno.h>
#ifdef KASANE_P0_PROBE
#include "esp_timer.h"
#endif

// docs/platform/hardware-constraints.md:45. The bus itself belongs to board.c; this
// file only ever adds a device to it.
#define SD_CS 12

static const char *TAG = "sd";
static sd_media_t media;
static sdmmc_card_t *card;
static sd_lease_registry_t leases;

void sd_media_init(void) {
    sd_media_reset(&media);
    sd_lease_registry_reset(&leases);
}

const sd_media_t *sd_media(void) { return &media; }

// Walk one step back UP, and PROVE it. sd_path.h says why the mount cannot ask
// for this rate itself; what it cannot say is that raising a clock behind a
// filesystem's back is only safe if something checks, because no handshake is
// involved and a card that cannot hold the rate does not announce it.
//
// The check is a real read of a real sector, compared against the same sector
// read at the rate that already worked. Sector 0 is the MBR: it exists on every
// card this firmware can mount, it does not move, and reading it costs one
// command. SPI mode CRCs every data block on top of that, so a mismatch here is
// two independent failures rather than a coincidence.
//
// Failure is not an error. It means the card is staying where it was, which is
// where it would have been without this function.
static void raise_clock(void) {
    if (!card) return;
    int want = sd_clock_boost(card->max_freq_khz);
    if (!want) return;
    int was = card->max_freq_khz;

    // One allocation, two halves, DMA-capable because the driver reads into it.
    uint8_t *ref = heap_caps_malloc(2 * 512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!ref) return;                       // no room to check means no raise
    uint8_t *again = ref + 512;

    esp_err_t e = sdmmc_read_sectors(card, ref, 0, 1);
    if (e != ESP_OK) { free(ref); return; }

    if ((*card->host.set_card_clk)(card->host.slot, (uint32_t)want) != ESP_OK) {
        free(ref);
        return;
    }
    e = sdmmc_read_sectors(card, again, 0, 1);
    if (e == ESP_OK && memcmp(ref, again, 512) == 0) {
        card->max_freq_khz = want;
        ESP_LOGI(TAG, "raised %d -> %d kHz and verified a sector at it",
                 was, want);
    } else {
        // Back to what worked, and say which of the two ways it failed: a read
        // that errored and a read that came back different are different
        // stories about the card.
        (*card->host.set_card_clk)(card->host.slot, (uint32_t)was);
        ESP_LOGI(TAG, "%d kHz did not hold (%s); staying at %d kHz", want,
                 e != ESP_OK ? esp_err_to_name(e) : "sector read back different",
                 was);
    }
    free(ref);
}

bool sd_media_mount(void) {
    if (media.state == SD_MEDIA_READY) return true;
    sd_media_service();
    // An acknowledged worker is necessary before another card can be mounted.
    // In particular, do not put a new VFS under a quarantined old FILE*.
    if (leases.pending_unmount) return false;
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
    // The clock is NOT set here. sd_path.h holds the ladder and the reasons;
    // this file walks it, because walking it means touching a card.
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        // docs/api/filesystem-api.md:56 -- never format on mount failure. A card
        // that does not mount is a card with someone's data on it until proven
        // otherwise, and this API has no way to ask.
        .format_if_mount_failed = false,
        .max_files = SD_MAX_OPEN_FILES,
        .allocation_unit_size = 0,
    };

    // The walk. Every failed attempt cleans up after itself -- esp-idf's mount
    // deinits the host on its way out (vfs_fat_sdmmc.c:403) -- so a retry is a
    // fresh attempt and not a leaked device handle per rung.
    esp_err_t e = ESP_FAIL;
    int khz = SD_CLOCK_LADDER[0];
    sd_mount_outcome_t outcome = SD_MOUNT_REFUSED;
    while (khz) {
        host.max_freq_khz = khz;
        e = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
        if (e == ESP_OK) break;
        card = NULL;
        // ESP_ERR_TIMEOUT / NOT_FOUND is an empty slot; anything else is
        // something that answered and would not mount at THIS rate. Section 3
        // keeps those apart because one is worth telling the person about and
        // the other is just "no card" -- and here it also decides whether a
        // slower rung could possibly help.
        outcome = (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_TIMEOUT)
                  ? SD_MOUNT_ABSENT : SD_MOUNT_REFUSED;
        int next = sd_clock_next(khz, outcome);
        if (next)
            ESP_LOGI(TAG, "%d kHz refused (%s), trying %d kHz",
                     khz, esp_err_to_name(e), next);
        khz = next;
    }
    if (e != ESP_OK) {
        if (outcome == SD_MOUNT_ABSENT) sd_media_removed(&media);
        else sd_media_failed(&media);
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(e));
        return false;
    }

    raise_clock();

    sd_media_mounted(&media);
    if (!sd_lease_registry_mount(&leases, media.generation)) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        card=NULL;
        sd_media_removed(&media);
        return false;
    }
    ESP_LOGI(TAG, "mounted generation=%u %llu MB sector=%u %d kHz",
             (unsigned)media.generation,
             ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20,
             (unsigned)card->csd.sector_size, card->max_freq_khz);
    // Worth saying out loud rather than leaving in a number nobody reads: at
    // the bottom rung nothing can stream off this card, so a feature that later
    // stutters has its explanation here rather than in the feature.
    if (card->max_freq_khz <= SD_CLOCK_LADDER[SD_CLOCK_STEPS - 1])
        ESP_LOGW(TAG, "card only answers at the identification clock; "
                      "streaming from it will not keep up");
    return true;
}

void sd_media_unmount(void) {
    sd_lease_cancel_all(&leases);
    // Removal, not failure: the grant goes with it, so a remount cannot silently
    // reconnect an app to a folder the person authorised on a different card.
    sd_media_removed(&media);
    // Explicit owner-task unmounts occur after picker/reset operations have
    // returned their handles. Give the mount heap back now when no lease pins
    // it; an active worker still defers the physical step to service().
    sd_media_service();
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
        sd_lease_cancel_all(&leases);
        sd_media_removed(&media);
    }
}

void sd_media_service(void) {
    // Workers signal through their own token. This is the owner task's only
    // bridge from a read failure to mutable media/picker-visible state.
    if(sd_lease_take_fault(&leases)) sd_media_note_error(ESP_ERR_TIMEOUT);
    if(sd_lease_ready_to_unmount(&leases)) {
        if(card) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT,card);
            card=NULL;
        }
        sd_lease_unmounted(&leases);
    }
}

bool sd_media_read_lease_bind(sd_media_read_lease_t *lease, FILE *file,
                              const char *fspath, uint32_t key,
                              uint32_t generation, uint32_t size,
                              sd_media_lease_mode_t mode) {
    if(!lease||!fspath||(mode==SD_MEDIA_LEASE_PERSISTENT&&!file)||
       (mode!=SD_MEDIA_LEASE_REOPEN&&mode!=SD_MEDIA_LEASE_PERSISTENT)) return false;
    size_t len=strlen(fspath);
    if(len>=sizeof lease->path||!sd_lease_attach(&leases,&lease->token,key,generation))
        return false;
    lease->file=file;
    lease->mode=mode;
    lease->size=size;
    memcpy(lease->path,fspath,len+1);
#ifdef KASANE_P0_PROBE
    lease->open_count=mode==SD_MEDIA_LEASE_PERSISTENT?1:0;
    lease->read_count=lease->read_bytes=lease->read_max_us=lease->read_slow_count=0;
#endif
    return true;
}

int32_t sd_media_read_lease_read_at(sd_media_read_lease_t *lease,
                                    uint32_t offset, uint8_t *out,
                                    uint32_t want, int *err) {
    if(err) *err=0;
    if(!lease||!out||!err) {
        if(err) *err=EINVAL;
        return -1;
    }
    if(atomic_load(&lease->token.revoked)) { *err=ENODEV; return -1; }
    if(offset>=lease->size) return 0;
    if(want>lease->size-offset) want=lease->size-offset;
    if(!want) return 0;
#ifdef KASANE_P0_PROBE
    int64_t started=esp_timer_get_time();
    if(lease->mode==SD_MEDIA_LEASE_REOPEN) lease->open_count++;
    lease->read_count++;
#endif
    FILE *file=lease->mode==SD_MEDIA_LEASE_REOPEN
               ?fopen(lease->path,"rb"):lease->file;
    if(!file) {
        *err=errno?errno:EIO;
        if(*err==EIO||*err==ENODEV) atomic_store(&lease->token.fault,true);
        return -1;
    }
    if(fseek(file,(long)offset,SEEK_SET)!=0) {
        *err=errno?errno:EIO;
        if(lease->mode==SD_MEDIA_LEASE_REOPEN) fclose(file);
        if(*err==EIO||*err==ENODEV) atomic_store(&lease->token.fault,true);
        return -1;
    }
    size_t got=fread(out,1,want,file);
    if(got<want&&ferror(file)) *err=errno?errno:EIO;
    if(lease->mode==SD_MEDIA_LEASE_REOPEN&&fclose(file)!=0&&!*err)
        *err=errno?errno:EIO;
#ifdef KASANE_P0_PROBE
    uint32_t us=(uint32_t)(esp_timer_get_time()-started);
    lease->read_bytes+=(uint32_t)got;
    if(us>lease->read_max_us) lease->read_max_us=us;
    if(us>=200000u) lease->read_slow_count++;
#endif
    if(*err) {
        if(*err==EIO||*err==ENODEV) atomic_store(&lease->token.fault,true);
        return -1;
    }
    return (int32_t)got;
}

void sd_media_read_lease_cancel(sd_media_read_lease_t *lease) {
    if(lease) atomic_store(&lease->token.revoked,true);
}

void sd_media_read_lease_ack(sd_media_read_lease_t *lease) {
    if(lease) sd_lease_ack(&lease->token);
}

bool sd_media_read_lease_acked(const sd_media_read_lease_t *lease) {
    return lease&&atomic_load(&lease->token.ack);
}

bool sd_media_read_lease_close(sd_media_read_lease_t *lease) {
    if(!sd_media_read_lease_acked(lease)) return false;
    // An ACK may arrive and be closed before the next owner pump. Consume its
    // fault here too, so detaching cannot hide the card-removal observation.
    if(atomic_exchange(&lease->token.fault,false))
        sd_media_note_error(ESP_ERR_TIMEOUT);
    // The ACK proves no worker can still use the FILE. Keep the lease attached
    // until fclose has finished, including a possible card error in fclose.
    if(lease->file) {
        if(fclose(lease->file)!=0&&errno==EIO)
            sd_media_note_error(ESP_ERR_TIMEOUT);
        lease->file=NULL;
    }
    return sd_lease_detach(&leases,&lease->token);
}

bool sd_media_read_lease_busy(uint32_t key) {
    return sd_lease_busy(&leases,key);
}

bool sd_media_read_lease_tree_busy(const char *fspath) {
    if(!fspath) return false;
    size_t len=strlen(fspath);
    for(const sd_lease_t *at=leases.head;at;at=at->next) {
        const sd_media_read_lease_t *lease=(const sd_media_read_lease_t *)at;
        if(!strncmp(lease->path,fspath,len)&&
           (lease->path[len]=='/'||lease->path[len]=='\0')) return true;
    }
    return false;
}

bool sd_media_file_close_safe(uint32_t generation) {
    return card&&generation!=0&&leases.generation==generation;
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
