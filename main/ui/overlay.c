#include "overlay.h"
#include "overlay_core.h"
#include "app_session.h"
#include "app_registry.h"
#include "pocket_overlay.h"
#include "board.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG="overlay";

extern const char deskclock_start[] asm("_binary_deskclock_js_start");
extern const char deskclock_end[]   asm("_binary_deskclock_js_end");
extern const char player_start[] asm("_binary_player_js_start");
extern const char player_end[]   asm("_binary_player_js_end");

// The registration an overlay gets instead of a row in shell.c's apps[]. An
// overlay is not launched from the Apps list -- it is what the home screen runs
// INSTEAD OF ITS MENU (3.1, revised 2026-09-09) -- so what registers it is a
// Settings choice and this table, which carries what 3.1's manifest carries:
// the id, the region, the frame budget and the source.
typedef struct {
    const char       *id;             // app_registry.c manifest id
    const char       *title;          // the Settings value name, at rest
    const char      **source;         // &start, so the linker symbol resolves late
    const char      **source_end;
    overlay_region_t  region;
    uint32_t          budget_us;
} overlay_app_t;

static const char *deskclock_src, *deskclock_src_end;
static const char *player_src, *player_src_end;

static const overlay_app_t DESKCLOCK = {
    .id="local.deskclock", .title="DESK CLOCK",
    .source=&deskclock_src, .source_end=&deskclock_src_end,
    // A small box, kept where it was. The clock was written against the old
    // rule and does not want the screen; it is the proof that a modest overlay
    // still works now that a large one is allowed.
    .region={.x=140,.y=46,.w=96,.h=22},
    // 8 ms of a 33 ms home frame. shell_draw() already spends 25-41 ms of it
    // depending on the scene, so this is generous rather than tight; what it
    // is really there to catch is an overlay that has started spending
    // hundreds of milliseconds, which is the shape that makes the menu
    // unusable.
    .budget_us=8000,
};

static const overlay_app_t MUSIC = {
    .id="local.player", .title="MUSIC",
    .source=&player_src, .source_end=&player_src_end,
    // The whole panel, which is what 3.1 now allows and what the old rule made
    // impossible: with XMB ended there is nothing underneath to stay clear of.
    // Modals still land on top, and they draw the whole screen themselves.
    .region={.x=0,.y=0,.w=LCD_W,.h=LCD_H},
    // More than the clock, because it holds a file open and reads the card
    // inside its turn. Still a small fraction of the frame: what this catches
    // is a turn that has stopped returning, not one that is working.
    .budget_us=12000,
};

static const overlay_app_t *const REGISTERED[OVERLAY_APPS] = { &DESKCLOCK, &MUSIC };
// One overlay at a time, and the reason is memory rather than taste: the guest
// heap is sized for one beside a scene, and two of them do not fit. The table
// has rows so the person can CHOOSE which one, not so two can run.
static const overlay_app_t *current;

// ---------------------------------------------------------------- state

static nvs_handle_t     prefs;
static bool             prefs_ready;
static overlay_state_t  state=OVERLAY_OFF;
// The non-volatile "starting" flag AS IT STANDS IN NVS, including the value
// this boot found there. Distinguishing that from "this boot wrote it" is what
// closes the hole where a person who leaves the setting on and picks ON again
// on a BLOCKED device gets a start: the flag is still set, and it is the flag
// and not the setting that decides.
static bool             flag_stored;
static bool             session_up;       // a guest belonging to us exists
// This boot has already seen the overlay start and run healthily. Later starts
// in the same boot -- every return from an app screen is one -- then skip the
// flag, and with it two NVS writes each. What the flag protects against is a
// crash DURING startup, which loops as boot -> start -> crash -> boot; a start
// that has already been proven once this boot cannot be the first link of that
// chain, and the next boot proves it again from scratch.
static bool             proven;
static overlay_budget_t budget;
// One live buffer per overlay. Only the selected one is ever written, so a row
// that is not running keeps its plain title rather than a stale status.
static char             labels[OVERLAY_APPS][20];
const char *const overlay_choice_names[1+OVERLAY_APPS]={"OFF",labels[0],labels[1]};
_Static_assert(OVERLAY_APPS==2,"overlay_choice_names lists one buffer per app");
// 0 = off, 1..OVERLAY_APPS = REGISTERED[choice-1].
static unsigned         choice;

static void label_reset(unsigned which) {
    if(which<1||which>OVERLAY_APPS) return;
    snprintf(labels[which-1],sizeof labels[0],"%s",REGISTERED[which-1]->title);
}

