// The list: rows, cursor, window, deadline. No pixels, no board, no IDF --
// the same split ui/overlay_core.c has against ui/overlay.c, and for the same
// reason: what a picker gets wrong is off-by-one at a window edge, and that is
// settled on the host in tools/test_pickmodal.c rather than by flashing.
#include "pickmodal.h"
#include <string.h>

// Refills the window so that `want` is inside it, and leaves the cursor there.
// The window is anchored a little before the target rather than exactly on it,
// so that scrolling back up by one does not immediately refill again.
static void window_to(pickmodal_t *p, unsigned want) {
    unsigned anchor = want >= PICK_ROWS_VISIBLE ? want-PICK_ROWS_VISIBLE : 0;
    p->more=false;
    p->count=p->cfg.fill?p->cfg.fill(p->cfg.user,anchor,p->rows,PICK_WINDOW,
                                     &p->more):0;
    p->first=anchor;
    // The list can have shrunk since the caller last looked -- on this board a
    // card that went away is discovered by a read returning less than it did,
    // because there is no card-detect pin to discover it any other way. An
    // anchor past the new end answers nothing, so ask again from the top
    // rather than leaving the person on an empty screen that still has rows.
    if(!p->count&&anchor) {
        p->count=p->cfg.fill?p->cfg.fill(p->cfg.user,0,p->rows,PICK_WINDOW,
                                         &p->more):0;
        p->first=0;
    }
    if(!p->count) { p->first=0; p->cursor=0; p->top=0; return; }
    unsigned last=p->first+p->count-1;
    p->cursor=want>last?last:(want<p->first?p->first:want);
    if(p->cursor<p->top) p->top=p->cursor;
    if(p->cursor>=p->top+PICK_ROWS_VISIBLE) p->top=p->cursor-PICK_ROWS_VISIBLE+1;
    if(p->top<p->first) p->top=p->first;
}

void pickmodal_open(pickmodal_t *p, const pickmodal_cfg_t *cfg) {
    memset(p,0,sizeof(*p));
    p->cfg=*cfg;
    window_to(p,0);
    p->top=p->first;
    p->dirty=true;
}

void pickmodal_reload(pickmodal_t *p) {
    p->top=0;
    window_to(p,0);
    p->top=p->first;
    p->dirty=true;
}

const pick_row_t *pickmodal_current(const pickmodal_t *p) {
    if(!p->count||p->cursor<p->first||p->cursor>=p->first+p->count) return NULL;
    return &p->rows[p->cursor-p->first];
}

pickmodal_event_t pickmodal_key(pickmodal_t *p, const keystroke_t *key) {
    if(!key) return PICK_EVENT_NONE;
    unsigned want=p->cursor;
    switch(key->nav) {
        case KEY_UP:
            if(!p->cursor) return PICK_EVENT_NONE;
            want=p->cursor-1;
            break;
        case KEY_DOWN:
            // The end of the window is not the end of the list, so "is there a
            // next row" is a question about count AND more, not about count.
            if(p->cursor+1>=p->first+p->count&&!p->more) return PICK_EVENT_NONE;
            want=p->cursor+1;
            break;
        case KEY_ENTER:
            return p->count?PICK_EVENT_CHOSE:PICK_EVENT_NONE;
        case KEY_BACK:
            return PICK_EVENT_CANCELLED;
        default:
            return PICK_EVENT_NONE;
    }
    if(want<p->first||want>=p->first+p->count) window_to(p,want);
    else p->cursor=want;
    if(p->cursor<p->top) p->top=p->cursor;
    if(p->cursor>=p->top+PICK_ROWS_VISIBLE) p->top=p->cursor-PICK_ROWS_VISIBLE+1;
    p->dirty=true;
    return PICK_EVENT_NONE;
}

bool pickmodal_expired(const pickmodal_t *p, int64_t now_us) {
    return p->cfg.deadline_us&&now_us>p->cfg.deadline_us;
}

