// The screen half of "the host asks the person", shared by every picker.
//
// WHY THIS EXISTS. sd_picker.h says it in its own words: the works picker and
// the folder picker were deliberately given the same shape, because "a second
// pattern for 'the host asks the person' would be a second place to get it
// wrong". A file picker is the third, and three copies of a cursor, a scroll
// window and a deadline is where that stops being a shape and starts being a
// duplication. So the shape is a type now.
//
// WHAT IS HERE AND WHAT IS NOT. This owns the LIST: the rows, where the cursor
// is, which window of them is visible, when the person ran out of time, and the
// pixels. It does not own the PROMISE, and that split is deliberate rather than
// tidy -- what a choice MEANS differs at every call site (the folder picker's
// choice is a grant, the file picker's is a path, the works picker's is a
// reference), and the settle is where a picker can leak something it should
// not. Each consumer keeps its own settle, in its own file, next to the reason
// it is written that way.
//
// The rows come from a callback rather than an array, because a directory does
// not fit in RAM and a listing must not have to. See pickmodal_fill_fn.
#ifndef PICKMODAL_H
#define PICKMODAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "keymap.h"

// A name long enough to be an sd: grant root (SD_ROOT_MAX is 96, but a row that
// cannot be shown is a row that cannot be chosen, and 64 is already wider than
// the screen at this face). A longer name is not offered rather than offered
// and then refused at the moment of choosing -- sd_picker.c's rule, kept.
#define PICK_NAME_MAX 64

// How many rows are held at once. The window is what bounds the memory, and it
// is bigger than the six that fit on screen so that ordinary scrolling does not
// re-read the directory on every keypress.
#define PICK_WINDOW      24
#define PICK_ROWS_VISIBLE 6

typedef struct {
    char     name[PICK_NAME_MAX];
    bool     is_dir;
    uint32_t size;      // bytes; meaningless and ignored when is_dir
} pick_row_t;

// Fills up to `max` rows starting at directory index `from`, returns how many
// it wrote and sets *more when something follows the last one.
//
// It is called with an arbitrary `from`, so a backend over readdir() has to
// rewind and skip. That is O(n) in the offset and it happens only when the
// cursor leaves the window, which is why the window is 24 and not 6: the cost
// is paid once per two screens of scrolling, not once per keypress.
typedef unsigned (*pickmodal_fill_fn)(void *user, unsigned from,
                                      pick_row_t *out, unsigned max,
                                      bool *more);

typedef enum {
    PICK_EVENT_NONE = 0,   // the key moved the cursor, or was not ours
    PICK_EVENT_CHOSE,      // Enter on a row; read it with pickmodal_current()
    PICK_EVENT_CANCELLED,  // Escape
    PICK_EVENT_EXPIRED,    // nobody answered in time
} pickmodal_event_t;

typedef struct {
    // The words on the screen are the HOST'S, never the app's. An app-supplied
    // title would be untrusted text on the one screen whose whole job is to
    // tell the person truthfully what they are about to allow.
    const char *title;
    const char *hint_rows;   // footer while there is something to choose
    const char *hint_empty;  // footer when the list came back empty
    const char *empty;       // the line drawn in place of rows

    pickmodal_fill_fn fill;
    void             *user;

    int64_t deadline_us;
} pickmodal_cfg_t;

typedef struct {
    pickmodal_cfg_t cfg;
    pick_row_t rows[PICK_WINDOW];
    unsigned   count;        // rows held, <= PICK_WINDOW
    unsigned   first;        // directory index of rows[0]
    unsigned   cursor;       // directory index of the selected row
    unsigned   top;          // directory index of the first visible row
    bool       more;         // something follows rows[count-1]
    bool       dirty;
} pickmodal_t;

// Fills the window from index 0 and puts the cursor on the first row.
void pickmodal_open(pickmodal_t *p, const pickmodal_cfg_t *cfg);

// One keystroke. Movement past the window's edge refills it through cfg.fill.
pickmodal_event_t pickmodal_key(pickmodal_t *p, const keystroke_t *key);

// The chosen row, or NULL when the list is empty. Valid until the next key.
const pick_row_t *pickmodal_current(const pickmodal_t *p);

// Re-reads the window at the current position, for a consumer that changed
// what fill() will answer (entering a directory, say). Cursor goes to the top.
void pickmodal_reload(pickmodal_t *p);

// True when the deadline has passed. Checked from the draw path, because
// pocket_api_pump() does not run while the guest is not ticked -- so nothing
// else would notice a promise whose time ran out while a person read the list.
bool pickmodal_expired(const pickmodal_t *p, int64_t now_us);

// Composes the whole screen, strip by strip, and presents it.
void pickmodal_draw(pickmodal_t *p);

#endif
