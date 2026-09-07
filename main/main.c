#include "board.h"
#include "shell.h"
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
#include "app_registry.h"
#include "pet_hub.h"
#include "pocket_bridge.h"
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
    if(c>='1'&&c<='6') { atomic_store(&diagnostic,c); return false; }
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
    bool (*key)(const keystroke_t *k);   // false: leave for the home screen
    bool (*dirty)(void);
    void (*draw)(void);
    // Set when the screen can run JavaScript. Returns true once it wants to.
    bool (*wants_run)(const char **source, size_t *len);
    // How the run ended, for the screen that asked for it.
    void (*ended)(esp_err_t started, const char *error);
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
static void begin_run(const char *app_id, const char *source, size_t len);

// The calibration program is embedded rather than kept in a source slot: it is
// the thing you reach for when the sensor is wrong, and a slot someone has
// edited is exactly what you cannot trust at that moment.
extern const char imucal_start[] asm("_binary_imucal_js_start");
extern const char imucal_end[]   asm("_binary_imucal_js_end");
extern const char pet_start[] asm("_binary_pet_js_start");
extern const char pet_end[] asm("_binary_pet_js_end");
extern const char companion_start[] asm("_binary_companion_js_start");
extern const char companion_end[] asm("_binary_companion_js_end");

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
        case 4: begin_run("local.imucal",imucal_start,(size_t)(imucal_end-imucal_start-1)); break;
        case 5: begin_run("local.pet",pet_start,(size_t)(pet_end-pet_start-1)); break;
        case 6: begin_run("local.companion",companion_start,(size_t)(companion_end-companion_start-1)); break;
        default: begin_run("local.hello",NULL,0);          // the built-in app
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
        .tag="code", .ready="CODE_READY", .open=code_open,
        .key=code_key, .dirty=code_dirty, .draw=code_draw,
        .wants_run=code_wants_run, .ended=code_ended,
        .frame_ms=16, .takes_text=true,
    },
    [SCREEN_TUTORIAL]={
        .tag="tutorial", .ready="TUTORIAL_READY", .open=tutorial_open,
        .key=tutorial_key, .dirty=tutorial_dirty, .draw=tutorial_draw,
        .wants_run=tutorial_wants_run, .ended=tutorial_ran,
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
    begin_run(APP_ID_WORK,source,length);
    // app_start_source() borrows the bytes only for the length of the start.
    pocket_workspace_run_done();
}

// Which app is starting, by name. Section 3 gives the host the identity, and
// app_session.c reads it back out of the registry to key the stores and to
// check what the manifest requires — so the name here is the whole of the
// decision, and the ternary on a source pointer that used to stand in for it
// is gone.
static void begin_run(const char *app_id, const char *source, size_t len) {
    owner=screen;
    app_registry_select(app_id);
    run_started = source ? app_start_source(source,len) : app_start();
    ESP_LOGI(SCREENS[owner].tag,"RUN %u bytes -> %s",
             (unsigned)len,esp_err_to_name(run_started));
    if(run_started==ESP_OK) { running=true; return; }
    // The guest never came up, so there is nothing to tick or stop; report it
    // to whoever asked and stay where we are.
    app_stop();
    running=false;
    const screen_ops_t *o=&SCREENS[owner];
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
        pet_repaint=pet_hub_pump();
        if(have&&pet_hub_key(stroke.nav)){have=false;pet_repaint=true;}
        if(pet_repaint&&running)app_force_redraw();
        const screen_ops_t *s=&SCREENS[screen];

        // The force stop, and Back on the home screen, arrive out of band.
        if(atomic_exchange(&stop,false)) {
            if(running) end_run(ESP_OK);
            else if(screen!=SCREEN_HOME) go_home();
            else if(home_error) home_error=NULL;
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
            screen_id_t was=screen;
            while(have) {
                if(!s->key(&stroke)) { go_home(); break; }
                if(screen!=was) break;
                const char *ignored; size_t ignored_len;
                if(s->wants_run && s->wants_run(&ignored,&ignored_len)) break;
                have=xQueueReceive(keys,&stroke,0)==pdTRUE;
            }
            s=&SCREENS[screen];              // key() may have moved us
            const char *source=NULL; size_t len=0;
            if(!running && s->wants_run && s->wants_run(&source,&len))
                begin_run(screen==SCREEN_TUTORIAL?"local.tutorial":"local.playground",
                          source,len);
            else if(!running)
                paint(s);

            int test=atomic_exchange(&diagnostic,0);
            if(test && !running && screen==SCREEN_HOME) {
                owner=SCREEN_HOME;
                app_registry_select(APP_ID_DEFAULT);
                run_started=app_start_test(test);
                running = run_started==ESP_OK;
                if(!running) { app_stop(); home_error="TEST ERROR"; }
            }
        }

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
