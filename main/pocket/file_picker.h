// pocket.fs.pickFile(volumeId, options) -> Promise<string|null>.
//
// The host screen that turns a person's choice into ONE path. It is the third
// consumer of the "host asks the person" shape and the first one built on the
// shared screen in ui/pickmodal.h rather than on its own copy of it.
//
// HOW THIS DIFFERS FROM fs.requestFolder, which is the question a reader of
// both will have. requestFolder MAKES an authorisation: the folder the person
// picks becomes the grant, and it is the only call to sd_media_grant_folder()
// in the firmware. pickFile makes none. It runs INSIDE a grant that already
// exists and hands back a path the app could have reached by listing, so the
// person choosing here is choosing convenience, not permission. That is why it
// refuses with PERMISSION_DENIED rather than opening a screen when there is no
// grant -- offering to browse a card the app was never given would make a
// picker into a way around the picker.
//
// Which also settles the volume question: sd: is the only volume here, because
// app: and assets: need no permission and the app can already list them. If a
// picker over those is ever wanted it is a UI convenience with no authorisation
// story, and the seam for it is the fill callback, not this file's rules.
#ifndef FILE_PICKER_H
#define FILE_PICKER_H

#include <stdbool.h>
#include "quickjs.h"
#include "keymap.h"

JSValue file_picker_request(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv);

// The modal contract main.c's tick_run() drives, the same one the folder and
// works pickers have: while it is up the guest is not ticked and the keys are
// the screen's.
bool file_picker_modal(void);
void file_picker_modal_key(const keystroke_t *key);
bool file_picker_modal_dirty(void);
void file_picker_modal_draw(void);

// pocket.fs.nextFile(path, options) -> Promise<string|null>.
//
// WHO OWNS "THE NEXT SONG". Not the app. Everything that decides what a
// playable file is -- the extension filter, the dot rule, sd_name_reserved,
// how deep a listing goes -- already lives in this file, next to the picker
// that shows those same rows to a person. An app walking fs.list for itself
// would build a SECOND implementation of that judgement, in JavaScript, where
// the two could disagree for months without anything failing.
//
// AND IT NEVER MATERIALISES THE LIST. fs.list refuses to sort because sorting
// means holding every name at once, and that refusal is right. But a total
// order does not require holding one: asking only for the SUCCESSOR -- the
// smallest name greater than this one -- is a single pass keeping a single
// candidate. O(n) time, O(1) memory, nothing on the guest's heap, and the guest
// never learns the names it did not ask for.
//
// It reveals nothing new either. Inside a grant fs.list already exposes every
// one of these names; what this adds is an ordering the host and the person
// agree on, not access.
JSValue file_picker_next(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv);

// The session is ending. Closes the screen and posts the completion.
void file_picker_reset(void);

#endif