static void say(const char *suffix) {
    if(!choice) return;
    snprintf(labels[choice-1],sizeof labels[0],"%s %s",
             REGISTERED[choice-1]->title,suffix);
}
// The one line a host script and a person read the same fact from.
static void announce(const char *what) {
    ESP_LOGI(TAG,"OVERLAY %s state=%d",what,(int)state);
}

// ------------------------------------------------------- the crash flag
//
// 3.1's most important valve, and the only one whose absence bricks the
// device: an overlay that faults during startup would otherwise be started
// again by the next boot, for ever. The flag goes down before the start and
// comes up only after the session has run healthily, so "still set at boot"
// means "the last start did not get that far".

static bool flag_set(bool on) {
    if(!prefs_ready) return false;
    esp_err_t err=nvs_set_u8(prefs,"starting",on?1:0);
    if(err==ESP_OK) err=nvs_commit(prefs);
    if(err!=ESP_OK) { ESP_LOGW(TAG,"flag write failed: %s",esp_err_to_name(err)); return false; }
    flag_stored=on;
    return true;
}

void overlay_init(void) {
    // The table holds pointers to these rather than the symbols themselves,
    // because a `.source` initialiser would need the address of an extern array
    // at file scope and that is fine -- but the END pointer is only ever used as
    // a length, and keeping both here puts the arithmetic in one place.
    deskclock_src=deskclock_start; deskclock_src_end=deskclock_end;
    player_src=player_start;       player_src_end=player_end;
    uint8_t stored=0;
    if(nvs_open("overlay",NVS_READWRITE,&prefs)==ESP_OK) {
        prefs_ready=true;
        if(nvs_get_u8(prefs,"starting",&stored)!=ESP_OK) stored=0;
    }
    flag_stored=stored!=0;
    for(unsigned i=1;i<=OVERLAY_APPS;i++) label_reset(i);
    state=overlay_boot_state(choice!=0,flag_stored);
    if(state==OVERLAY_BLOCKED) {
        // Deliberately left in NVS. Clearing it here would re-arm the device on
        // the boot after this one without anybody having decided anything.
        say("BLOCKED");
        ESP_LOGW(TAG,"OVERLAY_BLOCKED previous start did not complete");
    } else if(state==OVERLAY_STARTING) say("...");
    announce("init");
}

unsigned overlay_armed_get(void) { return choice; }

void overlay_armed_set(unsigned value) {
    if(value>OVERLAY_APPS) return;
    // Choosing the SAME overlay while it is already up or coming up is not a
    // request for anything. Choosing it again after a refusal or a stop IS a
    // retry, which is why those two states fall through. Choosing a DIFFERENT
    // one always acts: the running session has to end for the other to start.
    if(value && value==choice &&
       (state==OVERLAY_RUNNING||state==OVERLAY_STARTING)) return;
    unsigned was=choice;
    choice=value;
    if(was && was!=value) { overlay_release(); label_reset(was); }
    if(!choice) {
        overlay_release();
        state=OVERLAY_OFF;
        // Turning it off is the human act 3.1 requires before a blocked
        // overlay may run again, so this is where the flag is cleared.
        flag_set(false);
        proven=false;
        announce("off");
        return;
    }
    // Arming clears nothing. A device whose flag is still set stays blocked
    // however many times ON is chosen: the only thing that clears the flag is
    // the branch above, so the human act 3.1 requires is specifically turning
    // it OFF and then on again -- not pressing Enter twice on ON.
    if(flag_stored) {
        state=OVERLAY_BLOCKED;
        say("BLOCKED");
        ESP_LOGW(TAG,"OVERLAY_BLOCKED turn it off first");
        return;
    }
    state=OVERLAY_STARTING;
    say("...");
    announce("armed");
}

// ------------------------------------------------------------- start/stop

static void stop_with(const char *label, overlay_state_t next) {
    if(session_up) { app_stop(); session_up=false; }
    if(flag_stored) flag_set(false);
    state=next;
    say(label);
    ESP_LOGW(TAG,"OVERLAY_STOPPED %s worst=%uus turns=%u",
             label,(unsigned)budget.worst_us,(unsigned)budget.turns);
}

void overlay_release(void) {
    if(session_up) {
        app_stop(); session_up=false;
        // Back to armed-but-not-up, so the home screen starts it again when it
        // gets the frame back. A release is not a fault, so REFUSED and
        // STOPPED are left standing: those are decisions, and giving the
        // display to another screen does not reverse them.
        if(choice && state==OVERLAY_RUNNING) { state=OVERLAY_STARTING; say("..."); }
    }
    if(flag_stored) flag_set(false);
    pocket_overlay_reset();
}

