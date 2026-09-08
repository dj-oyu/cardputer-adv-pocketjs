// The half of fs.volume.sd that needs no card, no driver and no IDF: the media
// state machine, the grant, and the translation from a virtual sd:/ path to the
// FatFs path underneath it. Split out so tools/test_sd.c can exercise the rules
// that decide what an app may reach, on the host, without a board.
//
// Everything a mistake here would cost is an authorisation bug, which is the
// class of bug least likely to be noticed by running the feature and most
// likely to be noticed by a test that tries to escape.
#ifndef SD_PATH_H
#define SD_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// docs/filesystem-api.md section 3. ABSENT covers "no card" and "card removed":
// both are DISCONNECTED to an app, and neither is distinguishable from the
// other on this board, which has no card-detect pin.
typedef enum { SD_MEDIA_ABSENT = 0, SD_MEDIA_READY, SD_MEDIA_ERROR } sd_media_state_t;

#define SD_MOUNT_POINT  "/sd"
// The granted folder as FatFs sees it: SD_MOUNT_POINT plus one component.
#define SD_ROOT_MAX     96
// The FatFs side of a translated path.
#define SD_FSPATH_MAX   320

typedef struct {
    sd_media_state_t state;
    // Section 3: changes on every mount / removal / reinsertion. Handles and
    // list cursors carry the value they were made under, so a stale one is
    // detectable rather than merely unlucky. Starts at 0, which no live handle
    // can hold, so a zeroed struct grants nothing.
    uint32_t generation;
    bool     granted;
    char     root[SD_ROOT_MAX];
} sd_media_t;

void sd_media_reset(sd_media_t *m);

// A mount succeeded. Bumps the generation: section 3 makes reinsertion a new
// generation even if it is the same card, because nothing here can prove it is.
void sd_media_mounted(sd_media_t *m);

// The card is gone, or an operation failed in a way that says so. Drops the
// grant with it -- a new card is not the folder the person authorised, and
// reinsertion must go back through the picker rather than silently reconnecting
// an app to a different filesystem.
void sd_media_removed(sd_media_t *m);

// Mounted media that answered wrongly (CORRUPT_DATA, or a format we do not
// support). Distinct from ABSENT because the card is still physically there and
// a remount is not going to help.
void sd_media_failed(sd_media_t *m);

// The picker's result IS the grant, exactly as pocket_workspace.c has it. One
// path component, no separators: the person chose a folder in the card's root,
// not an arbitrary path an app proposed.
bool sd_media_grant(sd_media_t *m, const char *folder, size_t len);
void sd_media_revoke(sd_media_t *m);

// Ready AND granted. Both halves matter: a mounted card an app was never given
// is PERMISSION_DENIED, and a grant against absent media is DISCONNECTED.
bool sd_media_usable(const sd_media_t *m);

// A handle or cursor made under `gen` is still live.
bool sd_generation_valid(const sd_media_t *m, uint32_t gen);

// Reasons sd_path_build can refuse, so the caller can pick the right error
// without re-deriving why.
typedef enum {
    SD_PATH_OK = 0,
    SD_PATH_NO_GRANT,      // -> PERMISSION_DENIED (never reveal existence)
    SD_PATH_DISCONNECTED,  // -> DISCONNECTED
    SD_PATH_INVALID,       // -> INVALID_ARGUMENT
    SD_PATH_TOO_LONG,      // -> INVALID_ARGUMENT
} sd_path_result_t;

// `rel` is the part of the virtual path after "sd:/", already validated by
// pocket_fs.c's path_parse (which refuses ".", "..", backslash and the
// non-portable characters). This re-checks anyway: the cost is a byte compare
// and the thing it prevents is an app reaching outside the folder it was given.
// A defence that only works when its caller is correct is not a defence.
sd_path_result_t sd_path_build(const sd_media_t *m, const char *rel, size_t len,
                               char *out, size_t outsz);

#endif
