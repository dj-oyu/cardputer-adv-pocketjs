// The list half of every "host asks the person" screen, on the host.
//
// WHAT IS ACTUALLY AT RISK HERE. A picker's bugs are not in its pixels; they
// are off-by-one at the edge of a window that does not hold the whole
// directory. Before ui/pickmodal.c there was no window at all -- the folder
// picker read twelve names into an array and stopped -- so a card with more
// folders than that simply hid the rest, and nothing failed. This file is what
// makes the paging observable without a card and without a board.
//
//   gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
//       -I main/ui -I main/hal -I tools/hostshim
//       tools/test_pickmodal.c main/ui/pickmodal.c -o /tmp/t && /tmp/t
//
// (from the repository root)

#include "pickmodal.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond,...) do { if(!(cond)) { \
    printf("FAIL %s:%d ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while(0)

// ------------------------------------------------------- a directory of names

typedef struct { unsigned total, calls; } fake_dir_t;

static unsigned fake_fill(void *user, unsigned from, pick_row_t *out,
                          unsigned max, bool *more) {
    fake_dir_t *d=user;
    d->calls++;
    unsigned n=0;
    for(unsigned i=from;i<d->total&&n<max;i++,n++) {
        snprintf(out[n].name,PICK_NAME_MAX,"row%u",i);
        out[n].is_dir=(i%3)==0;
        out[n].size=i*1000u;
    }
    *more=from+n<d->total;
    return n;
}

static void open_over(pickmodal_t *p, fake_dir_t *d, unsigned total) {
    d->total=total; d->calls=0;
    pickmodal_cfg_t cfg={.title="T",.hint_rows="H",.hint_empty="E",.empty="X",
                         .fill=fake_fill,.user=d,.deadline_us=0};
    pickmodal_open(p,&cfg);
}

static keystroke_t nav(board_key_t k) {
    keystroke_t s={0}; s.nav=k; return s;
}

static void press(pickmodal_t *p, board_key_t k, unsigned times) {
    for(unsigned i=0;i<times;i++) { keystroke_t s=nav(k); pickmodal_key(p,&s); }
}

// ------------------------------------------------------------------ the cases

static void empty_list(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,0);
    CHECK(p.count==0,"no rows");
    CHECK(pickmodal_current(&p)==NULL,"nothing is current in an empty list");
    // Enter on nothing is not a choice. The folder picker relied on this to
    // avoid granting a row that does not exist.
    keystroke_t e=nav(KEY_ENTER);
    CHECK(pickmodal_key(&p,&e)==PICK_EVENT_NONE,"enter on an empty list");
    keystroke_t b=nav(KEY_BACK);
    CHECK(pickmodal_key(&p,&b)==PICK_EVENT_CANCELLED,"escape still cancels");
}

static void short_list(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,4);
    CHECK(p.count==4&&!p.more,"the whole directory fits in one window");
    CHECK(d.calls==1,"and it was read once");
    press(&p,KEY_UP,3);
    CHECK(p.cursor==0,"up at the top does not wrap or underflow");
    press(&p,KEY_DOWN,10);
    CHECK(p.cursor==3,"down at the end stops at the last row");
    CHECK(d.calls==1,"a list that fits is never re-read");
    const pick_row_t *row=pickmodal_current(&p);
    CHECK(row&&!strcmp(row->name,"row3"),"the last row is current");
}

// The case the old fixed array could not express: more rows than the window.
static void long_list(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,100);
    CHECK(p.count==PICK_WINDOW&&p.more,"the first window is full and not the end");

    // Walking down to the last row of the window costs no refill...
    press(&p,KEY_DOWN,PICK_WINDOW-1);
    CHECK(p.cursor==PICK_WINDOW-1,"cursor reached the window's last row");
    CHECK(d.calls==1,"no refill inside the window");
    // ...and stepping one past it does exactly one.
    press(&p,KEY_DOWN,1);
    CHECK(p.cursor==PICK_WINDOW,"cursor stepped out of the window");
    CHECK(d.calls==2,"one refill, not one per row");
    const pick_row_t *row=pickmodal_current(&p);
    CHECK(row&&!strcmp(row->name,"row24"),"and it is still the right row");

    // The anchor sits PICK_ROWS_VISIBLE before the cursor, so walking back up
    // a screenful stays inside the window that was just fetched. Without that
    // every keypress at a boundary would re-read the directory.
    unsigned calls=d.calls;
    press(&p,KEY_UP,PICK_ROWS_VISIBLE);
    CHECK(d.calls==calls,"stepping back a screenful does not refill");

    // The end of a paged list is the end of the list, not the end of a window.
    press(&p,KEY_DOWN,200);
    CHECK(p.cursor==99,"cursor stops on the last row of the directory");
    CHECK(!p.more,"and the last window knows it is last");
    row=pickmodal_current(&p);
    CHECK(row&&!strcmp(row->name,"row99"),"the last name is the last name");
}