// ------------------------------------------------------- room, before and after
//
// WHAT THE RISK ACTUALLY IS. 3.1 asks for a reservation because the Rust UI
// core does not report running out of memory -- it panics and reboots. That
// hazard is not present here: an overlay session has no Rust UI core (see
// pocket_overlay.h for why it was left out), and a guest allocator that runs
// short raises an OutOfMemory exception, which comes back through
// app_start_overlay() as an ordinary error and is refused below. The overlay
// cannot take the device down by running out of room.
//
// What it CAN do is DISPOSSESSION: quietly take memory that something else
// needs later, so that the radio will not come up or a stream will not play,
// and the failure surfaces somewhere with no visible connection to a clock.
// That is the risk this pair of checks is for, and naming it correctly is what
// makes the second one obviously necessary and the first one obviously
// insufficient.
//
// THE FLOOR. NET_RADIO_MIN_FREE in pocket/pocket_net.c: 56 KiB, measured on
// the board on 2026-09-07, the cost of bringing the radio up. It is the
// largest recurring claim anything on this device makes, so an overlay that
// leaves at least that much has not taken away an ability the machine had
// before it started. The number is repeated here rather than shared through a
// header because pocket_net.c is another agent's file and is in flight; if
// that constant moves, this one has to move with it, and that duplication is a
// debt to settle rather than a design.
#define OVERLAY_FREE_FLOOR (56*1024)

static uint32_t free_internal(void) {
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}

// What an overlay session is EXPECTED to cost the device heap, which is a
// different quantity from OVERLAY_GUEST_HEAP and must not be confused with it
// again. That one is a ceiling QuickJS is not allowed to exceed; this one is
// what it is likely to actually spend, and only this one belongs in a gate
// about whether there is room.
//
// DERIVED, NOT MEASURED, and labelled so deliberately. It comes from the two
// recorded analogues -- 85,602 bytes of guest heap for hello/main.js before
// the pocket surfaces existed (app_session.c), and js=86,233 read off the
// board for a running app -- rounded up for the parse peak. The overlay's own
// figure has never been taken, because the first attempt to take it refused
// before it got there. app_report() logs `js=` at the end of every start, so
// the next successful run replaces this derivation with a measurement, and it
// should be replaced rather than left standing.
#define OVERLAY_EXPECTED_COST (96*1024)

// The cheap gate, and NOT the guarantee.
//
// Two things it is no longer: it does not ask for one contiguous block, because
// QuickJS grows in many small allocations and never wants the whole cap in one
// piece; and it does not use the cap as the cost, because a ceiling is not a
// price. It asks only whether the total is plausible, so the obvious "no" costs
// nothing. The answer that counts is taken after the start, from what happened.
static bool plausible(void) {
    return free_internal()>=OVERLAY_EXPECTED_COST+OVERLAY_FREE_FLOOR;
}

static void start(void) {
    if(!choice) return;
    const overlay_app_t *o=REGISTERED[choice-1];
    current=o;
    uint32_t before=free_internal();
    if(!plausible()) {
        state=OVERLAY_REFUSED;
        say("NO ROOM");
        ESP_LOGW(TAG,"OVERLAY_REFUSED_GATE free=%u needs=%u expected=%u floor=%u",
                 (unsigned)before,
                 (unsigned)(OVERLAY_EXPECTED_COST+OVERLAY_FREE_FLOOR),
                 (unsigned)OVERLAY_EXPECTED_COST,(unsigned)OVERLAY_FREE_FLOOR);
        return;
    }
    // Written and committed BEFORE the start, and this ordering is the valve.
    if(!proven && !flag_set(true)) {
        // No flag means no way to notice a crash, and 3.1 would rather not run
        // than run unprotected -- the failure it protects against is a device
        // that cannot be reached at all.
        state=OVERLAY_REFUSED;
        say("NO FLAG");
        return;
    }
    pocket_overlay_set_region(&o->region);
    app_registry_select(o->id);
    // The -1 is the NUL that EMBED_TXTFILES appends and the guest must not see.
    esp_err_t err=app_start_overlay(*o->source,
                                    (size_t)(*o->source_end-*o->source-1));
    if(err!=ESP_OK) {
        // A refusal the app_session reported is not a crash: the guest never
        // ran, so the flag comes down and the person is told why.
        flag_set(false);
        state=OVERLAY_REFUSED;
        say("START FAIL");
        // Named apart from the gate's refusal on purpose. These two were both
        // "OVERLAY_REFUSED ..." for one build, and the board test that followed
        // was read as the gate having fired when it had passed -- the cause was
        // inside guest creation, four hundred bytes away in the source and a
        // long way away in meaning.
        ESP_LOGW(TAG,"OVERLAY_REFUSED_START %s free=%u cap=%u",
                 esp_err_to_name(err),(unsigned)free_internal(),
                 (unsigned)OVERLAY_GUEST_HEAP);
        return;
    }
    session_up=true;
    // Before the verify below, so a stand-down reports this session's counters
    // (no turns yet) rather than the previous session's.
    budget=(overlay_budget_t){.budget_us=o->budget_us,.over_limit=60,
                              .healthy_us=5000000};
    overlay_budget_start(&budget,(uint64_t)esp_timer_get_time());
    // VERIFY, having predicted. The guest is up, so what it cost is no longer a
    // forecast -- it is a subtraction. Standing down here puts the machine back
    // exactly where it was, which is the whole reason this check can exist for
    // an overlay and not for a foreground app.
    uint32_t after=free_internal();
    ESP_LOGI(TAG,"OVERLAY_COST free_before=%u free_after=%u took=%d floor=%u",
             (unsigned)before,(unsigned)after,(int)before-(int)after,
             (unsigned)OVERLAY_FREE_FLOOR);
    if(!overlay_room_left(after,OVERLAY_FREE_FLOOR)) {
        // Not "the overlay failed": the overlay succeeded and the machine is
        // worse off, which is the case 3.1 had no name for. The reason is
        // stateable -- the radio could no longer be brought up -- and it is
        // shown in the row the person would turn it off from.
        stop_with("NO ROOM",OVERLAY_REFUSED);
        ESP_LOGW(TAG,"OVERLAY_REFUSED_FLOOR free=%u below floor=%u",
                 (unsigned)after,(unsigned)OVERLAY_FREE_FLOOR);
        return;
    }
    state=OVERLAY_RUNNING;
    say("...");
    announce("running");
}

