#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The editing rules of a small host-owned text field, with no board, no IME and
// no allocation of its own.
//
// docs/common-api.md section 6 gives pocket.input.text a field the HOST owns:
// the guest never sees a keystroke and never holds the buffer. Almost every
// requirement in that paragraph is a decision about one keystroke -- does Enter
// submit or insert a newline, does Escape cancel the conversion or the session,
// does this commit still belong to this field -- and none of those decisions
// needs a screen. They live here so tools/test_textfield.c can settle them on a
// host, in the same spirit as vimcmd.c: the surface around them (pocket_text.c)
// cannot be run anywhere but the device, and this can.
//
// The buffer is BORROWED. A session's text exists while the session does --
// static DRAM is the binding constraint of this firmware -- so pocket_text.c
// mallocs exactly maxBytes+1 with the session and hands it in here.

typedef enum {
    TF_NONE = 0,   // handled, nothing for the app to hear
    TF_EDIT,       // the committed text changed -> onEdit
    TF_SUBMIT,     // -> onSubmit, and the session closes
    TF_CANCEL,     // -> onCancel, and the session closes
} tf_result_t;

typedef struct {
    char    *buf;         // borrowed, cap+1 bytes, always NUL-terminated
    size_t   cap;         // maxBytes. A byte count: the text is UTF-8.
    size_t   len;
    size_t   cursor;      // byte offset; always on a character boundary
    bool     multiline;
    // The last insert did not fit. Not an error the app hears -- section 6 has
    // no event for it -- but the field says so and the surface logs it.
    bool     refused;
    // Which field a commit belongs to. Bumped by tf_refocus(); a commit
    // carrying an older number is dropped rather than written. See tf_commit().
    uint32_t generation;
} textfield_t;

// `buf` must have cap+1 bytes. The field starts empty with the cursor at 0 and
// generation 1 -- never 0, so a caller that forgot to record one is refused
// rather than silently accepted.
void tf_init(textfield_t *t, char *buf, size_t cap, bool multiline);

// The `initial` of TextOptions. Refuses malformed UTF-8 (section 4 does, at
// every text API) and anything longer than cap. All or nothing: a field is
// never left holding half of what the app asked for.
bool tf_set(textfield_t *t, const char *s, size_t len);

// Insert at the cursor. Refused whole when it would pass cap, which is how
// maxBytes is kept without ever cutting a UTF-8 sequence in half -- the caller's
// unit is a character or an IME commit, and half of one is not a smaller
// version of it. Sets `refused` when it says no.
bool tf_insert(textfield_t *t, const char *s, size_t len);

// An IME commit, tagged with the generation the caller read BEFORE it fed the
// key. Returns false -- writing nothing -- when the field has been refocused
// since. That is the "フォーカス世代で遅延commitを拒否する" rule of section 6:
// a commit computed for one field must not land in the one that replaced it.
bool tf_commit(textfield_t *t, uint32_t generation, const char *s, size_t len);

// Declares that focus moved: every commit tagged with an older generation is
// now stale. Called when a session opens, closes, or is reopened underneath a
// callback that is still running.
void tf_refocus(textfield_t *t);

// One UTF-8 character before the cursor, never one byte of one.
bool tf_backspace(textfield_t *t);

// Cursor motion by character. Return true when it moved.
bool tf_left(textfield_t *t);
bool tf_right(textfield_t *t);

// One key, in the keystroke_t shape main/hal/keymap.c produces: either UTF-8
// bytes or a "\0name" token, with `len` load-bearing because of the leading
// NUL. `ime_took` is true when the IME consumed this key -- IME_TAKEN or
// IME_TEXT -- and it is the whole of the "確定EnterはonSubmitへ二重配送しない"
// rule: a key the engine ate is not also a key the field acts on.
tf_result_t tf_key(textfield_t *t, const char *key, size_t len, bool ime_took);
