#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.fs — the file surface of docs/common-api.md section 7, specified in
// detail by docs/filesystem-api.md.
//
// Three volumes are named by that document. This build serves two of them:
//
//   app:/     a log-structured store of this file's own making, living in the
//             tail of the `storage` partition behind srcstore's 16 slots.
//   assets:/  the JavaScript sources the firmware already embeds, read straight
//             out of flash without a copy in RAM.
//   sd:/      NOT implemented here. The slot exists on this board, but nothing
//             in this firmware has ever mounted it and the capability says so
//             rather than guessing. See the header comment in pocket_fs.c.
//
// Everything here runs on the JS owner task. Flash reads, erases and writes all
// happen inside the call, so a Promise from this surface is already settled
// when the app receives it — the same shape pocket_storage.c uses, and for the
// same reason: nothing may touch QuickJS from a worker until the host owns a
// completion path back. What that costs in a frame is measured in the README
// beside apps/pocketfs.

// Registers globalThis.pocket.fs. Install AFTER pocket_api_install(); the
// namespace hangs off the pocket root and this returns ESP_ERR_INVALID_STATE
// when that root is not there yet.
esp_err_t pocket_fs_install(JSContext *ctx, void *user_data);

// Ends the session's use of the store: every open handle is closed, an
// uncommitted create/replace has its temporary blocks erased, the list cursors
// and volume subscriptions are dropped, and the in-RAM index is freed. Call
// from app_stop() while the guest is still alive. Nothing of this surface
// outlives a session except 481 bytes of static tables; the 504-byte index is
// rebuilt from 64 sector headers the next time an app touches a file.
void pocket_fs_reset(void);

// Which app's files app:/ resolves to. Objects carry an owner hash and a lookup
// filters on it, so two apps cannot see each other's names even though they
// share one region. Today's session model has no app identity to pass in — one
// app runs at a time — so this mirrors pocket_storage_set_owner(): a
// host-settable name with a default, ready for when app registration lands.
// Call between sessions, never during one; open handles refer to objects that a
// change of owner would make unreachable.
void pocket_fs_set_owner(const char *app_id);

// Reads a whole file for another native surface. `out` may be NULL to ask only
// how big the file is. Returns the byte count, or -1 with *code set to the
// PocketError code the caller should reject with. Paths are parsed exactly as
// an app's are, so sd: is refused here for the same reason it is there.
//
// This exists for pocket_av.c's audio.player, which needs a clip's bytes and
// has no JS round trip to get them through. It is the JS task's to call, like
// everything else in this file: the flash reads happen inside it.
int32_t pocket_fs_read_all(const char *path, uint8_t *out, uint32_t cap,
                           const char **code);