void overlay_tick(uint32_t frame_us) {
    if(state==OVERLAY_STARTING) { start(); return; }
    if(state!=OVERLAY_RUNNING) return;

    int64_t began=esp_timer_get_time();
    esp_err_t err=app_overlay_tick();
    uint32_t spent=(uint32_t)(esp_timer_get_time()-began);
    if(err!=ESP_OK) { stop_with("FAULTED",OVERLAY_STOPPED); return; }
    if(overlay_budget_turn(&budget,spent,frame_us)) {
        // 3.1: stopped, and the stop is SHOWN. The Settings row is where the
        // person would go to turn it off, so it is where they are told it
        // already stopped -- a silent stop is indistinguishable from a fault.
        stop_with("OVER BUDGET",OVERLAY_STOPPED);
        return;
    }
    if(flag_stored && overlay_budget_healthy(&budget,(uint64_t)began)) {
        proven=true;
        flag_set(false);
        ESP_LOGI(TAG,"OVERLAY_HEALTHY worst=%uus",(unsigned)budget.worst_us);
    }
    // The frame cost, where the person deciding whether to keep it can read it.
    char cost[12];
    snprintf(cost,sizeof cost,"%u.%ums",
             (unsigned)(budget.last_us/1000),(unsigned)((budget.last_us%1000)/100));
    say(cost);
}

// The hook for the direction that dissolves the prediction problem rather than
// improving the prediction, and it HAS NO CALL SITES YET on purpose.
//
// Every check above is a guess about a future claimant taken at overlay start.
// The alternative is to stop guessing: let the claimant say so at the moment it
// actually needs the room, which is a single identifiable point in its own code
// -- esp_wifi_init in pocket_net.c, the decoder's buffers in pocket_av.c. An
// overlay that yields there needs no forecast at all, because by then the
// question is not "will something need this" but "this needs it now".
//
// The call sites are not added here because those two files belong to another
// workstream that is mid-flight on exactly the case this serves (Wi-Fi and
// Opus playback at once), and where the right moments are may depend on
// numbers that do not exist yet. This end is ready: it is idempotent, it is
// safe from any task that already calls into the shell, and it leaves the row
// saying why.
void overlay_yield(const char *claimant) {
    if(state!=OVERLAY_RUNNING && !session_up) return;
    ESP_LOGW(TAG,"OVERLAY_YIELDED to %s free=%u",
             claimant?claimant:"?",(unsigned)free_internal());
    stop_with("YIELDED",OVERLAY_STOPPED);
}

void overlay_paint(uint16_t *strip, int strip_y, int strip_h) {
    if(state!=OVERLAY_RUNNING) return;
    pocket_overlay_paint(strip,strip_y,strip_h);
}

bool overlay_running(void) { return state==OVERLAY_RUNNING; }

// The residue, and nothing else reaches here. main.c takes the reserved key
// first and never calls this with it -- structurally, by returning before this
// line, so "the overlay cannot receive the way out" is a property of the call
// site rather than a filter this file could get wrong.
void overlay_key(const keystroke_t *k) {
    if(state!=OVERLAY_RUNNING||!k) return;
    pocket_overlay_key(k);
}
