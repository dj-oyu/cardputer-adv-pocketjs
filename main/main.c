#include "board.h"
#include "shell.h"
#include "overlay.h"
#include "motion.h"
#include "sound.h"
#include "keymap.h"
#include "editor.h"
#include "codeedit.h"
#include "wifi_ui.h"
#include "tutorial.h"
#include "jpfont.h"
#include "skk_session.h"
#include "app_session.h"
#include "pocket_workspace.h"
#include "sd_picker.h"
#include "file_picker.h"
#include "overlay.h"
#include "app_registry.h"
#include "pet_hub.h"
#include "pocket_capture.h"
#include "pocket_bridge.h"
#include "pocket_text.h"
#include "scene_mem.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <string.h>

static QueueHandle_t keys;
static atomic_bool stop;
static atomic_bool capture;
static atomic_int diagnostic;
// Whether the screen on show consumes typed bytes. Only the USB reading needs
// it: the same byte is a menu direction on the home screen and a character
// everywhere else.
static atomic_bool text_screen;
static bool pet_repaint;

// USB drives the home screen with single letters, but an editor needs the
// bytes themselves so a host script can type at it. 0x1b closes either way.
static bool usb_stroke(char c, keystroke_t *k) {
    if(pocket_bridge_usb((uint8_t)c))return false;
    if(pet_hub_usb((uint8_t)c))return false;
    memset(k,0,sizeof(*k));
    if(atomic_load(&text_screen)) {
        // C-s is the editor's save, so the host capture moves to C-p.
        if(c==0x10) { atomic_store(&capture,true); return false; }
        if(c==27) { k->text[0]='\0';memcpy(k->text+1,"esc",3);k->len=4;k->nav=KEY_BACK;return true; }
        if(c=='\r'||c=='\n') { k->text[0]='\n';k->len=1;k->nav=KEY_ENTER;return true; }
        if(c=='\b'||c==0x7f) { k->text[0]='\b';k->len=1;return true; }
        if(c==0x0b) { k->toggle_ime=true;return true; }   // C-k stands in for C-j
        // The arrows and Del are Fn combinations on the keyboard and have no
        // byte of their own, so a host driving this over USB gets these.
        if(c==0x02) { k->text[0]='\0';memcpy(k->text+1,"left",4);k->len=5;k->nav=KEY_LEFT;return true; }
        if(c==0x06) { k->text[0]='\0';memcpy(k->text+1,"right",5);k->len=6;k->nav=KEY_RIGHT;return true; }
        if(c==0x04) { k->text[0]='\0';memcpy(k->text+1,"del",3);k->len=4;return true; }
        k->text[0]=c;k->len=1;return true;
    }
    if(c=='s') { atomic_store(&capture,true); return false; }
    if(c=='c') { motion_recenter(); return false; }
    // '8' is not an app: it checks the baked sound tables against this chip's
    // own libm (sound_check_tables), and is handled where the others start. It
    // is not folded into the range because '7' has no diagnostic behind it and
    // would silently start the default app.
    // '9' is the same kind of thing for the microphone: it sweeps the codec's
    // input paths and its ADC volume and prints what each produces, because the
    // board answered the first register table with silence and guessing again
    // is not a method. Like every letter in this function it arrives over USB;
    // the Cardputer's own '9' key goes to the shell and does nothing here.
    if((c>='1'&&c<='6')||c=='8'||c=='9') { atomic_store(&diagnostic,c); return false; }
    // The volume pair, as TEXT rather than as nav, because that is what the
    // Cardputer's own keys produce and what volume_key() reads. Without these
    // two a host script could reach every other key on the home screen and not
    // the only two that are the shell's everywhere.
    if(c=='-'||c=='=') { k->text[0]=c;k->len=1;return true; }
    if(c=='\r'||c=='\n'||c=='e')k->nav=KEY_ENTER;
    else if(c=='q'||c==27)k->nav=KEY_BACK;
    else if(c=='b')k->nav=KEY_RIGHT;
    else if(c=='a')k->nav=KEY_LEFT;
    else if(c=='u')k->nav=KEY_UP;
    else if(c=='d')k->nav=KEY_DOWN;
    else return false;
    return true;
}

