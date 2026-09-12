#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "keymap.h"

// pocket.input.text — the TextSession of docs/common-api.md section 6.
//
// The field is the HOST's: main.c takes the keystrokes away from the guest for
// as long as a session is open, main/text/textfield.c decides what each one
// means, skk_session.c converts, and app_session.c composites the result over
// the guest's own frame in the same strip loop pet_assets.c draws into. The
// guest sees committed text and nothing else -- no keystrokes, no preedit, no
// buffer. That is not an implementation detail; it is the first sentence of the
// paragraph this file implements.
//
// The editing rules live in textfield.c so they can be tested on a host
// (tools/test_textfield.c). What is here is the part that needs a board: the
// JS object, the IME wiring, the overlay and the session's lifetime.
//
// Everything runs on the JS owner task, which is also the UI task, so a
// callback fired straight out of a keystroke is on the right task already.

// pocket.input.text.open. Registers the input.text capability and contributes
// to the lazily-built `input` namespace pocket_ui.c also feeds.
esp_err_t pocket_text_install(JSContext *ctx, void *user_data);

// Closes any open session WITHOUT firing onCancel -- there is nobody left to
// hear it. Call from app_stop() while the guest is alive, beside the other
// surfaces' resets: a session holds three guest callbacks.
void pocket_text_reset(void);

// ------------------------------------------------------------- the host field

// True while a session owns the keyboard. main.c stops handing the guest a pad
// mask for as long as this is true, which is also what makes Escape mean the
// field rather than "leave the app".
bool pocket_text_active(void);

// One keystroke, before anything else has looked at it. Does the host's half
// immediately -- the IME, the buffer, the caret, the repaint, and the close
// that onSubmit/onCancel imply -- and QUEUES the guest's callback for
// pocket_text_pump(). main.c calls this outside app_tick(), and from L1 on a
// turn may open with an unfinished job queue that nothing may cut into
// (docs/vm-L1-design.md sec.2.1), which is why the JS_Call is not made here.
void pocket_text_key(const keystroke_t *key);

// Delivers the callbacks queued by pocket_text_key(), in the order the
// keystrokes produced them. Called from app_tick()'s pump phase, i.e. only on
// a turn that began with an empty job queue -- the same rule every other
// delivery into the guest follows.
void pocket_text_pump(void);

// Whether the field has changed since the last ask, and clears the flag. The
// guest's renderer only presents when IT has damage, so a field that moved on
// a frame the app did not needs app_force_redraw() to be seen at all.
bool pocket_text_take_dirty(void);

// Composites the field into one strip of the guest's frame. Called from
// app_tick()'s strip loop after the renderer and pet_assets_overlay(); a no-op
// when no session is open, which is every frame of every other app.
void pocket_text_overlay(uint16_t *pixels, int y, int rows);
