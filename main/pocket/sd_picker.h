// The host screen that turns a person's choice into the sd: grant.
//
// docs/filesystem-api.md section 2: "sdへのアクセス許可はホストのフォルダー選択
// で作成し、アプリは物理パスを指定してルートを広げられない". The app names no
// folder anywhere in this file -- it asks for the screen, the person picks a
// row, and the row IS the grant. That is the same shape pocket_workspace.c's
// works picker has, and it is deliberately the same: a second pattern for
// "the host asks the person" would be a second place to get it wrong.
#ifndef SD_PICKER_H
#define SD_PICKER_H

#include <stdbool.h>
#include "quickjs.h"
#include "keymap.h"

// pocket.fs.requestFolder(volumeId, options) -> Promise<string|null>.
// Resolves with "sd:/" once the person has chosen, null if they declined.
JSValue sd_picker_request(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv);

// The modal contract main.c's tick_run() drives, identical to the works
// picker's: while active the guest is not ticked and the keys are the screen's.
bool sd_picker_modal(void);
void sd_picker_modal_key(const keystroke_t *key);
bool sd_picker_modal_dirty(void);
void sd_picker_modal_draw(void);

// The session is ending. Closes the screen and posts the completion, so
// pocket_api_reset() has something to settle rather than a promise nobody owns.
void sd_picker_reset(void);

#endif