static void input_task(void *arg) {
    (void)arg;
    while(1) {
        keystroke_t k;
        bool have=keymap_poll(&k);
        motion_poll();
        char c;
        if(!have && usb_serial_jtag_read_bytes(&c,1,0)>0) have=usb_stroke(c,&k);
        if(have) {
            // Only the force stop jumps the queue, and it does so because the
            // drawing task may be inside a guest call that has to be
            // interrupted rather than waited out. Everything else is ordered.
            if(k.force_stop) { atomic_store(&stop,true); app_request_stop(); }
            else xQueueSend(keys,&k,0);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------------------------------------------------------------------
// Screens.
//
// One task draws, and only one screen is up at a time. They were four nested
// branches in the loop below, each repeating the capture handling, the repaint
// test and the frame pacing; the two most recent bugs were both a branch that
// copied three of those four correctly. The table makes each screen say what
// it is instead, and the loop does the repeated parts once.
// ---------------------------------------------------------------------------

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_PRACTICE,
    SCREEN_CODE,
    SCREEN_TUTORIAL,
    SCREEN_WIFI,
    SCREEN_COUNT
} screen_id_t;

typedef struct {
    const char *tag;                 // log tag
    const char *ready;               // logged on arrival
    void (*open)(void);
    // The other end of open(): called on the screen being left, so a screen
    // that took memory to be on show can give it back. Without it every screen
    // held its buffers from boot to power-off whether it was up or not.
    void (*close)(void);
    bool (*key)(const keystroke_t *k);   // false: leave for the home screen
    bool (*dirty)(void);
    void (*draw)(void);
    // Set when the screen can run JavaScript. Returns true once it wants to.
    bool (*wants_run)(const char **source, size_t *len);
    // What has to be evaluated before that source, in the same realm, or NULL.
    // Only a source the screen asked to run carries it: an app started from the
    // home screen or by pocket.workspace.run() is a whole program of its own,
    // and those call sites pass NULL rather than reading this.
    const char *(*prelude)(size_t *len);
    // How the run ended, for the screen that asked for it.
    void (*ended)(esp_err_t started, const char *error);
    // The buffers a screen lends the parser. run_release() is called once the
    // guest has finished reading the source -- not before: app_start_source()
    // parses straight out of the caller's bytes -- and run_restore() before
    // ended(), so whatever ended() reads is back. A screen that holds 8 KB of
    // editable source is dead for the whole of a run, which is the one stretch
    // in which the guest, the font atlas and the radio all want that block.
    void (*run_release)(void);
    void (*run_restore)(void);
    // For a screen that sends only the strips it changed. Two things make that
    // record wrong from outside: board_capture prints the strips that are
    // sent, so a partial frame is a partial screenshot, and pet_hub_overlay
    // rides on board_present, so the pet only moves on the strips that go out.
    // Both are announced here rather than guessed at by the screen.
    void (*repaint_all)(void);
    uint8_t frame_ms;
    bool takes_text;
} screen_ops_t;

static screen_id_t screen=SCREEN_HOME;
static bool        running;          // a guest owns the display
static screen_id_t owner;            // which screen asked for it
static esp_err_t   run_started;
static const char *home_error;
static unsigned    home_phase;

// The home screen predates this table and keeps its own shapes, so it is
// adapted rather than changed.
static bool home_dirty(void) { return true; }   // the background animates
static void home_draw(void)  { shell_draw(home_error,home_phase++); }

static void enter(screen_id_t next);
static void begin_run(const char *app_id, const char *prelude, size_t prelude_len,
                      const char *source, size_t len);

// The calibration program is embedded rather than kept in a source slot: it is
// the thing you reach for when the sensor is wrong, and a slot someone has
// edited is exactly what you cannot trust at that moment.
extern const char imucal_start[] asm("_binary_imucal_js_start");
extern const char imucal_end[]   asm("_binary_imucal_js_end");
extern const char pet_start[] asm("_binary_pet_js_start");
extern const char pet_end[] asm("_binary_pet_js_end");
extern const char companion_start[] asm("_binary_companion_js_start");
extern const char companion_end[] asm("_binary_companion_js_end");
extern const char streamplay_start[] asm("_binary_streamplay_js_start");
extern const char streamplay_end[] asm("_binary_streamplay_js_end");
extern const char opusplay_start[] asm("_binary_opusplay_js_start");
extern const char opusplay_end[] asm("_binary_opusplay_js_end");
extern const char opusfit_start[] asm("_binary_opusfit_js_start");
extern const char opusfit_end[] asm("_binary_opusfit_js_end");
extern const char mp3play_start[] asm("_binary_mp3play_js_start");
extern const char mp3play_end[] asm("_binary_mp3play_js_end");

// shell_key() cannot say "hand the display to another screen": its bool already
// means "launch the app shell_app() names". The request is left behind instead,
// and every shell_key() call site has to collect it — one that does not leaves
// the request standing until the next press, which then opens the screen on the
// wrong key.
static void take_pending_screen(void) {
    switch(shell_pending_screen()) {
        case SHELL_SCREEN_WIFI: enter(SCREEN_WIFI); break;
        case SHELL_SCREEN_NONE: break;
    }
}

// The volume keys, and they belong to the SHELL rather than to whatever is on
// the home screen. Pressed while the player is up they change the volume, which
// is what makes volume operable from the player -- but the player is not what
// changes it, and no app can. An app that could turn itself up is an app that
// could turn itself up while nobody is watching, and this keeps the practical
// half of that without the dangerous half.
//
// They cost two keys out of the residue 3.1 hands to an overlay, which is a
// real price and is why it is two and not a row of them. `-` and `=` are the
// unshifted pair on the top row; the shifted spellings are accepted because a
// person holding shift meant the same thing.
//
// Taken on the home screen only. A foreground app receives every key and having
// two of them silently disappear is a worse surprise than reaching for Settings.
static bool volume_key(const keystroke_t *k) {
    if(!k||k->len!=1) return false;
    char c=k->text[0];
    int step=(c=='-'||c=='_')?-1:((c=='='||c=='+')?1:0);
    if(!step) return false;
    unsigned now=sound_volume();
    if(step<0&&now) sound_set_volume(now-1);
    else if(step>0&&now+1<SOUND_VOLUME_STEPS) sound_set_volume(now+1);
    // Shown and stored even when the step was clamped: pressing down at the
    // bottom is a person asking where they are, and answering with nothing is
    // indistinguishable from a key that did not arrive.
    shell_volume_touched();
    shell_settings_save();
    return true;
}

// Whether a host screen is up over the home screen. 3.1: modals beat the
// overlay, and that has to include the RESERVED KEY -- Escape on a folder
// picker means "I decline", and spending it on standing the overlay down would
// leave the picker up with its promise unsettled and no way to answer it.
static bool home_modal(void) {
    return pocket_workspace_modal()||sd_picker_modal()||file_picker_modal();
}

static bool home_key(const keystroke_t *k) {
    board_key_t nav=k->nav;
    if(nav==KEY_NONE) return true;
    // An app that failed leaves its reason on the home screen. Either key
    // dismisses it, and the screen says so, which is what a host driving this
    // over USB waits for.
    if(home_error) {
        if(nav==KEY_ENTER||nav==KEY_BACK) {
            home_error=NULL;
            ESP_LOGI("shell","HOME_READY");
        }
        return true;
    }
    bool launch=shell_key(nav);
    take_pending_screen();
    if(!launch) return true;
    switch(shell_app()) {
        case 1: enter(SCREEN_PRACTICE); break;
        case 2: enter(SCREEN_CODE); break;
        case 3: enter(SCREEN_TUTORIAL); break;
        case 4: begin_run("local.imucal",NULL,0,imucal_start,(size_t)(imucal_end-imucal_start-1)); break;
        case 5: begin_run("local.pet",NULL,0,pet_start,(size_t)(pet_end-pet_start-1)); break;
        case 6: begin_run("local.companion",NULL,0,companion_start,(size_t)(companion_end-companion_start-1)); break;
        case 7: begin_run("local.streamplay",NULL,0,streamplay_start,(size_t)(streamplay_end-streamplay_start-1)); break;
        case 8: begin_run("local.opusplay",NULL,0,opusplay_start,(size_t)(opusplay_end-opusplay_start-1)); break;
        case 9: begin_run("local.opusfit",NULL,0,opusfit_start,(size_t)(opusfit_end-opusfit_start-1)); break;
        case 10: begin_run("local.mp3play",NULL,0,mp3play_start,(size_t)(mp3play_end-mp3play_start-1)); break;
        default: begin_run("local.hello",NULL,0,NULL,0);          // the built-in app
    }
    return true;
}

static bool code_wants_run(const char **source, size_t *len) {
    if(code_state()!=CODE_RUNNING) return false;
    *source=code_source(len);
    return true;
}
static void code_ended(esp_err_t started, const char *error) {
    // A source with no frame drew what it drew and has nothing to run, so its
    // output is the result rather than a failure.
    if(started==ESP_ERR_NOT_FOUND)  code_returned("EVALUATED (NO frame)");
    else if(started!=ESP_OK)        code_returned(error[0]?error:"START FAILED");
    else                            code_returned(error[0]?error:NULL);
}

static bool tutorial_wants_run(const char **source, size_t *len) {
    if(tutorial_state()!=TUTORIAL_WRITING || code_state()!=CODE_RUNNING) return false;
    *source=tutorial_source(len);
    return true;
}

static const screen_ops_t SCREENS[SCREEN_COUNT]={
    [SCREEN_HOME]={
        .tag="shell", .ready="HOME_READY",
        .key=home_key, .dirty=home_dirty, .draw=home_draw,
        .frame_ms=33,
    },
    [SCREEN_PRACTICE]={
        .tag="editor", .ready="EDITOR_READY", .open=editor_open,
        .key=editor_key, .dirty=editor_dirty, .draw=editor_draw,
        .frame_ms=16, .takes_text=true,
    },
    [SCREEN_CODE]={
        .tag="code", .ready="CODE_READY", .open=code_open, .close=code_close,
        .key=code_key, .dirty=code_dirty, .draw=code_draw,
        .wants_run=code_wants_run, .ended=code_ended,
        .run_release=code_run_release, .run_restore=code_run_restore,
        .repaint_all=code_repaint_all,
        .frame_ms=16, .takes_text=true,
    },
    [SCREEN_TUTORIAL]={
        .tag="tutorial", .ready="TUTORIAL_READY", .open=tutorial_open,
        .close=tutorial_close,
        .key=tutorial_key, .dirty=tutorial_dirty, .draw=tutorial_draw,
        .wants_run=tutorial_wants_run, .prelude=tutorial_prelude,
        .ended=tutorial_ran,
        .run_release=tutorial_run_release, .run_restore=tutorial_run_restore,
        .frame_ms=16, .takes_text=true,
    },
    // Nothing on this screen animates; it repaints when a key or the sync task
    // moves it. 33 ms is the pace of waiting for a radio, not of typing.
    [SCREEN_WIFI]={
        .tag="wifi_ui", .ready="WIFI_READY", .open=wifi_ui_open,
        .key=wifi_ui_key, .dirty=wifi_ui_dirty, .draw=wifi_ui_draw,
        .frame_ms=33, .takes_text=true,
    },
};

static void enter(screen_id_t next) {
    // docs/common-api.md 3.1 moves the guest's lifetime from "entering and
    // leaving the app screen" to "the home screen owning the frame", and this
    // is the leaving half: the home screen is giving the display away, so the
    // session it owns ends here. It has to be before the screen changes,
    // because app_stop() tears down surfaces that the arriving screen may
    // immediately build again.
    if(next!=SCREEN_HOME) overlay_release();
    if(SCREENS[screen].close) SCREENS[screen].close();
    screen=next;
    atomic_store(&text_screen,SCREENS[next].takes_text);
    if(SCREENS[next].open) SCREENS[next].open();
    ESP_LOGI(SCREENS[next].tag,"%s",SCREENS[next].ready);
}

static void go_home(void) {
    sound_play(2);
    enter(SCREEN_HOME);
}

static void take_pending_run(void);

static void end_run(esp_err_t tick_err) {
    app_stop();
    running=false;
    xQueueReset(keys);
    const screen_ops_t *o=&SCREENS[owner];
    // Before ended(): the tutorial's verdicts read the editor's source, and the
    // editor's own ended() draws it.
    if(o->run_restore) o->run_restore();
    const char *why=app_error();
    if(o->ended) o->ended(run_started, tick_err==ESP_OK?why:(why[0]?why:"EXECUTION FAILED"));
    else {
        if(tick_err!=ESP_OK) home_error="EXECUTION FAILED";
        sound_play(2);
        ESP_LOGI("shell","HOME_READY");
    }
    // A session that ended because it called pocket.workspace.run() hands the
    // screen straight to the work it named: section 7 has the current session
    // end and the target take over, with no second program running at any
    // moment. The screen the run belongs to is unchanged, so Back still leaves
    // to where the person started.
    take_pending_run();
}

static void take_pending_run(void) {
    const char *source=NULL;
    size_t      length=0;
    if(!pocket_workspace_run_take(&source,&length)) return;
    begin_run(APP_ID_WORK,NULL,0,source,length);
    // app_start_source() borrows the bytes only for the length of the start.
    pocket_workspace_run_done();
}

// Which app is starting, by name. Section 3 gives the host the identity, and
// app_session.c reads it back out of the registry to key the stores and to
// check what the manifest requires — so the name here is the whole of the
// decision, and the ternary on a source pointer that used to stand in for it
// is gone.
static void begin_run(const char *app_id, const char *prelude, size_t prelude_len,
                      const char *source, size_t len) {
    owner=screen;
    // The other half of the same rule. Every path that builds a foreground
    // guest comes through here, and app_session.c holds ONE set of statics --
    // so the overlay's session must be gone before this one is built. A path
    // that reached app_start_test() without passing here would silently
    // overwrite a live guest pointer; the one that exists (the USB
    // diagnostics) releases the overlay itself.
    overlay_release();
    // The home screen's background is about to stop being drawn for as long as
    // the guest owns the display, so its scratch stops being worth anything to
    // it and starts being worth a great deal to the guest, the font atlas and
    // the radio. Here rather than in enter(): this is the moment the memory
    // changes hands, and the next prepare() after the run takes it back.
    scene_mem_release();
    app_registry_select(app_id);
    run_started = source ? app_start_source(prelude,prelude_len,source,len)
                         : app_start();
    // The bytes have been parsed, so the screen that lent them can put them
    // back in the heap for as long as the guest is up.
    if(SCREENS[owner].run_release) SCREENS[owner].run_release();
    ESP_LOGI(SCREENS[owner].tag,"RUN %u bytes -> %s",
             (unsigned)len,esp_err_to_name(run_started));
    if(run_started==ESP_OK) { running=true; return; }
    // The guest never came up, so there is nothing to tick or stop; report it
    // to whoever asked and stay where we are.
    app_stop();
    running=false;
    const screen_ops_t *o=&SCREENS[owner];
    if(o->run_restore) o->run_restore();
    if(o->ended) o->ended(run_started,app_error());
    else home_error="START FAILED";
}

// One frame of a running guest. Back returns to the screen that started it,
// except from the home screen, where the whole app is what Back leaves.
static void tick_run(bool have, const keystroke_t *stroke) {
    // The works picker is a host screen over a live guest: while it is up the
    // guest is not ticked and the keys are the picker's. Its promise settles on
    // the turn after it closes, which is the first turn the guest runs again.
    if(pocket_workspace_modal()) {
        if(have) pocket_workspace_modal_key(stroke);
        if(pocket_workspace_modal_dirty()) pocket_workspace_modal_draw();
        if(!pocket_workspace_modal()) app_force_redraw();
        return;
    }
    // fs.requestFolder puts up the same kind of screen for the same kind of
    // reason: the person choosing a folder on the card IS the grant, so the
    // guest waits while they do it. An app that opened both in one turn gets
    // them in this order rather than on top of each other; the second one's
    // deadline keeps running while it waits, which is what a person who is
    // being asked two questions at once would expect of the second.
    if(sd_picker_modal()) {
        if(have) sd_picker_modal_key(stroke);
        if(sd_picker_modal_dirty()) sd_picker_modal_draw();
        if(!sd_picker_modal()) app_force_redraw();
        return;
    }
    // fs.pickFile is the same screen again, one grant further in: the person
    // choosing a file is choosing convenience rather than permission, so this
    // one comes after the grant picker for the same reason the grant picker
    // comes after the works picker -- two questions asked in one turn are
    // answered in the order they would have to be answered anyway.
    if(file_picker_modal()) {
        if(have) file_picker_modal_key(stroke);
        if(file_picker_modal_dirty()) file_picker_modal_draw();
        if(!file_picker_modal()) app_force_redraw();
        return;
    }
    // A text session takes the keyboard from the guest for as long as it is
    // open: section 6 gives the HOST the field, so the app receives neither the
    // keystrokes nor the Back that would otherwise end it -- Escape there means
    // "cancel the conversion, or the field", and both are pocket_text's.
    if(have && pocket_text_active()) {
        pocket_text_key(stroke);
        have=false;
        // The guest's renderer presents only when IT has damage, so a field
        // that moved on a frame the app did not would not be sent at all.
        if(pocket_text_take_dirty()) app_force_redraw();
    }
    board_key_t key=have?stroke->nav:KEY_NONE;
    bool leave = have && key==KEY_BACK;
    esp_err_t e=ESP_OK;
    // Let the guest persist its last state before cancellation tears it down.
    if(leave) { e=app_tick(0x2000); app_request_stop(); }

    bool shot=atomic_exchange(&capture,false);
    if(shot) { board_capture(true); app_force_redraw(); }
    if(!leave) {
        if(key==KEY_ENTER) sound_play(1);
        uint32_t buttons=key==KEY_ENTER?0x4000:key==KEY_UP?0x10:
                         key==KEY_RIGHT?0x20:key==KEY_DOWN?0x40:key==KEY_LEFT?0x80:0;
        e=app_tick(buttons);
        // A release frame, so consecutive queued presses stay distinct.
        if(e==ESP_OK && buttons) e=app_tick(0);
    }
    if(shot) board_capture(false);
    // Again after the turn, for the field the app itself moved: open() and
    // close() are ordinary JS calls and can happen in a frame with no key and
    // no damage of the guest's own. The repaint lands on the next frame, which
    // is 33 ms and is why the check above exists as well -- a keystroke has to
    // be seen in the frame it was typed in.
    if(pocket_text_take_dirty()) app_force_redraw();
    // The recording indicator is composited into every strip that is sent, so
    // it needs a strip to be sent: an app that has stopped drawing would
    // otherwise leave the screen it last drew, with no dot on it.
    if(pocket_capture_take_dirty()) app_force_redraw();

    // Accepted this turn, so no further input reaches the app: the session ends
    // here and end_run() starts what it asked for.
    if(!leave && e==ESP_OK && pocket_workspace_run_requested()) { end_run(ESP_OK); return; }
    if(leave || e!=ESP_OK) end_run(e);
}

// Repaint, and report what one costs. The frame cap would hide it otherwise.
static void paint(const screen_ops_t *s) {
    int64_t began=esp_timer_get_time();
    bool shot=atomic_exchange(&capture,false);
    if(!shot && !pet_repaint && !s->dirty()) return;
    if((shot||pet_repaint) && s->repaint_all) s->repaint_all();
    if(shot) board_capture(true);
    s->draw();
    if(shot) board_capture(false);
    if(shot || s->frame_ms>=33) return;      // the home screen logs its own
    static int64_t sum, peak; static unsigned n;
    int64_t work=esp_timer_get_time()-began;
    sum+=work; if(work>peak) peak=work;
    if(++n==8) { ESP_LOGI(s->tag,"REPAINT avg=%lldus max=%lldus",sum/8,peak);
                 sum=0; peak=0; n=0; }
}

static void ui_task(void *arg) {
    (void)arg;
    ESP_LOGI("shell","ui runs on core %d",xPortGetCoreID());
    ESP_LOGI("shell","HOME_READY");
    while(1) {
        int64_t frame_start=esp_timer_get_time();
        keystroke_t stroke={0};
        bool have=xQueueReceive(keys,&stroke,0)==pdTRUE;
        // Before everything: the volume is the device's, so it is answered
        // before any question about who owns the screen.
        if(have&&!running&&screen==SCREEN_HOME&&!home_modal()&&volume_key(&stroke))
            have=false;
        pet_repaint=pet_hub_pump();
        if(have&&pet_hub_key(stroke.nav)){have=false;pet_repaint=true;}
        if(pet_repaint&&running)app_force_redraw();
        const screen_ops_t *s=&SCREENS[screen];

        // The force stop, and Back on the home screen, arrive out of band.
        if(atomic_exchange(&stop,false)) {
            if(running) end_run(ESP_OK);
            else if(screen!=SCREEN_HOME) go_home();
            else if(home_error) home_error=NULL;
            // The force stop stands an overlay down too. This is NOT the
            // reserved key -- this block only runs for Ctrl+Alt+Del, which is
            // why putting the reserved key here left Back travelling the
            // ordinary path and reaching the guest. The reserved key is in the
            // residue loop below, where every other keystroke is decided.
            else if(overlay_running()&&!home_modal()) overlay_yield("force stop");
            else { shell_key(KEY_BACK); take_pending_screen(); }
            xQueueReset(keys);
            have=false;
        }

        if(running) {
            tick_run(have,&stroke);
        } else {
            // A repaint costs about as much as a whole frame, so taking one
            // key per frame capped typing at the frame rate -- and the queue
            // then threw the rest away without saying so: a 3,055 byte source
            // typed in over USB arrived 621 bytes short. Keys are drained and
            // painted once, which is also the only state anybody can see.
            //
            // A key that moves screens or asks for a run ends the batch, so
            // the screen it moved to gets its own frame rather than being fed
            // the keys meant for the one before it. Both wants_run predicates
            // are pure, which is what makes asking twice per frame free.
            // MODALS BEAT THE OVERLAY (3.1). The same three screens tick_run()
            // drives for a foreground guest, driven here for an overlay one --
            // and this is not symmetry for its own sake: without it a modal an
            // overlay opened would never be drawn and never take a key, so
            // "the shell's prompts win" would be a sentence in a document with
            // nothing behind it. The order is tick_run()'s, for the reason
            // given there.
            if(!running && screen==SCREEN_HOME && overlay_running()) {
                if(pocket_workspace_modal()) {
                    if(have) pocket_workspace_modal_key(&stroke);
                    if(pocket_workspace_modal_dirty()) pocket_workspace_modal_draw();
                    goto framed;
                }
                if(sd_picker_modal()) {
                    if(have) sd_picker_modal_key(&stroke);
                    if(sd_picker_modal_dirty()) sd_picker_modal_draw();
                    goto framed;
                }
                if(file_picker_modal()) {
                    if(have) file_picker_modal_key(&stroke);
                    if(file_picker_modal_dirty()) file_picker_modal_draw();
                    goto framed;
                }
            }
            screen_id_t was=screen;
            // The residue. An overlay has ended the menu, so the keys the menu
            // used to consume are no longer consumed by anything -- that is the
            // premise 3.1's older text was missing, not a rule it got wrong.
            // Back never arrives: it was taken above.
            if(!running && screen==SCREEN_HOME && overlay_running()) {
                while(have) {
                    // THE RESERVED KEY, taken before delivery and never after.
                    // 3.1 asks that the way out be undeliverable to the overlay
                    // by construction, and this `break` is that construction:
                    // the loop that hands keys to the guest is the only path
                    // there is, and Back leaves it before reaching the call.
                    //
                    // A modal keeps it -- Escape on a folder picker means "I
                    // decline", and spending it on standing the overlay down
                    // would leave that screen up with its promise unsettled.
                    if(stroke.nav==KEY_BACK&&!home_modal()) {
                        overlay_yield("the person");
                        // The menu is back, which is what this marker has
                        // always meant. Saying it here keeps the contract the
                        // host scripts read -- they open with Back and wait for
                        // this line, and an armed overlay would otherwise make
                        // that wait forever.
                        ESP_LOGI("shell","HOME_READY");
                        have=false;
                        break;
                    }
                    overlay_key(&stroke);
                    have=xQueueReceive(keys,&stroke,0)==pdTRUE;
                }
            }
            while(have) {
                if(!s->key(&stroke)) { go_home(); break; }
                if(screen!=was) break;
                const char *ignored; size_t ignored_len;
                if(s->wants_run && s->wants_run(&ignored,&ignored_len)) break;
                have=xQueueReceive(keys,&stroke,0)==pdTRUE;
            }
            s=&SCREENS[screen];              // key() may have moved us
            // One overlay turn, before the frame it will be composited into
            // and only where the home screen still owns the display. It runs
            // ahead of the wants_run test below so that a key which starts a
            // foreground app in this same frame still finds the overlay
            // released by begin_run() rather than half-ticked.
            if(!running && screen==SCREEN_HOME) overlay_tick();
            const char *source=NULL; size_t len=0;
            if(!running && s->wants_run && s->wants_run(&source,&len)) {
                const char *pre=NULL; size_t pre_len=0;
                if(s->prelude) pre=s->prelude(&pre_len);
                begin_run(screen==SCREEN_TUTORIAL?"local.tutorial":"local.playground",
                          pre,pre_len,source,len);
            }
            else if(!running)
                paint(s);

            framed:
            {
            int test=atomic_exchange(&diagnostic,0);
            // Runs in place of starting a guest: it needs no app, and holding
            // 6,480 bytes of scratch is only affordable while none is running.
            if(test=='8') { sound_check_tables(); test=0; }
            // Returns immediately: the sweep runs on its own task, because
            // twelve seconds of it inline here is twelve seconds without a
            // frame reaching the panel. Started from here anyway, so it cannot
            // begin while a guest owns the display.
            if(test=='9') { sound_capture_probe(); test=0; }
            if(test && !running && screen==SCREEN_HOME) {
                overlay_release();
                owner=SCREEN_HOME;
                app_registry_select(APP_ID_DEFAULT);
                run_started=app_start_test(test);
                running = run_started==ESP_OK;
                if(!running) { app_stop(); home_error="TEST ERROR"; }
            }
            }
        }

        // Which reading of the USB byte stream applies. A running app is
        // normally not a text screen, and a text session makes it one for as
        // long as it is up -- otherwise a host script driving this over USB
        // could not type into the field it just opened.
        atomic_store(&text_screen,
                     SCREENS[screen].takes_text || pocket_text_active());

        int held=(int)((esp_timer_get_time()-frame_start)/1000);
        unsigned cap = running ? 33 : SCREENS[screen].frame_ms;
        vTaskDelay(pdMS_TO_TICKS((unsigned)held<cap?cap-held:1));
    }
}

// Every stored preference, the SKK settings and the Wi-Fi credentials live in
// NVS, so it comes up before the first thing that reads it. It used to be one
// term of an && chain inside shell_init(), where the failure was discarded and
// surfaced only as settings that had reset themselves.
static void nvs_init(void) {
    esp_err_t err=nvs_flash_init();
    // These two are the only failures an erase fixes: the partition is full of
    // stale pages, or it was written by a different NVS version. Erasing on
    // anything else would throw away the user's settings to work around a fault
    // we would rather see, so the rest is logged and left alone.
    if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("boot","NVS_ERASED %s",esp_err_to_name(err));
        err=nvs_flash_erase();
        if(err==ESP_OK) err=nvs_flash_init();
    }
    if(err!=ESP_OK) ESP_LOGE("boot","NVS_INIT_FAILED %s",esp_err_to_name(err));
}

void app_main(void) {
    ESP_LOGI("boot","Cardputer ADV PocketJS M1; app=3MiB skk=2MiB fonts=512KiB");
    ESP_ERROR_CHECK(board_init());
    nvs_init();
    pet_hub_init();
    shell_init();
    // After shell_init(), which is where the stored arming bit reaches the
    // overlay's setter, and before the UI task, which is what would start it.
    // This is the boot that decides whether an overlay that did not survive
    // its last start gets another one, and the answer is no.
    overlay_init();
    // Neither is fatal: the home stays usable with no dictionary and no font,
    // and the editors show which one is missing.
    jpfont_init();
    skk_session_init();
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=1024,.rx_buffer_size=256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    keys=xQueueCreate(16,sizeof(keystroke_t));configASSERT(keys);
    configASSERT(xTaskCreate(input_task,"input",4096,NULL,6,NULL)==pdPASS);
    configASSERT(xTaskCreate(ui_task,"ui",32768,NULL,5,NULL)==pdPASS);
}
