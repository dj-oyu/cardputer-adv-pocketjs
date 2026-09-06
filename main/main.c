#include "board.h"
#include "shell.h"
#include "motion.h"
#include "sound.h"
#include "keymap.h"
#include "editor.h"
#include "codeedit.h"
#include "tutorial.h"
#include "jpfont.h"
#include "skk_session.h"
#include "app_session.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
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

// USB drives the home screen with single letters, but an editor needs the
// bytes themselves so a host script can type at it. 0x1b closes either way.
static bool usb_stroke(char c, keystroke_t *k) {
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
static void begin_run(const char *source, size_t len);

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
    if(!shell_key(nav)) return true;
    switch(shell_app()) {
        case 1: enter(SCREEN_PRACTICE); break;
        case 2: enter(SCREEN_CODE); break;
        case 3: enter(SCREEN_TUTORIAL); break;
        default: begin_run(NULL,0);          // the built-in app
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
}

static void begin_run(const char *source, size_t len) {
    owner=screen;
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
    board_key_t key=have?stroke->nav:KEY_NONE;
    bool leave = have && key==KEY_BACK;
    if(leave) app_request_stop();

    bool shot=atomic_exchange(&capture,false);
    if(shot) { board_capture(true); app_force_redraw(); }
    esp_err_t e=ESP_OK;
    if(!leave) {
        if(key==KEY_ENTER) sound_play(1);
        e=app_tick(key==KEY_ENTER?0x4000:0);
        // A release frame, so consecutive queued presses stay distinct.
        if(e==ESP_OK && key==KEY_ENTER) e=app_tick(0);
    }
    if(shot) board_capture(false);

    if(leave || e!=ESP_OK) end_run(e);
}

// Repaint, and report what one costs. The frame cap would hide it otherwise.
static void paint(const screen_ops_t *s) {
    int64_t began=esp_timer_get_time();
    bool shot=atomic_exchange(&capture,false);
    if(!shot && !s->dirty()) return;
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
        const screen_ops_t *s=&SCREENS[screen];

        // The force stop, and Back on the home screen, arrive out of band.
        if(atomic_exchange(&stop,false)) {
            if(running) end_run(ESP_OK);
            else if(screen!=SCREEN_HOME) go_home();
            else if(home_error) home_error=NULL;
            else shell_key(KEY_BACK);
            xQueueReset(keys);
            have=false;
        }

        if(running) {
            tick_run(have,&stroke);
        } else {
            if(have && !s->key(&stroke)) go_home();
            s=&SCREENS[screen];              // key() may have moved us
            const char *source=NULL; size_t len=0;
            if(!running && s->wants_run && s->wants_run(&source,&len))
                begin_run(source,len);
            else if(!running)
                paint(s);

            int test=atomic_exchange(&diagnostic,0);
            if(test && !running && screen==SCREEN_HOME) {
                owner=SCREEN_HOME;
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

void app_main(void) {
    ESP_LOGI("boot","Cardputer ADV PocketJS M1; app=3MiB skk=2MiB fonts=512KiB");
    ESP_ERROR_CHECK(board_init());
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
