#include "shell.h"
#include "scene.h"
#include "wave.h"
#include "ocean.h"
#include "solar_sail.h"
#include "flower.h"
#include "glass_rain.h"
#include "board.h"
#include "motion.h"
#include "sound.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "fonts.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_log.h"

static uint16_t *strip;
static int strip_y, strip_h;
static unsigned mode;
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
    // flowers they had not seen. The row now rotates the three botanicals on
    // its own, each change hidden behind a dissolve; the interval and the fade
    // are named constants in flower.c. FLOWER RAY drew FLOWER_CRYSTAL, which
    // still exists and is still tested but is no longer a thing to pick.
    {"FLOWER", flower_scene_prepare, flower_scene_draw, NULL, 0},
};
#define BACKGROUND_N (sizeof(SCENES)/sizeof(SCENES[0]))
// mode is an index into SCENES and NVS can hand back anything, so every read
// goes through here rather than trusting the stored byte.
static const scene_ops_t *scene(void) {
    return &SCENES[mode<BACKGROUND_N?mode:0];
}
static const char *const toggles[]={"OFF","ON"};
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t present_sum, prep_sum, loop_sum, hud_sum;
// The vector rows alone, inside loop_sum. Worth its two cycle reads a row: for
// a long time "loop" was read as if it were the kernel, and it is not — the
// sky above the horizon and the scaffolding are a quarter of it.
static uint64_t kernel_cycles;
static uint64_t draw_sum;
static float fps;
static unsigned category,setting,app;
static const char *apps[]={"HELLO WORLD","SKK PRACTICE","PLAYGROUND","TUTORIAL",
                          "IMU CALIBRATION","POCKET PET","PET COMPANION"};
static const char *app_details[]={"JAVASCRIPT / POCKETJS","JAPANESE INPUT DRILL",
                                  "WRITE AND RUN JAVASCRIPT","LEARN TO WRITE IT",
                                  "FIND THE SENSOR AXES","CHOOSE AND CARE FOR YOUR PET",
                                  "AI USAGE / ALARM / TIMER"};
#define APP_N (sizeof(apps)/sizeof(apps[0]))
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
        if(prefs_ready) {
            esp_err_t err=ESP_OK;
            for(unsigned i=0;i<SETTING_N&&err==ESP_OK;i++)
                if(settings[i].key)
                    err=nvs_set_u8(prefs,settings[i].key,(uint8_t)settings[i].get());
            if(!err)err=nvs_commit(prefs);
            if(err)ESP_LOGW("settings","Save failed: %s",esp_err_to_name(err));
        }
        char summary[128]={0};
        settings_summary(summary,sizeof summary);
        ESP_LOGI("settings","VALUE %s",summary);
    }
    return false;
}
void shell_change_background(int direction) {
    mode=(unsigned)(((int)mode+(int)BACKGROUND_N+direction)%(int)BACKGROUND_N);
    window_start=0;samples=0;max_us=0;draw_sum=0;fps=0;
    present_sum=prep_sum=loop_sum=hud_sum=0;
    ESP_LOGI("background","MODE %u %s",mode,scene()->name);
}

