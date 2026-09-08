// Mount, generation and removal for fs.volume.sd. The decisions that need no
// card are in sd_path.h; this is only the part that touches one.
#ifndef SD_MEDIA_H
#define SD_MEDIA_H

#include "sd_path.h"

// Matches FS_MAX_HANDLES in pocket_fs.c: two open files is the state section 8
// bounds an app to, and the measured cost of two is 1,640 bytes.
#define SD_MAX_OPEN_FILES 2

void sd_media_init(void);

// The current state, for capability probes and volume info. Never NULL.
const sd_media_t *sd_media(void);

// Explicit host-driven mount. Section 3 gives no app a way to ask for this:
// mounting is the host's, and an app only ever observes the result.
bool sd_media_mount(void);
void sd_media_unmount(void);

// Classify an I/O failure. On this board removal has no pin and is only ever
// the shape of a failed command, so this is the ONLY way the volume can learn
// that a card is gone.
void sd_media_note_error(int err);

// Touches the medium; section 3 allows that for space() and forbids it for
// volumes(). False leaves the outputs untouched rather than reporting zero,
// because section 3 keeps "unavailable" and 0 apart.
bool sd_media_space(uint64_t *capacity, uint64_t *freebytes);

#endif
