#include "shell.h"
#include "scene.h"
#include "wave.h"
#include "ocean.h"
#include "solar_sail.h"
#include "flower.h"
#ifdef KASANE_FLOWER_D1_PROBE
#include "flower_d1_species_probe.h"
#include "flower_d1_pressure_probe.h"
#endif
#include "menu_rows.h"
#include "overlay.h"
#include "net_autosync.h"
#include "pet_hub.h"
#include "pocket_kasane.h"
#include "app_session.h"
#ifdef KASANE_D2_OVERLAY_PROC_PROBE
#include "pocket_proc.h"
static bool d2_overlay_fault_injected;
static bool d2_overlay_retry;
static int d2_overlay_sends_before_failure=-1;
#endif
#ifdef KASANE_P5_NOTICE_PROBE
#include "system/sys_device.h"
#endif
#include "glass_rain.h"
#include "board.h"
#include "paint.h"
#include "motion.h"
#include "sound.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "fonts.h"
#include "ksn_font.h"
#include "kasane/ksn_p0_probe.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_log.h"
#if defined(KASANE_FLOWER_FRAME_PROBE) || defined(KASANE_FLOWER_AUDIO_PROBE) || \
    defined(KASANE_FLOWER_D1_PROBE) || \
    defined(KASANE_D6_SD_AV_STREAM_PROBE)