// Every menu label is drawn twice (shadow, then face) for each of seventeen
// strips, so this runs some four hundred times a frame. It used to reach the
// buffer through a clipped per-dot writer (the shape scene/stars.c still
// uses), which re-tested four bounds for each lit dot; now the
// row is resolved once per row, blank glyph rows are skipped whole, and a run
// that has left the screen ends the string.
static void text(int x,int y,const char *s,int scale,uint16_t color) {
    if(y>=strip_y+strip_h || y+7*scale<=strip_y) return;
    for(;*s;s++,x+=6*scale) {
        if(x>=LCD_W) return;
        if(x+5*scale<=0) continue;
        unsigned c=(unsigned char)*s;
        if(c<32 || c>126) c='?';
        const uint8_t *glyph=font_rows+(c-32)*7;
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
static void label(int x,int y,const char *s,int scale,float emphasis) {
    if(emphasis<=0)return;
    if(emphasis>1)emphasis=1;
    text(x+1,y+1,s,scale,board_rgb(2,7,15));
    text(x,y,s,scale,board_rgb(65+172*emphasis,100+146*emphasis,125+130*emphasis));
}
static float item_y(float delta) {
    // Leave space for the category rail between the previous and focused item.
    return delta<0?69+57*delta:69+41*delta;
}
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
        label(x,(int)lroundf(y),item,2,opacity*strength*clearance);
    }
    float settled=1-fminf(fabsf(position-roundf(position))*4,1);
    if(detail)label(x,89,detail,1,opacity*settled*0.75f);
}
static void draw_menu(void) {
    for(unsigned c=0;c<2;c++) {
        float offset=(c-category_pos)*96;
        float visibility=1-fminf(fabsf(c-category_pos),1);
        int x=(int)lroundf(16+offset-depth_pos*160);
        label(x,37,categories[c],1,(0.35f+0.65f*visibility)*(1-depth_pos*0.6f));
        if(visibility>0.01f) {
            if(c==0) {
                menu_list(x,app_pos,apps,sizeof apps[0],APP_N,visibility,app_details[app]);
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

static void solar_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
    solar_sail_prepare(dt,tilt_x,tilt_y);
}
static void flower_scene_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
    flower_prepare_rotating(dt,tilt_x,tilt_y);
    glass_rain_prepare(dt,(uint32_t)frame_started);
}
static uint32_t solar_scene_draw(uint16_t *s,int y,int height) {
    // No inline assembly in this one: its cost is ordinary C and belongs in
    // loop=, not in kernel=.
    solar_sail_draw(s,y,height);
    return 0;
}
static uint32_t flower_scene_draw(uint16_t *s,int y,int height) {
    uint32_t c0=esp_cpu_get_cycle_count();
    flower_draw(s,y,height);
    glass_rain_draw(s,y,height);
    return esp_cpu_get_cycle_count()-c0;
}
static void solar_labels(uint16_t *s,int y,int height) {
    (void)s;(void)y;(void)height;
    text(12,121,solar_sail_time_label(),1,board_rgb(61,88,105));
    text(166,121,solar_sail_target(),1,board_rgb(87,125,144));
}

void shell_draw(const char *error, unsigned phase) {
    (void)phase;
    strip=board_strip();
    int64_t started=esp_timer_get_time();
    unsigned present_us=0, loop_us=0, hud_us=0;
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
    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
        int64_t band=esp_timer_get_time();
        kernel_cycles+=sc->draw(strip,strip_y,strip_h);
        loop_us+=(unsigned)(esp_timer_get_time()-band);
        band=esp_timer_get_time();
        if(sc->overlay)sc->overlay(strip,strip_y,strip_h);
        char meter[16];snprintf(meter,sizeof(meter),"%2.0f FPS",fps);
        if(show_fps)text(194,8,meter,1,muted);
        if(error) {
            text(24,69,"APP ERROR",2,white);text(24,94,error,1,muted);
            text(12,123,"ESC / ENTER TO RETURN",1,muted);
        } else draw_menu();
        hud_us+=(unsigned)(esp_timer_get_time()-band);
        // Timed apart from the pixels: 240x135x2 bytes at 40 MHz is about
        // 13 ms of bit time whatever the arithmetic above costs, and that is
        // the floor any optimisation of it is measured against.
        int64_t sent=esp_timer_get_time();
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
        present_us+=(unsigned)(esp_timer_get_time()-sent);
    }
    unsigned elapsed=(unsigned)(esp_timer_get_time()-started);
    if(!window_start)window_start=started;
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
            "PERF mode=%u clock=%s fps=%.1f draw=%.2f prep=%.2f loop=%.2f kernel=%.2f hud=%.2f send=%.2f",
            mode,solar_sail_time_label(),fps,(double)draw_sum/samples/1000.0,
            (double)prep_sum/samples/1000.0,(double)loop_sum/samples/1000.0,
            (double)kernel_cycles/samples/240000.0,
            (double)hud_sum/samples/1000.0,(double)present_sum/samples/1000.0);
        samples=0;draw_sum=0;present_sum=0;prep_sum=0;loop_sum=0;hud_sum=0;kernel_cycles=0;
        max_us=0;window_start=now;
    }
}