// Exactly one window: `more` must be false, or KEY_DOWN would walk off the end
// looking for a row that is not there.
static void exact_window(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,PICK_WINDOW);
    CHECK(p.count==PICK_WINDOW&&!p.more,"a full window that is also the whole list");
    press(&p,KEY_DOWN,PICK_WINDOW+5);
    CHECK(p.cursor==PICK_WINDOW-1,"the cursor stops at the real last row");
    CHECK(d.calls==1,"and nothing was re-read looking for more");
}

// The visible six always contain the cursor, at every position. This is what
// the person actually sees, and it is the property a scroll bug breaks.
static void cursor_stays_visible(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,100);
    for(unsigned i=0;i<99;i++) {
        press(&p,KEY_DOWN,1);
        CHECK(p.cursor>=p.top&&p.cursor<p.top+PICK_ROWS_VISIBLE,
              "cursor %u is inside the visible window at top %u",p.cursor,p.top);
        CHECK(p.cursor>=p.first&&p.cursor<p.first+p.count,
              "cursor %u is inside the held window at first %u",p.cursor,p.first);
    }
    for(unsigned i=0;i<99;i++) {
        press(&p,KEY_UP,1);
        CHECK(p.cursor>=p.top&&p.cursor<p.top+PICK_ROWS_VISIBLE,
              "cursor %u is visible walking back, top %u",p.cursor,p.top);
        CHECK(p.cursor>=p.first&&p.cursor<p.first+p.count,
              "cursor %u is held walking back, first %u",p.cursor,p.first);
    }
    CHECK(p.cursor==0,"back at the top");
}

// A directory that shrinks under the screen. On this board that is the
// ordinary case rather than the exotic one: there is no card-detect pin, so a
// card that goes away is discovered by a read returning less than it did.
static void list_shrinks(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,100);
    press(&p,KEY_DOWN,80);
    CHECK(p.cursor==80,"walked to row 80");
    d.total=10;
    // The shrink is discovered at the next WINDOW edge, not at the next
    // keypress: rows already held are still shown, and that is the honest
    // behaviour rather than a bug -- nothing has re-read the directory, so
    // nothing yet knows. Walking off the held window is what asks again.
    press(&p,KEY_DOWN,1);
    CHECK(p.cursor==81,"a step inside the stale window still moves");
    press(&p,KEY_DOWN,30);
    CHECK(p.cursor<10,"the cursor is clamped into what came back, not left past it");
    CHECK(pickmodal_current(&p)!=NULL,"and still points at a row that exists");
    CHECK(p.first==0,"an anchor past the new end asks again from the top");
}

// Entering a folder is a reload, not a scroll: the cursor goes to the top of
// the new directory rather than keeping an index that means nothing there.
static void reload_resets(void) {
    pickmodal_t p; fake_dir_t d;
    open_over(&p,&d,100);
    press(&p,KEY_DOWN,50);
    d.total=7;
    pickmodal_reload(&p);
    CHECK(p.cursor==0&&p.top==0&&p.first==0,"reload starts at the top");
    CHECK(p.count==7&&!p.more,"and holds the new directory");
}

static void deadline(void) {
    pickmodal_t p; fake_dir_t d;
    d.total=3; d.calls=0;
    pickmodal_cfg_t cfg={.title="T",.hint_rows="H",.hint_empty="E",.empty="X",
                         .fill=fake_fill,.user=&d,.deadline_us=1000};
    pickmodal_open(&p,&cfg);
    CHECK(!pickmodal_expired(&p,999),"not yet");
    CHECK(!pickmodal_expired(&p,1000),"not at the deadline itself");
    CHECK(pickmodal_expired(&p,1001),"expired after it");
    // A zero deadline is "no deadline", which is what a native caller with no
    // promise to settle wants. A picker that expired at once would close
    // before the person saw it.
    cfg.deadline_us=0;
    pickmodal_open(&p,&cfg);
    CHECK(!pickmodal_expired(&p,1<<30),"zero means no deadline");
}

int main(void) {
    empty_list();
    short_list();
    long_list();
    exact_window();
    cursor_stays_visible();
    list_shrinks();
    reload_resets();
    deadline();
    printf(failures?"FAILURES %d\n":"ok\n",failures);
    return failures?1:0;
}
