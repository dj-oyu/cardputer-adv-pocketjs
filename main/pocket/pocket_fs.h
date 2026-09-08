#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.fs — the file surface of docs/common-api.md section 7, specified in
// detail by docs/filesystem-api.md.
//
// Three volumes are named by that document, and this build serves all three:
//
//   app:/     a log-structured store of this file's own making, living in the
//             tail of the `storage` partition behind srcstore's 16 slots.
//   assets:/  the JavaScript sources the firmware already embeds, read straight
//             out of flash without a copy in RAM.
//   sd:/      the folder on the card that the PERSON granted, through the host
//             screen in sd_picker.c. Nothing else in this firmware mounts a
//             card, so an app that was never granted a folder cannot tell a
//             card in the slot from an empty one. Read and write both, with
//             crashSafeReplace=false and the consequences that carries -- see
//             the header comment in pocket_fs.c and sd_path.h.
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

// One frame's worth of "did the card change?". sd: is the only volume that
// moves, and it moves in two places that cannot deliver an event themselves --
// inside the folder picker's key handling, and inside a failed I/O in the
// middle of a native call. Firing a guest callback from either would run JS
// while this surface is part way through an operation. This is where
// fs.onVolumeChange and the fs.volume.sd capability change are announced
// instead: once a turn, on the JS task, with nothing in flight. Call it from
// app_tick() beside the other pumps; it costs two comparisons when the card has
// not moved, and nothing at all before an app has read pocket.fs.
void pocket_fs_pump(void);

// Which app's files app:/ resolves to. Objects carry an owner hash and a lookup
// filters on it, so two apps cannot see each other's names even though they
// share one region. Today's session model has no app identity to pass in — one
// app runs at a time — so this mirrors pocket_storage_set_owner(): a
// host-settable name with a default, ready for when app registration lands.
// Call between sessions, never during one; open handles refer to objects that a
// change of owner would make unreachable.
void pocket_fs_set_owner(const char *app_id);

// Reads for another native surface -- pocket_av.c's audio.player, which needs a
// clip's bytes and has no JS round trip to get them through. `read_all` takes
// the whole file, and `out` may be NULL to ask only how big it is; `read_at`
// takes a range, for a caller that streams rather than loads. Both return the
// byte count -- read_at is short at the end of the file and 0 past it -- or -1
// with *code set to the PocketError code the caller should report.
//
// All three volumes, sd: included: paths are parsed and authorised exactly as
// an app's are, by the same code, so the grant is tested before the media state
// and a file some app is part way through writing answers BUSY. There is no
// handle: read_at re-resolves every call on purpose, so a source that is
// deleted, replaced, or on a card that has been pulled ends a stream as a read
// failure rather than quietly playing something else.
//
// THE JS TASK'S TO CALL. The flash reads happen inside the call; the app:
// index, the handle table, the sd: grant and generation, and FatFs itself all
// belong to that task. A worker calling either of these would race the folder
// picker.
//
// What re-resolving costs, because it is not the same on every volume:
// assets: is a bounds check, app: is one small flash read, and sd: is a
// directory walk plus an fopen -- on a 400 kHz bus, milliseconds, inside
// whatever frame called it. That is a real budget for a streaming caller and
// it is arithmetic, not a measurement: nobody has yet timed a card-backed
// stream on this board.
int32_t pocket_fs_read_all(const char *path, uint8_t *out, uint32_t cap,
                           const char **code);
int32_t pocket_fs_read_at(const char *path, uint32_t offset, uint8_t *out,
                          uint32_t want, const char **code);
