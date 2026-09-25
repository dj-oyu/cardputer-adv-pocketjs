// Mount, generation and removal for fs.volume.sd. The decisions that need no
// card are in sd_path.h; this is only the part that touches one.
#ifndef SD_MEDIA_H
#define SD_MEDIA_H

#include "sd_path.h"
#include "sd_lease.h"
#include <stdio.h>

// Matches FS_MAX_HANDLES in pocket_fs.c: two open files is the state section 8
// bounds an app to, and the measured cost of two is 1,640 bytes.
//
// It bounds FatFs, not the app, and those are no longer the same count. The
// native readers in pocket_fs.c open a FILE of their own for the length of one
// call, so an app already holding two sd: handles makes the player's next
// pocket_fs_read_at() fail to open -- LIMIT_EXCEEDED, correctly reported and
// still a surprise.
//
// Raising this to 3 was proposed and DECLINED on 2026-09-09, so that it is not
// re-proposed from scratch: the cost is about 820 bytes of heap per mount
// (derived from the 1,640 measured for two, not measured on its own), and the
// case it would smooth is a native stream from a card that cannot feed one in
// realtime at the clock sd_media.c negotiates. Time a card read first; that
// measurement decides this and several larger things with it.
#define SD_MAX_OPEN_FILES 2

void sd_media_init(void);

// The current state, for capability probes and volume info. Never NULL.
const sd_media_t *sd_media(void);

// Explicit host-driven mount. Section 3 gives no app a way to ask for this:
// mounting is the host's, and an app only ever observes the result.
bool sd_media_mount(void);
void sd_media_unmount(void);

// The picker's choice, applied to the live media. sd_media() hands back a
// const pointer precisely so that this is the only way the grant can move, and
// so that the one call site is greppable: the grant comes from a person or it
// does not exist. sd_path.c's sd_media_grant() does the checking.
bool sd_media_grant_folder(const char *folder, size_t len);

// Classify an I/O failure. On this board removal has no pin and is only ever
// the shape of a failed command, so this is the ONLY way the volume can learn
// that a card is gone.
void sd_media_note_error(int err);

// The owner task creates a lease from an already-authorised SD reader. REOPEN
// holds no FILE between reads; PERSISTENT transfers the open FILE to the
// session. The copied physical path belongs to this playback session.
// Only its worker calls read_at() and ack(); no worker calls sd_media(),
// the picker, pocket_fs_read_at(), or JS. The owner calls close() only after ACK.
// A failed close (no ACK yet) must leave the whole session allocated.
typedef enum {
    SD_MEDIA_LEASE_REOPEN=0,
    SD_MEDIA_LEASE_PERSISTENT,
} sd_media_lease_mode_t;

typedef struct {
    sd_lease_t token;
    FILE *file;
    sd_media_lease_mode_t mode;
    uint32_t size;
    char path[SD_FSPATH_MAX];
#ifdef KASANE_P0_PROBE
    uint32_t open_count, read_count, read_bytes, read_max_us, read_slow_count;
#endif
} sd_media_read_lease_t;

bool sd_media_read_lease_bind(sd_media_read_lease_t *lease, FILE *file,
                              const char *fspath, uint32_t key,
                              uint32_t generation, uint32_t size,
                              sd_media_lease_mode_t mode);
int32_t sd_media_read_lease_read_at(sd_media_read_lease_t *lease,
                                    uint32_t offset, uint8_t *out,
                                    uint32_t want, int *err);
void sd_media_read_lease_cancel(sd_media_read_lease_t *lease);
void sd_media_read_lease_ack(sd_media_read_lease_t *lease);
bool sd_media_read_lease_acked(const sd_media_read_lease_t *lease);
bool sd_media_read_lease_close(sd_media_read_lease_t *lease);
bool sd_media_read_lease_busy(uint32_t key);
bool sd_media_read_lease_tree_busy(const char *fspath);
// A synchronous FILE may be closed after logical removal while physical VFS
// teardown is deferred, but never after that VFS has gone or been replaced.
bool sd_media_file_close_safe(uint32_t generation);

// Called on the owner task after filesystem operations have returned. A failed
// command revokes the grant immediately but VFS teardown waits for every
// lease's worker ACK and close. A timed-out lease pins the mount safely.
void sd_media_service(void);

// Touches the medium; section 3 allows that for space() and forbids it for
// volumes(). False leaves the outputs untouched rather than reporting zero,
// because section 3 keeps "unavailable" and 0 apart.
bool sd_media_space(uint64_t *capacity, uint64_t *freebytes);

#endif