#include "esp_heap_caps.h"
#endif
#ifdef KASANE_P5_OVERLAY_REPAIR_PROBE
#include <stdatomic.h>
#ifdef KASANE_P5_SEND_PHASE_PROBE
#include "kasane/ksn_repair_send_probe.h"
#endif
static atomic_bool overlay_repair_requested;
static atomic_bool overlay_repair_capture_requested;
/* UI task only; the input task only sets the atomic request. */
static unsigned overlay_repair_stage;
static int overlay_repair_remaining=-1;
static bool overlay_repair_capturing;
#ifdef KASANE_P5_SEND_PHASE_PROBE
static ksn_repair_send_probe repair_send_probe;
static bool repair_send_probe_active;
#endif
void shell_overlay_repair_request(bool capture_pixels) {
    atomic_store(&overlay_repair_capture_requested,capture_pixels);
    atomic_store(&overlay_repair_requested,true);
}
#endif
#ifdef KASANE_P5_LOWHEAP_PROBE
#include <stdatomic.h>
#include "esp_heap_caps.h"
static atomic_bool lowheap_toggle_requested;
static uint8_t *lowheap_reservation;
void shell_lowheap_toggle_request(void) {
    atomic_store(&lowheap_toggle_requested,true);
}
static void shell_lowheap_service(void) {
    if(!atomic_exchange(&lowheap_toggle_requested,false))return;
    if(lowheap_reservation){
        heap_caps_free(lowheap_reservation);
        lowheap_reservation=NULL;
        ESP_LOGI("KSN_P5_LOWHEAP","OFF free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        return;
    }
    lowheap_reservation=heap_caps_malloc(16384,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!lowheap_reservation){ESP_LOGE("KSN_P5_LOWHEAP","ALLOC_FAIL bytes=16384");return;}
    memset(lowheap_reservation,0xa5,16384);
    ESP_LOGI("KSN_P5_LOWHEAP","ON bytes=16384 free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
}
#endif

static uint16_t *strip;
static int strip_y, strip_h;
static unsigned mode;
#ifndef KASANE_FLOWER_FRAME_PROBE
/* Keep the committed scene until the candidate has been fully presented. */
static flower_frame *flower_scene_frames[2];
static glass_rain_frame flower_scene_rain[2];
static unsigned flower_scene_committed;
static bool flower_scene_candidate,flower_scene_retry;
static bool flower_scene_has_committed,flower_scene_use_committed;
#ifdef KASANE_FLOWER_D1_PROBE
static bool flower_d1_species_running,flower_d1_pressure_tried;
static bool flower_d1_pressure_active,flower_d1_recovery_pending;
static uint32_t flower_d1_before_hash;
#endif
#ifdef KASANE_FLOWER_ALLOC_FAULT_PROBE
static unsigned flower_scene_fault_frames;
#endif
static void flower_scene_release(void) {
#ifdef KASANE_FLOWER_D1_PROBE
    flower_d1_pressure_end();
    flower_d1_pressure_active=false;
    flower_d1_recovery_pending=false;
#endif
    for(unsigned i=0;i<2;i++) {
        flower_frame_destroy(flower_scene_frames[i]);
        flower_scene_frames[i]=NULL;
    }
    flower_scene_candidate=flower_scene_retry=false;
    flower_scene_committed=0;
    flower_scene_has_committed=flower_scene_use_committed=false;
#ifdef KASANE_FLOWER_ALLOC_FAULT_PROBE
    flower_scene_fault_frames=0;
#endif
}
#endif
#ifdef KASANE_FLOWER_FRAME_PROBE
static flower_frame *flower_probe_frames[2];
static unsigned flower_probe_current;
static bool flower_probe_valid;
static bool flower_probe_arm;
static unsigned flower_probe_windows;
static uint64_t flower_probe_capture_us;
static unsigned flower_probe_captures;
static uint32_t flower_probe_min_free=UINT32_MAX;
static uint32_t flower_probe_min_largest=UINT32_MAX;
static void flower_probe_release(void) {
    for(unsigned i=0;i<2;i++) {
        flower_frame_destroy(flower_probe_frames[i]);
        flower_probe_frames[i]=NULL;
    }
    flower_probe_valid=false;
}
#endif
void shell_release_background_frames(void) {
#ifdef KASANE_FLOWER_FRAME_PROBE
    flower_probe_release();
#else
    size_t held=flower_frame_bytes(flower_scene_frames[0])+
                flower_frame_bytes(flower_scene_frames[1]);
    flower_scene_release();
    if(held)ESP_LOGI("background","FLOWER_FRAMES_RELEASE bytes=%u",
                     (unsigned)held);
#endif
}
// The backgrounds. See scene_ops_t in scene/scene.h for why this is a table.
// FLOWER MESH used to sit after FLOWER RAY and drew the same flower from a
// stored vertex mesh. The mesh cost 17,472 bytes of .bss for the whole life of
// the boot on a board whose heap is the binding constraint, and the analytic
// path it sat beside needs no geometry at all -- so the renderer is ray only
// and the row went with it. tools/home_modes.py reads the rows below.
static void solar_prepare(float dt,int tilt_x,int tilt_y,unsigned variant);
static void flower_scene_prepare(float dt,int tilt_x,int tilt_y,unsigned variant);
static uint32_t solar_scene_draw(uint16_t *strip,int y,int height);
static uint32_t flower_scene_draw(uint16_t *strip,int y,int height);
static void solar_labels(uint16_t *strip,int y,int height);

static const scene_ops_t SCENES[]={
    {"LEVEL WAVE",    wave_prepare,  wave_draw,  wave_overlay,  0},
    {"OCEAN + STARS", ocean_prepare, ocean_draw, ocean_overlay, 0},
    {"SOLAR SAIL",    solar_prepare, solar_scene_draw, solar_labels,   0},
    // One row for the plants. It used to be four -- FLOWER RAY, LILY OF VALLEY,
    // SUNFLOWER, SNOWDROP -- which asked somebody to choose between three
    // flowers they had not seen. The row now rotates the botanicals on
    // its own, each change hidden behind a dissolve; the interval and the fade
    // are named constants in flower.c.
    {"FLOWER", flower_scene_prepare, flower_scene_draw, NULL, 0},
};
#define BACKGROUND_N (sizeof(SCENES)/sizeof(SCENES[0]))
// mode is an index into SCENES and NVS can hand back anything, so every read
// goes through here rather than trusting the stored byte.
static const scene_ops_t *scene(void) {
    return &SCENES[mode<BACKGROUND_N?mode:0];
}
static const char *const toggles[]={"OFF","ON"};
// Five names rather than five numbers: a percentage would imply a scale the
// codec does not have (its steps are decibels), and "60%" would be a number
// nobody measured.
static const char *const volumes[]={"QUIET","LOW","MID","HIGH","LOUD"};
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t present_sum, prep_sum, loop_sum, hud_sum;
// The vector rows alone, inside loop_sum. Worth its two cycle reads a row: for
// a long time "loop" was read as if it were the kernel, and it is not — the
// sky above the horizon and the scaffolding are a quarter of it.
static uint64_t kernel_cycles;
// TEMPORARY, and it splits `hud` the way kernel= splits `loop`. hud has been
// 6.2 ms for as long as anyone has measured it -- larger than shade, 13% of the
// frame -- and nothing has ever looked inside it, because every counter added
// this session went into the scene. It is four different things running once
// per strip, seventeen times a frame:
//
//   fmt    the FPS string, formatted whether or not it is shown
//   ovl    the scene's overlay (NULL for FLOWER, so zero there)
//   fps    drawing that string
//   menu   the whole menu: the layout once a frame, the painting per strip
//
// `ovl` is the self-check: FLOWER's overlay is NULL, so it must read 0.00 there
// or the split is wrong. A counter that can be held against a known zero is
// worth more than one that cannot.
//
// It was spent for a while. FLOWER carried an overlay -- a trace of the swarm's
// births and deaths -- and the pair of counters that replaced the known zero
// (this one and the trace's own) had to agree instead. The trace measured 3.3
// to 4.0 ms and came out again, so the zero is back, and the episode is in
// docs/perf/pie-simd.md 4.3 rather than in a comment here.
//
// Cycle counts, not esp_timer_get_time(): the timer is 0.90 us a call
// (docs/perf/pie-simd.md 6.5), and eight of those per strip would be 0.12 ms of
// measurement on a 6.2 ms subject, concentrated on whichever piece is smallest.
// `rsr.ccount` is one instruction.
static uint64_t hud_fmt_cy,hud_ovl_cy,hud_fps_cy,hud_menu_cy;
#define HUD_FENCE __asm__ __volatile__("":::"memory")
static uint64_t draw_sum;
static float fps;
#ifdef KASANE_FLOWER_AUDIO_PROBE
static uint32_t flower_audio_min_free=UINT32_MAX;
static uint32_t flower_audio_min_largest=UINT32_MAX;
static unsigned flower_audio_windows;
static bool flower_audio_active;
#endif
static unsigned category,setting,app;
// APPEND to this table; do not insert. tools/capture_home.py and
// tools/test_settings.py both leave this list for the settings category before
// they count anything, so a row on the end changes no navigation either of them
// does -- capture_home.py line 80 says as much about POCKET PET. A row in the
// MIDDLE would renumber shell_app(), and with it main.c's switch, silently.
static const char *apps[]={"HELLO WORLD","SKK PRACTICE","PLAYGROUND","TUTORIAL",
                          "IMU CALIBRATION","POCKET PET","PET COMPANION","STRESS TEST",
                          "MEGADEMO","GRID LAB","VIDEO LAB",
                          "LCD CATCH","DERBY WATCH","BIG WAVE"};
static const char *app_details[]={"JAVASCRIPT / POCKETJS","JAPANESE INPUT DRILL",
                                  "WRITE AND RUN JAVASCRIPT","LEARN TO WRITE IT",
                                  "FIND THE SENSOR AXES","CHOOSE AND CARE FOR YOUR PET",
                                  "AI USAGE / ALARM / TIMER","HEAP CHURN + DRAWING LOAD",
                                  "PROCEDURAL 3D / GLITCH","TYPED GRID / AUTO PIE",
                                  "RGB565 STREAM / UI",
                                  "LCD SEGMENT ARCADE","WATCH THE RACE","3D SURF / EASD"};
// AUDIO STREAM / OPUS STREAM / OPUS + WI-FI / MP3 PLAYBACK used to be appended
// here (apps/streamplay, apps/opusplay, apps/opusfit, apps/mp3play) -- dev/test
// apps for the MP3 and Opus decoders, removed once those decoders were verified
// (docs/api/common-api.md 9.1-9.1.3). apps/player, the shipped feature that uses the
// same decoders, is a MUSIC overlay reached from the home screen, not a row here.
#define APP_N (sizeof(apps)/sizeof(apps[0]))
// The manifest id each row launches (main.c's shell_app() switch), NULL for the
// rows that open a host screen. Only for the paused mark: a kept app's row
// shows it (docs/vm/app-suspend-design.md sec.8-4). Same order as apps[].
static const char *const app_ids[]={"local.hello",NULL,NULL,NULL,"local.imucal",
                                    "local.pet","local.companion","local.stress",
                                    "local.megademo","local.gridlab","local.videolab",
                                    "local.lcdcatch","local.derby","local.bigwave"};
_Static_assert(sizeof app_ids/sizeof app_ids[0]==APP_N,"app_ids follows apps[]");
static float app_pos;
unsigned shell_app(void) { return app; }
static bool choices;
static unsigned choice;
static float category_pos, item_pos, choice_pos, depth_pos;
static int64_t animation_time;
static const char *categories[]={"APPS","SETTINGS"};
static bool show_fps,sfx=true;
static nvs_handle_t prefs;
static bool prefs_ready;
static shell_screen_t pending_screen;

// The settings list used to live in eight places at once — a label array, a
// navigation bound, two "how many values does this one have" ternaries, the
// seed, the apply chain, the save chain and the detail line — and they only
// agreed by hand. Growing the backgrounds from two to three already broke a
// capture script that had counted key presses against the old length. It is
// one table now: a row is an entry, and nothing outside the table counts.
typedef enum {
    SETTING_CHOICES,   // a fixed set of value names, chosen on the depth screen
    SETTING_ACTION,    // selecting it leaves the home screen for another one
} setting_kind_t;
typedef struct {
    const char *label;           // the row, as the Settings list draws it
    setting_kind_t kind;
    // SETTING_CHOICES: the value names, and the step from one to the next.
    // BACKGROUND's live inside scene_ops_t rows rather than in an array of
    // their own, so the stride is the row -- the same trick menu_list already
    // uses to walk this table's own labels, and for the same reason: a
    // parallel array of labels is a second list that can disagree with the
    // first one.
    const char *const *values;
    size_t stride;
    unsigned count;              // SETTING_CHOICES: how many of them
    const char *key;             // NVS key, NULL for anything not persisted
    unsigned (*get)(void);       // the live value, as an index into values
    void (*set)(unsigned);       // make the live value that index
    shell_screen_t screen;       // SETTING_ACTION: where Enter goes
} setting_t;

// Loading a preference and applying one are the same act — make the setting
// equal to this index — so the table carries one setter for both. Background
// goes through shell_change_background() because changing the mode also has to
// throw away the frame statistics gathered under the old one.
static unsigned background_get(void) {return mode;}
static void background_set(unsigned v) {
    // A device that stored SNOWDROP under the old seven-row menu comes back to
    // a four-row one. Out of range means the first row, not whatever index the
    // modulo in shell_change_background() would have landed on -- that would
    // have silently moved somebody from a flower to the solar system.
    if(v>=BACKGROUND_N)v=0;
    if(v!=mode)shell_change_background((int)v-(int)mode);
}
static unsigned fps_get(void) {return show_fps;}
static void fps_set(unsigned v) {show_fps=v!=0;}
static unsigned sound_get(void) {return sfx;}
static void sound_set(unsigned v) {sfx=v!=0;sound_set_enabled(sfx);}

static unsigned volume_get(void) { return sound_volume(); }
static void volume_set(unsigned v) { sound_set_volume(v); }
static unsigned autotime_get(void) { return net_autosync_enabled(); }
static void autotime_set(unsigned v) { net_autosync_set_enabled(v!=0); }

static const setting_t settings[]={
    {"BACKGROUND", SETTING_CHOICES, &SCENES[0].name, sizeof SCENES[0], BACKGROUND_N,
     "background",  background_get,  background_set, SHELL_SCREEN_NONE},
    {"FPS DISPLAY",SETTING_CHOICES, toggles, sizeof toggles[0], 2,
     "fps",         fps_get,         fps_set,        SHELL_SCREEN_NONE},
    {"SOUND",      SETTING_CHOICES, toggles, sizeof toggles[0], 2,
     "sound",       sound_get,       sound_set,      SHELL_SCREEN_NONE},
    // The first action row. It has no value and no NVS key of its own: what it
    // changes lives in the "wifi" namespace, written by the screen it opens.
    // Named for the thing it configures, like every row above it — syncing the
    // clock is one action on that screen, not the whole of what it is for.
    {"WI-FI",      SETTING_ACTION,  NULL,    0,                 0,
     NULL,          NULL,            NULL,           SHELL_SCREEN_WIFI},
    // The overlay of docs/api/common-api.md 3.1, and the only way to it. 3.1 asks
    // that revoking permission be reachable from the home screen even while
    // the overlay is broken, so it is a row here and not a screen of its own:
    // this list draws with nothing of the overlay's on the path. The ON label
    // is a live buffer -- ui/overlay.c writes the frame cost, the refusal or
    // the stop into it -- which is the "somewhere the person can see it and
    // decide" the same section asks for. APPENDED, not inserted: the settings
    // rows are navigated by counted key presses in tools/test_settings.py and
    // tools/capture_home.py.
    // 3.1: the choice is WHICH overlay replaces the menu, not whether one is
    // drawn on top of it. This is the row a person turns it off from, so it is
    // also where each overlay's live status appears.
    {"HOME OVERLAY", SETTING_CHOICES, overlay_choice_names, sizeof overlay_choice_names[0],
     1+OVERLAY_APPS,
     "overlay",     overlay_armed_get, overlay_armed_set, SHELL_SCREEN_NONE},
    // APPENDED, like every row before it: the settings are navigated by counted
    // key presses in tools/test_settings.py and tools/capture_home.py, and
    // inserting would move every index below it silently.
    {"VOLUME",     SETTING_CHOICES, volumes, sizeof volumes[0], SOUND_VOLUME_STEPS,
     "volume",      volume_get,      volume_set,     SHELL_SCREEN_NONE},
    // The switch for net_autosync.c: whether the home screen, left idle, may
    // bring the radio up for a few seconds to set the clock. A value row, not
    // a line on the Wi-Fi screen, because it is a standing preference and this
    // table is where those persist. On by default; with no network stored it
    // does nothing either way. APPENDED, for the same counted key presses.
    {"AUTO TIME SYNC", SETTING_CHOICES, toggles, sizeof toggles[0], 2,
     "autotime",    autotime_get,    autotime_set,   SHELL_SCREEN_NONE},
};
#define SETTING_N (sizeof(settings)/sizeof(settings[0]))
// One value name out of a row's list, wherever that list keeps them.
static const char *value_name(const setting_t *e,unsigned i) {
    return *(const char *const *)((const char *)e->values+(size_t)i*e->stride);
}

// "background=0 fps=1 sound=1" — the line test_settings.py parses. Built from
// the NVS keys so a new persisted row appears in it without being named here;
// an action row has no key and no value, and stays out.
static void settings_summary(char *out,size_t size) {
    size_t used=0;
    for(unsigned i=0;i<SETTING_N;i++) {
        if(!settings[i].key)continue;
        int n=snprintf(out+used,size-used,used?" %s=%u":"%s=%u",
                       settings[i].key,settings[i].get());
        if(n<0||(size_t)n>=size-used)break;
        used+=(size_t)n;
    }
}

void shell_init(void) {
    char summary[128]={0};
    // NVS is brought up by app_main() now. It used to be the first term of this
    // condition, where a failure was indistinguishable from "no key stored" and
    // reached the person only as settings that reset themselves.
    if(nvs_open("home",NVS_READWRITE,&prefs)==ESP_OK) {
        prefs_ready=true;uint8_t v;
        for(unsigned i=0;i<SETTING_N;i++)
            if(settings[i].key&&nvs_get_u8(prefs,settings[i].key,&v)==ESP_OK)
                settings[i].set(v);
    }
    // Still unconditional: with no stored key the default has to reach the
    // mixer too, and the setter above never ran.
    sound_set_enabled(sfx);
    settings_summary(summary,sizeof summary);
    ESP_LOGI("settings","LOADED %s",summary);
}

shell_screen_t shell_pending_screen(void) {
    shell_screen_t requested=pending_screen;
    pending_screen=SHELL_SCREEN_NONE;
    return requested;
}
void shell_settings_save(void) {
    if(!prefs_ready) return;
    esp_err_t err=ESP_OK;
    for(unsigned i=0;i<SETTING_N&&err==ESP_OK;i++)
        if(settings[i].key)
            err=nvs_set_u8(prefs,settings[i].key,(uint8_t)settings[i].get());
    if(!err)err=nvs_commit(prefs);
    if(err)ESP_LOGW("settings","Save failed: %s",esp_err_to_name(err));
}

// The volume was changed by its keys rather than by its row, so the row is not
// on screen to show what happened. This is the shell saying so, and it is the
// shell's to say: an overlay that drew the volume would be drawing a number it
// cannot change and might not have.
//
// It goes ON TOP of the overlay, which is allowed and is not a hole in 3.1: the
// rule is that an overlay may not cover the shell's CONSENT surfaces, and this
// is the shell covering an overlay, which is the direction that was never in
// question.
static int64_t volume_shown_us;
void shell_volume_touched(void) { volume_shown_us=esp_timer_get_time(); }

static void paint_volume(void) {
    if(!volume_shown_us) return;
    if(esp_timer_get_time()-volume_shown_us>1500000) { volume_shown_us=0; return; }
    // paint.c keeps the live strip in module state and overlay_paint() may have
    // pointed it somewhere else this pass, so it is set again here rather than
    // assumed.
    paint_begin(strip,strip_y,strip_h);
    const uint16_t ink=board_rgb(237,246,255), dim=board_rgb(40,60,84),
                   back=board_rgb(8,13,22);
    int w=SOUND_VOLUME_STEPS*10+8, x=LCD_W-w-6, y=6;
    paint_fill(x,y,w,14,back);
    for(unsigned i=0;i<SOUND_VOLUME_STEPS;i++)
        paint_fill(x+4+(int)i*10,y+4,8,6,i<=sound_volume()?ink:dim);
}

// Whether a setting's value list is open: Back then closes the list rather
// than acting at the home screen's root (main.c: Back at the root stops the
// background music, S5).
bool shell_choices_open(void) { return choices; }

bool shell_key(board_key_t key) {
    if(key==KEY_BACK) {
        choices=false;sound_play(2);
        ESP_LOGI("shell","HOME_READY");
    } else if(choices&&(key==KEY_UP||key==KEY_DOWN)) {
        unsigned next=choice,count=settings[setting].count;
        if(key==KEY_DOWN&&next+1<count)next++;
        if(key==KEY_UP&&next>0)next--;
        if(next!=choice){choice=next;sound_play(0);}
        ESP_LOGI("settings","CHOICE %u",choice);
    } else if(choices&&(key==KEY_LEFT||key==KEY_RIGHT)) {
        // Horizontal input changes categories only at the root.
    } else if(key==KEY_LEFT||key==KEY_RIGHT) {
        unsigned next=key==KEY_RIGHT?1:0;
        if(next!=category){category=next;sound_play(0);}
        ESP_LOGI("shell","CATEGORY %u",category);
    } else if(category==0&&(key==KEY_UP||key==KEY_DOWN)) {
        unsigned next=app;
        if(key==KEY_DOWN&&next+1<(unsigned)APP_N)next++;
        if(key==KEY_UP&&next>0)next--;
        if(next!=app){app=next;sound_play(0);}
        ESP_LOGI("shell","APP %u",app);
    } else if(category==1&&(key==KEY_UP||key==KEY_DOWN)) {
        unsigned next=setting;
        if(key==KEY_DOWN&&next+1<(unsigned)SETTING_N)next++;
        if(key==KEY_UP&&next>0)next--;
        if(next!=setting){setting=next;sound_play(0);}
        ESP_LOGI("settings","SELECT %u",setting);
    } else if(key==KEY_ENTER) {
        if(category==0){sound_play(1);return true;}
        const setting_t *entry=&settings[setting];
        if(!choices) {
            if(entry->kind==SETTING_ACTION) {
                pending_screen=entry->screen;
                sound_play(1);
                ESP_LOGI("settings","SCREEN %u screen=%d",setting,(int)entry->screen);
                return false;
            }
            choices=true;choice=entry->get();
            choice_pos=choice;sound_play(1);
            ESP_LOGI("settings","OPEN %u choice=%u",setting,choice);
            return false;
        }
        entry->set(choice);
        choices=false;
        sound_play(1);
        shell_settings_save();
        char summary[128]={0};
        settings_summary(summary,sizeof summary);
        ESP_LOGI("settings","VALUE %s",summary);
    }
    return false;
}
void shell_change_background(int direction) {
    mode=(unsigned)(((int)mode+(int)BACKGROUND_N+direction)%(int)BACKGROUND_N);
#ifdef KASANE_FLOWER_FRAME_PROBE
    if(mode!=3)flower_probe_release();
    flower_probe_arm=false;flower_probe_windows=0;
#else
    if(mode!=3)flower_scene_release();
#endif
    window_start=0;samples=0;max_us=0;draw_sum=0;fps=0;
#ifdef KASANE_FLOWER_AUDIO_PROBE
    flower_audio_min_free=flower_audio_min_largest=UINT32_MAX;
    flower_audio_windows=0;flower_audio_active=false;
#endif
    present_sum=prep_sum=loop_sum=hud_sum=0;
    hud_fmt_cy=hud_ovl_cy=hud_fps_cy=hud_menu_cy=0;
    ESP_LOGI("background","MODE %u %s",mode,scene()->name);
}

// Every menu label is drawn twice (shadow, then face) for each of seventeen
// strips, so this runs some four hundred times a frame. It used to reach the
// buffer through a clipped per-dot writer (the shape scene/stars.c still
// uses), which re-tested four bounds for each lit dot; now the
// row is resolved once per row, blank glyph rows are skipped whole, and a run
// that has left the screen ends the string.
// The paused mark (docs/vm/app-suspend-design.md sec.8-4): two bars, drawn for
// 0x7F because the font has no '|' (it came out as "??"). Symmetric, so the
// bit order of a row does not matter.
static const uint8_t PAUSE_GLYPH[7]={0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b};
static void text(int x,int y,const char *s,int scale,uint16_t color) {
    if(y>=strip_y+strip_h || y+7*scale<=strip_y) return;
    for(;*s;s++,x+=6*scale) {
        if(x>=LCD_W) return;
        if(x+5*scale<=0) continue;
        unsigned c=(unsigned char)*s;
        if(c!=0x7f && (c<32 || c>126)) c='?';
        const uint8_t *glyph=c==0x7f?PAUSE_GLYPH:font_rows+(c-32)*7;
        for(int gy=0;gy<7;gy++) {
            unsigned bits=glyph[gy];
            if(!bits) continue;
            for(int sy=0;sy<scale;sy++) {
                int py=y+gy*scale+sy-strip_y;
                if(py<0||py>=strip_h) continue;
                uint16_t *row=strip+(size_t)py*LCD_W;
                for(int gx=0;gx<5;gx++) {
                    if(!(bits&(1u<<(4-gx)))) continue;
                    for(int sx=0;sx<scale;sx++) {
                        int px=x+gx*scale+sx;
                        if(px>=0&&px<LCD_W) row[px]=color;
                    }
                }
            }
        }
    }
}
static float approach(float value,float target,float amount) {
    float result=value+(target-value)*amount;
    return fabsf(result-target)<0.005f?target:result;
}
// The menu's layout is a pure function of the four animated positions and does
// not depend on which strip is being drawn -- only the *drawing* was clipped,
// by text()'s strip check at the top. So all of it ran seventeen times a frame
// to produce the same answer: eleven rows of fades, item_y, lroundf, fminf and
// a settings getter, of which sixteen repetitions were discarded. It is
// resolved once into this list and painted per strip.
//
// The list holds resolved colours rather than emphases, which moves the three
// float multiplies and the board_rgb out of the per-strip path with everything
// else. Shadow then text, in list order, is the same order and the same pixels
// as the old label() produced.
// The list lives on shell_draw's stack, not in .bss: the ui task has 32 KB and
// DIRAM on this board does not (CLAUDE.md). A file-static pointer is what lets
// label() reach it from two calls down without threading a parameter through
// menu_layout and menu_list -- the same shape flower.c uses for `petals`.
typedef struct { int16_t x,y; uint8_t scale; uint16_t color; const char *s; } hud_label_t;
// Sized from the menu itself, so adding an app or a settings row grows it: two
// category labels, both lists with their detail line, and the choices overlay
// with its own label. The +8 is the largest `count` any settings row may offer
// before this has to be revisited, and the clamp in label() is the backstop if
// it ever is -- a dropped label is a missing menu row, so it is worth both.
#define HUD_LABEL_MAX (2 + (APP_N+1) + (SETTING_N+1) + (1+8))
static hud_label_t *hud_labels;
static unsigned hud_label_n;

static void label(int x,int y,const char *s,int scale,float emphasis) {
    if(emphasis<=0||!s)return;
    if(!hud_labels||hud_label_n>=HUD_LABEL_MAX)return;
    if(emphasis>1)emphasis=1;
    hud_labels[hud_label_n++]=(hud_label_t){(int16_t)x,(int16_t)y,(uint8_t)scale,
        board_rgb(65+172*emphasis,100+146*emphasis,125+130*emphasis),s};
}
static void paint_labels(void) {
    const uint16_t shadow=board_rgb(2,7,15);
    for(unsigned i=0;i<hud_label_n;i++) {
        const hud_label_t *l=&hud_labels[i];
        text(l->x+1,l->y+1,l->s,l->scale,shadow);
        text(l->x,l->y,l->s,l->scale,l->color);
    }
}
// The rule lives in menu_rows.h so that something other than a screenshot can
// read it; see that header for what it implies about where a decoration may go.
static float item_y(float delta) { return menu_item_y(delta); }
// `stride` is the step from one label to the next: the settings table keeps its
// label inside a wider struct, and walking it in place beats a parallel array
// of labels that could drift out of step with the table.
static void menu_list(int x,float position,const char *const *items,size_t stride,
                      unsigned count,float opacity,const char *detail) {
    for(unsigned i=0;i<count;i++) {
        float distance=fabsf(i-position);
        float strength=1-fminf(distance,1)*0.70f;
        float y=item_y(i-position);
        // Fade while crossing the category text, so two lines never collide.
        float clearance=fminf(fabsf(y-34)/20,1);
        const char *item=*(const char *const *)((const char *)items+(size_t)i*stride);
        label(x,(int)lroundf(y),item,MENU_ITEM_SCALE,opacity*strength*clearance);
    }
    float settled=1-fminf(fabsf(position-roundf(position))*4,1);
    if(detail)label(x,MENU_DETAIL_Y,detail,1,opacity*settled*0.75f);
}
static void menu_layout(void) {
    hud_label_n=0;
    for(unsigned c=0;c<2;c++) {
        float offset=(c-category_pos)*96;
        float visibility=1-fminf(fabsf(c-category_pos),1);
        int x=(int)lroundf(16+offset-depth_pos*160);
        label(x,MENU_CATEGORY_Y,categories[c],1,(0.35f+0.65f*visibility)*(1-depth_pos*0.6f));
        if(visibility>0.01f) {
            if(c==0) {
                menu_list(x,app_pos,apps,sizeof apps[0],APP_N,visibility,app_details[app]);
                // The paused mark: in the 16 px left of the row, as tall as its
                // text (10 x 14 px at the row's scale), and faded with it -- so it moves with its row and
                // adds no row of its own (test_settings.py and capture_home.py
                // count rows).
                const char *kept=app_dormant_id();
                for(unsigned i=0;kept[0]&&i<APP_N;i++) {
                    if(!app_ids[i]||strcmp(app_ids[i],kept)) continue;
                    float strength=1-fminf(fabsf(i-app_pos),1)*0.70f;
                    float y=item_y(i-app_pos);
                    float clearance=fminf(fabsf(y-34)/20,1);
                    label(x-13,(int)lroundf(y),"\x7f",MENU_ITEM_SCALE,visibility*strength*clearance);
                }
            } else {
                // An action row has no value to show under the list.
                const setting_t *entry=&settings[setting];
                const char *detail=entry->values?value_name(entry,entry->get()):NULL;
                menu_list(x,item_pos,&settings[0].label,sizeof settings[0],SETTING_N,
                          visibility*(1-depth_pos),detail);
            }
        }
    }
    if(depth_pos>0.005f) {
        int x=(int)lroundf(16+(1-depth_pos)*LCD_W);
        const setting_t *entry=&settings[setting];
        label(x,37,entry->label,1,depth_pos);
        menu_list(x,choice_pos,entry->values,entry->stride,entry->count,
                  depth_pos,NULL);
    }
}
// ---------------------------------------------------------------------------
// The scenes, as the table above names them.
//
// These are adapters, not renderers: each one is the branch shell_draw used to
// take, moved behind the row that selects it. wave and ocean still keep their
// state and their vector rows in this file; solar_sail, flower and glass_rain
// are their own translation units already.
//
// Every scene now keeps its own clock, accumulated from dt and clamped the way
// solar_sail and flower already clamped theirs -- so the table has one
// convention and a new row has one obvious shape. The absolute clock that used
// to be computed here and read by wave and ocean is gone with them.
// ---------------------------------------------------------------------------
static int64_t frame_started;
static glass_rain_frame flower_rain_frame;

static void solar_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
    solar_sail_prepare(dt,tilt_x,tilt_y);
}
static void flower_scene_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
#ifndef KASANE_FLOWER_FRAME_PROBE
    if(flower_scene_retry)return;
#endif
#ifdef KASANE_FLOWER_FRAME_PROBE
    /* Stabilize the visual workload after the first two-second warm-up.
     * The A/B still alternates in one binary while the scene clock is held. */
    if(flower_probe_windows>=1)dt=0;
#endif
#ifdef KASANE_FLOWER_D1_PROBE
    flower_d1_species_running=flower_d1_species_prepare();
    if(!flower_d1_species_running)flower_prepare_rotating(dt,tilt_x,tilt_y);
#else
    flower_prepare_rotating(dt,tilt_x,tilt_y);
#endif
#ifdef KASANE_FLOWER_FRAME_PROBE
    flower_probe_valid=false;
    if(flower_probe_arm) {
        unsigned next=flower_probe_current^1u;
        if(!flower_probe_frames[next])flower_probe_frames[next]=flower_frame_create();
        int64_t capture_start=esp_timer_get_time();
        flower_probe_valid=flower_probe_frames[next] &&
            flower_frame_capture(flower_probe_frames[next]);
        flower_probe_capture_us+=(uint64_t)(esp_timer_get_time()-capture_start);
        flower_probe_captures++;
        flower_probe_current=next;
    }
    uint32_t free_bytes=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    uint32_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(free_bytes<flower_probe_min_free)flower_probe_min_free=free_bytes;
    if(largest<flower_probe_min_largest)flower_probe_min_largest=largest;
#endif
    glass_rain_prepare(dt,(uint32_t)frame_started);
    glass_rain_capture(&flower_rain_frame);
#ifndef KASANE_FLOWER_FRAME_PROBE
    unsigned next=flower_scene_committed^1u;
#ifdef KASANE_FLOWER_D1_PROBE
    if(!flower_d1_species_running&&!flower_d1_pressure_tried&&
       flower_scene_has_committed&&flower_scene_frames[flower_scene_committed]) {
        flower_d1_pressure_tried=true;
        flower_frame_destroy(flower_scene_frames[next]);
        flower_scene_frames[next]=NULL;
        flower_d1_before_hash=flower_d1_committed_hash(
            flower_scene_frames[flower_scene_committed],
            &flower_scene_rain[flower_scene_committed]);
        size_t required=flower_d1_candidate_allocation_bytes();
        flower_d1_pressure_active=flower_d1_pressure_begin(required);
    }
#endif
#ifdef KASANE_FLOWER_ALLOC_FAULT_PROBE
    bool forced_failure=flower_scene_has_committed&&
        (++flower_scene_fault_frames%120u)==0u;
    if(forced_failure){
        flower_frame_destroy(flower_scene_frames[next]);
        flower_scene_frames[next]=NULL;
    }
#else
    bool forced_failure=false;
#endif
    if(!forced_failure&&!flower_scene_frames[next])
        flower_scene_frames[next]=flower_frame_create();
    flower_scene_candidate=flower_scene_frames[next]&&
        flower_frame_capture(flower_scene_frames[next]);
    if(flower_scene_candidate)flower_scene_rain[next]=flower_rain_frame;
    /* An allocation failure pauses on the last complete frame. The live
     * prepare state is never used to repair a partially transferred frame. */
    flower_scene_use_committed=!flower_scene_candidate&&flower_scene_has_committed;
#ifdef KASANE_FLOWER_D1_PROBE
    if(flower_d1_pressure_active) {
        ESP_LOGI("FLOWER_D1_ALLOC","ATTEMPT candidate=%u committed=%u fallback=%u free=%lu largest=%lu",
                 (unsigned)flower_scene_candidate,(unsigned)flower_scene_has_committed,
                 (unsigned)flower_scene_use_committed,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    }
#endif
#ifdef KASANE_FLOWER_ALLOC_FAULT_PROBE
    if(forced_failure)ESP_LOGI("FLOWER_ALLOC_FAULT",
        "injected=1 candidate=%u committed=%u fallback=%u",
        (unsigned)flower_scene_candidate,(unsigned)flower_scene_has_committed,
        (unsigned)flower_scene_use_committed);
#endif
#endif
}
static uint32_t solar_scene_draw(uint16_t *s,int y,int height) {
    // No inline assembly in this one: its cost is ordinary C and belongs in
    // loop=, not in kernel=.
    solar_sail_draw(s,y,height);
    return 0;
}
static uint32_t flower_scene_draw(uint16_t *s,int y,int height) {
    uint32_t c0=esp_cpu_get_cycle_count();
#ifdef KASANE_FLOWER_FRAME_PROBE
    if(flower_probe_valid)
        flower_frame_draw(flower_probe_frames[flower_probe_current],s,y,height);
    else
#else
    if(flower_scene_candidate)
        flower_frame_draw(flower_scene_frames[flower_scene_committed^1u],s,y,height);
    else if(flower_scene_use_committed)
        flower_frame_draw(flower_scene_frames[flower_scene_committed],s,y,height);
    else
#endif
    flower_draw(s,y,height);
#ifndef KASANE_FLOWER_FRAME_PROBE
    if(flower_scene_candidate)
        glass_rain_draw_frame(&flower_scene_rain[flower_scene_committed^1u],s,y,height);
    else if(flower_scene_use_committed)
        glass_rain_draw_frame(&flower_scene_rain[flower_scene_committed],s,y,height);
    else
#endif
    glass_rain_draw_frame(&flower_rain_frame,s,y,height);
    return esp_cpu_get_cycle_count()-c0;
}
static void solar_labels(uint16_t *s,int y,int height) {
    (void)s;(void)y;(void)height;
    text(12,121,solar_sail_time_label(),1,board_rgb(61,88,105));
    text(166,121,solar_sail_target(),1,board_rgb(87,125,144));
}

/* Kasane overlay display port. Loading and sending are deliberately split:
 * load_backdrop replays the native scene below the retained command bank;
 * present adds shell-owned indicators above it and transfers the result. */
typedef struct {
    const scene_ops_t *scene;
    const char *meter;
    uint16_t muted;
    unsigned *loop_us,*hud_us,*present_us;
} shell_overlay_port;

static uint16_t *shell_overlay_strip(void *ctx) {
    (void)ctx;return board_strip();
}
static ksn_result shell_overlay_backdrop(void *opaque,uint16_t y,uint16_t rows,
                                         uint16_t *pixels) {
    shell_overlay_port *ctx=opaque;
    strip=pixels;strip_y=y;strip_h=rows;
    int64_t began=esp_timer_get_time();
    kernel_cycles+=ctx->scene->draw(strip,strip_y,strip_h);
    *ctx->loop_us+=(unsigned)(esp_timer_get_time()-began);
    began=esp_timer_get_time();
    HUD_FENCE;uint32_t h0=esp_cpu_get_cycle_count();HUD_FENCE;
    if(ctx->scene->overlay)ctx->scene->overlay(strip,strip_y,strip_h);
    HUD_FENCE;hud_ovl_cy+=esp_cpu_get_cycle_count()-h0;HUD_FENCE;
    *ctx->hud_us+=(unsigned)(esp_timer_get_time()-began);
    return KSN_OK;
}
static ksn_result shell_overlay_send(void *opaque,uint16_t y,uint16_t rows,
                                     const uint16_t *pixels) {
#ifdef KASANE_D2_OVERLAY_PROC_PROBE
    if(!d2_overlay_fault_injected&&pocket_proc_pending()) {
        if(d2_overlay_sends_before_failure<0) {
            d2_overlay_sends_before_failure=3;
            ESP_LOGI("D2_OVERLAY","ARMED pending=1 after=3");
        }
        if(d2_overlay_sends_before_failure--==0) {
            d2_overlay_fault_injected=true;
            d2_overlay_retry=true;
            ESP_LOGW("D2_OVERLAY","FAIL pending=1 y=%u rows=%u",
                     (unsigned)y,(unsigned)rows);
            return KSN_IO;
        }
    }
#endif
#ifdef KASANE_P5_OVERLAY_REPAIR_PROBE
    if(overlay_repair_stage==1&&overlay_repair_remaining--==0) {
        overlay_repair_stage=2;
        ESP_LOGW("KSN_P5_REPAIR","INJECT_FAIL y=%u after=3",(unsigned)y);
        return KSN_IO;
    }
#endif
    shell_overlay_port *ctx=opaque;
    strip=(uint16_t *)pixels;strip_y=y;strip_h=rows;
    int64_t began=esp_timer_get_time();
    HUD_FENCE;uint32_t h0=esp_cpu_get_cycle_count();HUD_FENCE;
    if(show_fps)text(194,8,ctx->meter,1,ctx->muted);
    HUD_FENCE;uint32_t h1=esp_cpu_get_cycle_count();HUD_FENCE;
    paint_volume();
    HUD_FENCE;uint32_t h2=esp_cpu_get_cycle_count();HUD_FENCE;
    hud_fps_cy+=h1-h0;hud_menu_cy+=h2-h1;
    *ctx->hud_us+=(unsigned)(esp_timer_get_time()-began);
    began=esp_timer_get_time();
    /* The SYSTEM bank already owns the notice. Match the foreground Kasane
     * display port: board_present's legacy pet hook must not paint it again. */
    bool notice_composited=pocket_kasane_notice_composited();
    pet_hub_overlay_suppress(notice_composited);
    esp_err_t result=board_present(y,rows,(uint16_t *)pixels);
    pet_hub_overlay_suppress(false);
    *ctx->present_us+=(unsigned)(esp_timer_get_time()-began);
    return result==ESP_OK?KSN_OK:KSN_IO;
}

void shell_draw(const char *error, unsigned phase) {
#ifdef KASANE_P5_NOTICE_PROBE
    static int last_notice_active=-1;
    sys_notice current_notice;
    bool notice_active=sys_notify_active(sys_device_notifications(),&current_notice);
    if((int)notice_active!=last_notice_active){
        ESP_LOGI("KSN_P5_NOTICE","STATE active=%u overlay_ksn=%u ksn=%u composited=%u pending=%u",
                 (unsigned)notice_active,(unsigned)overlay_kasane_active(),
                 (unsigned)pocket_kasane_active(),(unsigned)pocket_kasane_notice_composited(),
                 (unsigned)pocket_kasane_system_pending());
        last_notice_active=(int)notice_active;
    }
#endif
    (void)phase;
#ifdef KASANE_P5_LOWHEAP_PROBE
    shell_lowheap_service();
#endif
    strip=board_strip();
#ifdef KASANE_D2_OVERLAY_PROC_PROBE
    if(!overlay_kasane_active()) {
        d2_overlay_fault_injected=false;
        d2_overlay_retry=false;
        d2_overlay_sends_before_failure=-1;
    }
#endif
#ifdef KASANE_P5_OVERLAY_REPAIR_PROBE
    if(!overlay_kasane_active()) {
#ifdef KASANE_P5_SEND_PHASE_PROBE
        if(repair_send_probe_active){
            ESP_LOGI("KSN_P5_REPAIR",
                "SEND_PHASE normal=%lu/%lu inject=%lu/%lu recover=%lu/%lu",
                (unsigned long)repair_send_probe.frames[KSN_REPAIR_SEND_NORMAL],
                (unsigned long)repair_send_probe.max_us[KSN_REPAIR_SEND_NORMAL],
                (unsigned long)repair_send_probe.frames[KSN_REPAIR_SEND_INJECT],
                (unsigned long)repair_send_probe.max_us[KSN_REPAIR_SEND_INJECT],
                (unsigned long)repair_send_probe.frames[KSN_REPAIR_SEND_RECOVER],
                (unsigned long)repair_send_probe.max_us[KSN_REPAIR_SEND_RECOVER]);
            memset(&repair_send_probe,0,sizeof(repair_send_probe));
            repair_send_probe_active=false;
        }
#endif
        (void)atomic_exchange(&overlay_repair_requested,false);
        (void)atomic_exchange(&overlay_repair_capture_requested,false);
        if(overlay_repair_capturing)board_capture(false);
        overlay_repair_capturing=false;
        overlay_repair_stage=0;
        overlay_repair_remaining=-1;
    } else if(atomic_exchange(&overlay_repair_requested,false)) {
        bool capture=atomic_exchange(&overlay_repair_capture_requested,false);
        if(!overlay_repair_stage) {
            overlay_repair_stage=1;
            overlay_repair_remaining=3;
            overlay_repair_capturing=capture;
            if(capture)board_capture(true);
            ESP_LOGI("KSN_P5_REPAIR","ARMED after=3");
        } else ESP_LOGW("KSN_P5_REPAIR","BUSY stage=%u",overlay_repair_stage);
    }
#endif
#ifdef KASANE_P5_SEND_PHASE_PROBE
    unsigned repair_stage_before=overlay_repair_stage;
#endif
#ifdef KASANE_P0_BUS_PROBE
    bool bus_trace=!error&&overlay_kasane_active()&&!board_capture_active();
    if(bus_trace)ksn_p0_bus_begin_frame();
#endif
    int64_t started=esp_timer_get_time();
    unsigned present_us=0, loop_us=0, hud_us=0;
#ifndef KASANE_FLOWER_FRAME_PROBE
    bool flower_scene_presented=true;
#endif
#ifdef KASANE_P5_NOTICE_PROBE
    uint32_t p5_bytes=0,p5_comp_us=0;
    unsigned p5_bands=0;
    bool p5_host_top_dynamic=false;
#endif
    float dt=animation_time?(started-animation_time)*0.000001f:0.033f;
    animation_time=started;
    float amount=1-expf(-dt/0.045f);
    category_pos=approach(category_pos,category,amount);
    item_pos=approach(item_pos,setting,amount);
    app_pos=approach(app_pos,app,amount);
    choice_pos=approach(choice_pos,choice,amount);
    depth_pos=approach(depth_pos,choices?1:0,amount);
    // One owner draws the LCD; eight rows at a time. No full-screen framebuffer.
    const uint16_t white=board_rgb(237,246,255), muted=board_rgb(122,169,197);
    int tilt_x,tilt_y;motion_get(&tilt_x,&tilt_y);
    // One row, one scene. What used to be four `if(mode==...)` chains scattered
    // through this function is now three calls through the table.
    const scene_ops_t *sc=scene();
    frame_started=started;
    sc->prepare(dt,tilt_x,tilt_y,sc->variant);
    int64_t after_prep=esp_timer_get_time();
    // Both of these used to run once per strip. Neither depends on the strip.
    // They stay inside hud_us so that the figure keeps meaning "all of the HUD"
    // across this change and the before and after can be subtracted; two timer
    // calls a frame is 1.8 us against the 6.2 ms being measured.
    hud_label_t labels[HUD_LABEL_MAX];hud_labels=labels;
    int64_t hud_once=esp_timer_get_time();
    HUD_FENCE;uint32_t f0=esp_cpu_get_cycle_count();HUD_FENCE;
    char meter[16];meter[0]=0;
    if(show_fps)snprintf(meter,sizeof(meter),"%2.0f FPS",fps);
    HUD_FENCE;uint32_t f1=esp_cpu_get_cycle_count();HUD_FENCE;
    hud_fmt_cy+=f1-f0;
    // 3.1 (revised 2026-09-09): AN OVERLAY ENDS THE MENU. Not "is drawn under
    // it" -- the home screen is in one state or the other, and this is where
    // that is decided. Laying the menu out anyway and then not painting it
    // would leave hud_labels holding rows nothing draws, which is a different
    // and worse thing than a menu that does not exist this frame.
    bool xmb=!overlay_running();
    if(!error&&xmb)menu_layout();
    HUD_FENCE;hud_menu_cy+=esp_cpu_get_cycle_count()-f1;HUD_FENCE;
    hud_us+=(unsigned)(esp_timer_get_time()-hud_once);
    if(!error&&overlay_kasane_active()) {
        shell_overlay_port host={.scene=sc,.meter=meter,.muted=muted,
            .loop_us=&loop_us,.hud_us=&hud_us,.present_us=&present_us};
        bool host_top_dynamic=show_fps||volume_shown_us||board_capture_active();
        ksn_display_port port={.ctx=&host,.strip=shell_overlay_strip,
            .present=shell_overlay_send,.width=LCD_W,.height=LCD_H,
            .strip_rows=STRIP_H,.text=&ksn_font_port};
        ksn_render_stats stats={0};
        unsigned loop_before=loop_us,hud_before=hud_us,present_before=present_us;
        int64_t composite_began=esp_timer_get_time();
        ksn_result result=overlay_kasane_present(&port,shell_overlay_backdrop,
                                                  host_top_dynamic,&stats);
#ifdef KASANE_D2_OVERLAY_PROC_PROBE
        if(d2_overlay_fault_injected) {
            ESP_LOGI("D2_OVERLAY","PRESENT result=%u bytes=%u pending=%u flower_mode=%u",
                     (unsigned)result,(unsigned)stats.transferred_bytes,
                     (unsigned)pocket_proc_pending(),(unsigned)(mode==3));
            if(d2_overlay_retry&&result==KSN_OK&&stats.bands) {
                ESP_LOGI("D2_OVERLAY","RETRY_OK bytes=%u",
                         (unsigned)stats.transferred_bytes);
                d2_overlay_retry=false;
            }
        }
#endif
#ifndef KASANE_FLOWER_FRAME_PROBE
        flower_scene_presented=result==KSN_OK;
#endif
#ifdef KASANE_P5_OVERLAY_REPAIR_PROBE
        if(overlay_repair_stage==2&&result==KSN_OK&&stats.bands) {
            ESP_LOGI("KSN_P5_REPAIR","REPAIR_OK bands=%u bytes=%u",
                     ksn_render_band_count(stats.bands),(unsigned)stats.transferred_bytes);
            if(overlay_repair_capturing)board_capture(false);
            overlay_repair_capturing=false;
            overlay_repair_stage=0;
            overlay_repair_remaining=-1;
        }
#endif
        ksn_p0_probe_transfer(stats.transferred_bytes,ksn_render_band_count(stats.bands));
        uint64_t elapsed=(uint64_t)(esp_timer_get_time()-composite_began);
        uint64_t excluded=(uint64_t)(loop_us-loop_before)+(hud_us-hud_before)+
                          (present_us-present_before);
        overlay_kasane_charge((uint32_t)(elapsed>excluded?elapsed-excluded:0));
#ifdef KASANE_P5_NOTICE_PROBE
        p5_bytes=stats.transferred_bytes;
        p5_bands=ksn_render_band_count(stats.bands);
        p5_comp_us=(uint32_t)(elapsed>excluded?elapsed-excluded:0);
        p5_host_top_dynamic=host_top_dynamic;
#endif
        if(result!=KSN_OK)
            ESP_LOGW("overlay","Kasane composite failed: %u",(unsigned)result);
    } else for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
        int64_t band=esp_timer_get_time();
        kernel_cycles+=sc->draw(strip,strip_y,strip_h);
        loop_us+=(unsigned)(esp_timer_get_time()-band);
        band=esp_timer_get_time();
        HUD_FENCE;uint32_t h0=esp_cpu_get_cycle_count();HUD_FENCE;
        if(sc->overlay)sc->overlay(strip,strip_y,strip_h);
        // Scene, then overlay. Nothing of the shell's own goes on top of it
        // here any more: 3.1 asks that only MODAL surfaces beat an overlay, and
        // those are whole screens of their own drawn from main.c's loop, not
        // rows composited into this frame. The menu that used to land on top of
        // this line does not exist while an overlay is up (see menu_layout
        // above), which is what makes the ordering here uninteresting rather
        // than load bearing.
        overlay_paint(strip,strip_y,strip_h);
        HUD_FENCE;uint32_t h1=esp_cpu_get_cycle_count();HUD_FENCE;
        if(show_fps)text(194,8,meter,1,muted);
        HUD_FENCE;uint32_t h3=esp_cpu_get_cycle_count();HUD_FENCE;
        hud_ovl_cy+=h1-h0;hud_fps_cy+=h3-h1;
        if(error) {
            text(24,69,"APP ERROR",2,white);text(24,94,error,1,muted);
            text(12,123,"ESC / ENTER TO RETURN",1,muted);
        } else if(xmb) paint_labels();
        paint_volume();
        HUD_FENCE;hud_menu_cy+=esp_cpu_get_cycle_count()-h3;HUD_FENCE;
        hud_us+=(unsigned)(esp_timer_get_time()-band);
        // Timed apart from the pixels: 240x135x2 bytes at 40 MHz is about
        // 13 ms of bit time whatever the arithmetic above costs, and that is
        // the floor any optimisation of it is measured against.
        int64_t sent=esp_timer_get_time();
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
        present_us+=(unsigned)(esp_timer_get_time()-sent);
    }
#ifndef KASANE_FLOWER_FRAME_PROBE
    if(mode==3) {
        if(flower_scene_presented) {
#ifdef KASANE_FLOWER_D1_PROBE
            bool d1_candidate=flower_scene_candidate;
            bool d1_fallback=flower_scene_use_committed;
#endif
            if(flower_scene_candidate) {
                flower_scene_committed^=1u;
                flower_scene_has_committed=true;
            }
#ifdef KASANE_FLOWER_D1_PROBE
            if(flower_d1_pressure_active) {
                uint32_t after=flower_d1_committed_hash(
                    flower_scene_frames[flower_scene_committed],
                    &flower_scene_rain[flower_scene_committed]);
                ESP_LOGI("FLOWER_D1_ALLOC",
                         "PRESSURE ready=1 candidate=%u committed=%u fallback=%u before=%08lx after=%08lx",
                         (unsigned)d1_candidate,(unsigned)flower_scene_has_committed,
                         (unsigned)d1_fallback,(unsigned long)flower_d1_before_hash,
                         (unsigned long)after);
                flower_d1_pressure_end();
                flower_d1_pressure_active=false;
                flower_d1_recovery_pending=true;
            } else if(flower_d1_recovery_pending) {
                uint32_t hash=flower_d1_committed_hash(
                    flower_scene_frames[flower_scene_committed],
                    &flower_scene_rain[flower_scene_committed]);
                ESP_LOGI("FLOWER_D1_ALLOC",
                         "RECOVERY candidate=%u committed=%u hash=%08lx",
                         (unsigned)d1_candidate,(unsigned)flower_scene_has_committed,
                         (unsigned long)hash);
                flower_d1_recovery_pending=false;
            }
#endif
            flower_scene_candidate=false;
            flower_scene_retry=false;
        } else {
            flower_scene_retry=true;
#ifdef KASANE_FLOWER_D1_PROBE
            if(flower_d1_pressure_active) {
                flower_d1_pressure_end();
                flower_d1_pressure_active=false;
                ESP_LOGE("FLOWER_D1_ALLOC","PRESENT_FAILED");
            }
#endif
        }
    }
#endif
    unsigned elapsed=(unsigned)(esp_timer_get_time()-started);
    if(overlay_kasane_active()&&!board_capture_active()){
        ksn_p0_probe_sample(KSN_P0_OVERLAY_DRAW,elapsed);
        ksn_p0_probe_sample(KSN_P0_OVERLAY_SEND,present_us);
        ksn_p0_probe_sample(KSN_P0_OVERLAY_COMPUTE,
                            elapsed>present_us?elapsed-present_us:0u);
#ifdef KASANE_P5_SEND_PHASE_PROBE
        ksn_repair_send_class phase=ksn_repair_send_classify(
            repair_stage_before,overlay_repair_stage);
        ksn_repair_send_record(&repair_send_probe,phase,present_us);
        if(phase!=KSN_REPAIR_SEND_NORMAL)repair_send_probe_active=true;
#endif
    }
#ifdef KASANE_P5_NOTICE_PROBE
    /* Log only after the measured frame has been sampled. This may perturb the
     * next interval, but not the draw sample being diagnosed. */
    static int last_notice_composited=-1;
    bool notice_composited=pocket_kasane_notice_composited();
    if((int)notice_composited!=last_notice_composited){
        ESP_LOGI("KSN_P5_NOTICE","COMPOSITED %u",(unsigned)notice_composited);
        last_notice_composited=(int)notice_composited;
    }
    if(elapsed>10000&&overlay_kasane_active()&&!board_capture_active())
        ESP_LOGI("KSN_P5_SPIKE",
                 "draw=%u present=%u prep=%u loop=%u hud=%u comp=%u bytes=%u bands=%u notice=%u composited=%u host=%u",
                 elapsed,present_us,(unsigned)(after_prep-started),loop_us,hud_us,
                 (unsigned)p5_comp_us,(unsigned)p5_bytes,p5_bands,
                 (unsigned)notice_active,(unsigned)notice_composited,
                 (unsigned)p5_host_top_dynamic);
#endif
#ifdef KASANE_P0_BUS_PROBE
    if(bus_trace)ksn_p0_bus_end_frame(present_us);
#endif
    if(!window_start)window_start=started;
#ifdef KASANE_FLOWER_AUDIO_PROBE
    if(mode==3){
        uint32_t free_now=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        uint32_t largest_now=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(free_now<flower_audio_min_free)flower_audio_min_free=free_now;
        if(largest_now<flower_audio_min_largest)flower_audio_min_largest=largest_now;
    }
#endif
    samples++;draw_sum+=elapsed;present_sum+=present_us;
    prep_sum+=(unsigned)(after_prep-started);loop_sum+=loop_us;hud_sum+=hud_us;
    if(elapsed>max_us)max_us=elapsed;
    int64_t now=esp_timer_get_time();
    if(now-window_start>=2000000) {
        fps=samples*1000000.0f/(now-window_start);
        // Split four ways, because "pixels" was standing for the phase tables,
        // the per-pixel loop, the stars and the menu at once, and only one of
        // those is worth vectorising.
        // clock= is the only way a host script can see whether the SNTP sync
        // took: the label it reports is drawn on the sail scene and nothing
        // else logs it, so checking it used to mean reading pixels.
        ESP_LOGI("background",
            "PERF mode=%u async=%d clock=%s fps=%.1f draw=%.2f prep=%.2f loop=%.2f kernel=%.2f "
            "hud=%.2f (ovl=%.2f fmt=%.2f fps=%.2f menu=%.2f) send=%.2f",
            mode,board_async_get(),solar_sail_time_label(),fps,(double)draw_sum/samples/1000.0,
            (double)prep_sum/samples/1000.0,(double)loop_sum/samples/1000.0,
            (double)kernel_cycles/samples/240000.0,
            (double)hud_sum/samples/1000.0,
            (double)hud_ovl_cy/samples/240000.0,(double)hud_fmt_cy/samples/240000.0,
            (double)hud_fps_cy/samples/240000.0,(double)hud_menu_cy/samples/240000.0,
            (double)present_sum/samples/1000.0);
#ifdef KASANE_D6_SD_AV_STREAM_PROBE
        if(mode==3) ESP_LOGI("D6_AV","HEAP free=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
#ifdef KASANE_FLOWER_AUDIO_PROBE
        if(mode==3){
            ESP_LOGI("FLOWER_AUDIO_PROBE",
                "tone=%u available=%u free_min=%u largest_min=%u frames=%u",
                (unsigned)flower_audio_active,(unsigned)sound_available(),
                (unsigned)flower_audio_min_free,(unsigned)flower_audio_min_largest,
                samples);
            flower_audio_min_free=flower_audio_min_largest=UINT32_MAX;
            flower_audio_active=(++flower_audio_windows%2u)!=0u;
            if(flower_audio_active){
                int32_t id=sound_tone(440,1900,0.12f,NULL,NULL);
                ESP_LOGI("FLOWER_AUDIO_PROBE","tone_request=%ld",(long)id);
            }
        }
#endif
#ifdef KASANE_FLOWER_FRAME_PROBE
        if(mode==3)ESP_LOGI("FLOWER_FRAME_PROBE",
            "arm=%u captures=%u capture_us=%.1f old_bytes=%u current_bytes=%u min_free=%u min_largest=%u valid=%u",
            (unsigned)flower_probe_arm,flower_probe_captures,
            flower_probe_captures?(double)flower_probe_capture_us/flower_probe_captures:0.0,
            (unsigned)flower_frame_bytes(flower_probe_frames[flower_probe_current^1u]),
            (unsigned)flower_frame_bytes(flower_probe_frames[flower_probe_current]),
            (unsigned)flower_probe_min_free,(unsigned)flower_probe_min_largest,
            (unsigned)flower_probe_valid);
        flower_probe_capture_us=0;flower_probe_captures=0;
        flower_probe_min_free=flower_probe_min_largest=UINT32_MAX;
        if(mode==3)flower_probe_arm=(++flower_probe_windows/2u)&1u;
#endif
#ifndef SCENE_AB
#define SCENE_AB 0
#endif
#if SCENE_AB
        // Same-binary A/B, one PERF window per arm, off in the shipping build.
        // The arms rotate all-on / kernel off / all-on / swap-into off, so every
        // "off" window has an all-on neighbour on each side in the same scene and
        // the paired difference is that one switch. The AB line carries the
        // window's own canopy cycles next to draw and send, because SPLIT's
        // 60-frame window does not line up with this 2-second one. Pair it with
        // the neighbours' mean; docs/perf/pie-simd.md 7 has the numbers it gave.
        {
            extern int g_garden_canopy_pie,g_board_swap_into,g_garden_decor_group;
            // The decor group width is the other knob this scene has: the profile
            // evaluation, gain and dither are per group, so the width divides them
            // (~8.5 instructions a pixel at 4, ~4.3 at 8) and it is also the whole of
            // the approximation -- the group shares its first column's profile, which
            // moves 13% of pixels by up to 33/255 between 4 and 8 (pie-opt-plan 9).
            // One full four-arm cycle per width, so the canopy/swap A/B above stays
            // unconfounded and the width still walks 8 -> 6 -> 4 over twelve arms.
            g_garden_decor_group=(ab_arm/4)%3==1?6:(ab_arm/4)%3==2?4:8;
            extern uint32_t garden_prof_canopy(uint32_t *rows);
            static unsigned ab_arm;
            uint32_t crows=0,ccy=garden_prof_canopy(&crows);
            ESP_LOGI("background","AB arm=%u pie=%d swap=%d decor=%d frames=%u canopy=%.3f (%u rows, %u cy/row) draw=%.2f send=%.2f",
                     ab_arm%4,g_garden_canopy_pie,g_board_swap_into,g_garden_decor_group,
                     samples,(double)ccy/samples/240000.0,crows/samples,crows?ccy/crows:0,
                     (double)draw_sum/samples/1000.0,(double)present_sum/samples/1000.0);
            ab_arm++;
            g_garden_canopy_pie=ab_arm%4!=1;
            g_board_swap_into=ab_arm%4!=3;
        }
#else
        // No flip in a shipped build. This used to invert the panel-transfer
        // path once per 2-second window so `async=` alternated and adjacent PERF
        // windows were a same-binary A/B of the queued transfer against the
        // blocking one -- cheaper on this board than two builds of the same code
        // (CLAUDE.md: placement moves this part by up to 15%), which is why the
        // shipped firmware used to spend half its windows on the path it had
        // decided against. That measurement is done and recorded in
        // docs/perf/pie-consolidation.md 3b (FLOWER: draw 38.73 -> 33.30 ms,
        // fps 24.95 -> 28.80); the queue is what ships, and the flip would only
        // make the picture's cost depend on the second.
#endif
        samples=0;draw_sum=0;present_sum=0;prep_sum=0;loop_sum=0;hud_sum=0;kernel_cycles=0;
        hud_fmt_cy=hud_ovl_cy=hud_fps_cy=hud_menu_cy=0;
        max_us=0;window_start=now;
    }
}
