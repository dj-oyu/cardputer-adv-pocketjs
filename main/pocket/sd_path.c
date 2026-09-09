#include "sd_path.h"
#include <string.h>

void sd_media_reset(sd_media_t *m) {
    memset(m, 0, sizeof *m);
    m->state = SD_MEDIA_ABSENT;
}

void sd_media_mounted(sd_media_t *m) {
    m->state = SD_MEDIA_READY;
    m->generation++;
}

void sd_media_removed(sd_media_t *m) {
    m->state = SD_MEDIA_ABSENT;
    sd_media_revoke(m);
}

void sd_media_failed(sd_media_t *m) {
    m->state = SD_MEDIA_ERROR;
}

void sd_media_revoke(sd_media_t *m) {
    m->granted = false;
    m->root[0] = '\0';
}

// One component. Rejecting a separator here is what makes "the person chose a
// folder" true rather than a description of the happy path -- a grant of
// "a/../.." would otherwise be a grant of the whole card.
bool sd_media_grant(sd_media_t *m, const char *folder, size_t len) {
    if (!folder || len == 0) return false;
    if (len == 1 && folder[0] == '.') return false;
    if (len == 2 && folder[0] == '.' && folder[1] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        char c = folder[i];
        if (c == '/' || c == '\\' || c == '\0') return false;
    }
    // SD_MOUNT_POINT + '/' + folder + NUL
    if (sizeof(SD_MOUNT_POINT) + 1 + len > SD_ROOT_MAX) return false;

    size_t n = strlen(SD_MOUNT_POINT);
    memcpy(m->root, SD_MOUNT_POINT, n);
    m->root[n++] = '/';
    memcpy(m->root + n, folder, len);
    m->root[n + len] = '\0';
    m->granted = true;
    return true;
}

bool sd_media_usable(const sd_media_t *m) {
    return m->state == SD_MEDIA_READY && m->granted;
}

bool sd_generation_valid(const sd_media_t *m, uint32_t gen) {
    // Generation 0 is never handed out, so a zeroed handle is never valid.
    return gen != 0 && m->state == SD_MEDIA_READY && m->generation == gen;
}

bool sd_name_reserved(const char *name, size_t len) {
    size_t n = sizeof(SD_TEMP_SUFFIX) - 1;
    return len >= n && memcmp(name + len - n, SD_TEMP_SUFFIX, n) == 0;
}

sd_path_result_t sd_path_build(const sd_media_t *m, const char *rel, size_t len,
                               char *out, size_t outsz) {
    // Order matters and is a contract, not a preference. docs/filesystem-api.md
    // line 176: refuse before authorisation without revealing whether anything
    // is there. So the grant is checked BEFORE the media state -- otherwise an
    // ungranted app could tell a mounted card from an empty slot by which error
    // it got back, which is a small leak but a real one.
    if (!m->granted) return SD_PATH_NO_GRANT;
    if (m->state != SD_MEDIA_READY) return SD_PATH_DISCONNECTED;

    size_t rootn = strlen(m->root);
    if (rootn == 0) return SD_PATH_NO_GRANT;

    // Root of the virtual volume: "sd:/" itself.
    if (len == 0) {
        if (rootn + 1 > outsz) return SD_PATH_TOO_LONG;
        memcpy(out, m->root, rootn + 1);
        return SD_PATH_OK;
    }

    if (rel[0] == '/' || rel[len - 1] == '/') return SD_PATH_INVALID;

    // Walk the components. path_parse has already done this; doing it again is
    // cheap and removes the assumption that the only caller is the correct one.
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && rel[i] != '/') i++;
        size_t n = i - start;
        if (n == 0) return SD_PATH_INVALID;                 // "a//b"
        if (n == 1 && rel[start] == '.') return SD_PATH_INVALID;
        if (n == 2 && rel[start] == '.' && rel[start+1] == '.') return SD_PATH_INVALID;
        for (size_t k = 0; k < n; k++)
            if (rel[start + k] == '\\' || rel[start + k] == '\0')
                return SD_PATH_INVALID;
        // The host's own temporary files. Refused at every depth, not only at
        // the leaf: a directory named this way would hide a whole subtree from
        // listings for the same reason, and no app has a use for the name.
        if (sd_name_reserved(rel + start, n)) return SD_PATH_INVALID;
        if (i < len) i++;                                    // step over '/'
    }

    if (rootn + 1 + len + 1 > outsz || rootn + 1 + len + 1 > SD_FSPATH_MAX)
        return SD_PATH_TOO_LONG;

    memcpy(out, m->root, rootn);
    out[rootn] = '/';
    memcpy(out + rootn + 1, rel, len);
    out[rootn + 1 + len] = '\0';
    return SD_PATH_OK;
}

// ------------------------------------------------------------- clock ladder

// Fastest first. The rungs are not arbitrary:
//
//   40,000  SDMMC_FREQ_HIGHSPEED, the ceiling of SPI mode. The grades above it
//           that sdmmc.h names (SDR50, DDR50, 52M) belong to the 4-bit SD
//           interface and cannot come out of a one-wire link at all.
//   20,000  SDMMC_FREQ_DEFAULT, and the highest rung that does NOT make the
//           driver attempt the high-speed switch. Cards that fail the rung
//           above usually fail it here, in the switch rather than on the wire.
//   10,000  and
//    4,000  for a card or a wiring that will not hold the two above. Nothing
//           has ever needed these on this board; they are here so that "the
//           card is unusable" is a conclusion the walk reaches rather than an
//           assumption the first failure makes.
//      400  SDMMC_FREQ_PROBING, the identification clock. A card that only
//           works here works, barely: audio cannot stream off it (one
//           2,048-byte refill is ~41 ms against a 33 ms frame). Reaching this
//           rung is worth saying out loud, and sd_media.c does.
const int SD_CLOCK_LADDER[SD_CLOCK_STEPS] = { 40000, 20000, 10000, 4000, 400 };

int sd_clock_next(int khz, sd_mount_outcome_t outcome) {
    if (outcome != SD_MOUNT_REFUSED) return 0;
    for (int i = 0; i + 1 < SD_CLOCK_STEPS; i++)
        if (SD_CLOCK_LADDER[i] == khz) return SD_CLOCK_LADDER[i + 1];
    // Off the ladder (the bottom rung, or a rate nobody put here) ends the
    // walk. Returning the top would loop forever on a card that never mounts.
    return 0;
}

int sd_clock_boost(int mounted_khz) {
    return mounted_khz == 20000 ? 25000 : 0;
}
