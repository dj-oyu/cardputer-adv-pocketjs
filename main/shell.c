#include "shell.h"
#include "solar_sail.h"
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
static const char *const names[]={"LEVEL WAVE","OCEAN + STARS","SOLAR SAIL"};
#define BACKGROUND_N (sizeof(names)/sizeof(names[0]))
static const char *const toggles[]={"OFF","ON"};
static int16_t ribbons[3][LCD_W];
static uint8_t softness[3][64];
static int16_t sine[256], distortion[LCD_W];
// Laid out for the vector row below: eight columns at a time, three planes it
// walks in order with one pointer — the bend and the second phase's x term,
// both times 16 because the row keeps its phases at that scale, and |x-160|.
// The 16-byte alignment is not optional: a 128-bit load forces the low four
// address bits to zero rather than faulting. The extra block is read, never
// drawn: the last block's fused loads fetch the phases of a block that does
// not exist, and they have to land somewhere we own.
static int16_t ocean_cols[LCD_W/8+1][3][8] __attribute__((aligned(16)));
// The three ribbons, eight columns at a time, for the wave row's vector form.
static int16_t wave_cols[LCD_W/8][3][8] __attribute__((aligned(16)));
// softness in the low half of each entry and its channel weight in the high
// half, so one indexed load fetches both and the unzip separates them. Entry
// 64 is zero, which is how "further than 64 rows away" stops being a branch.
static uint32_t wave_lut[3][65];
static int depth_phase[LCD_H], cross_phase[LCD_H];
static bool sine_ready;
static struct {int x,y;uint16_t color;} stars[36];
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
                          "IMU CALIBRATION"};
static const char *app_details[]={"JAVASCRIPT / POCKETJS","JAPANESE INPUT DRILL",
                                  "WRITE AND RUN JAVASCRIPT","LEARN TO WRITE IT",
                                  "FIND THE SENSOR AXES"};
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
    const char *const *values;   // SETTING_CHOICES: the value names
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
static void background_set(unsigned v) {if(v!=mode)shell_change_background((int)v-(int)mode);}
static unsigned fps_get(void) {return show_fps;}
static void fps_set(unsigned v) {show_fps=v!=0;}
static unsigned sound_get(void) {return sfx;}
static void sound_set(unsigned v) {sfx=v!=0;sound_set_enabled(sfx);}

static const setting_t settings[]={
    {"BACKGROUND", SETTING_CHOICES, names,   BACKGROUND_N, "background",
     background_get, background_set, SHELL_SCREEN_NONE},
    {"FPS DISPLAY",SETTING_CHOICES, toggles, 2,            "fps",
     fps_get,        fps_set,        SHELL_SCREEN_NONE},
    {"SOUND",      SETTING_CHOICES, toggles, 2,            "sound",
     sound_get,      sound_set,      SHELL_SCREEN_NONE},
};
#define SETTING_N (sizeof(settings)/sizeof(settings[0]))

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
    if(nvs_flash_init()==ESP_OK && nvs_open("home",NVS_READWRITE,&prefs)==ESP_OK) {
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
    ESP_LOGI("background","MODE %u %s",mode,names[mode]);
}
static float fade(float x) {return x*x*x*(x*(x*6-15)+10);}
static float mix(float a,float b,float t) {return a+(b-a)*t;}
static unsigned hash(int x,int y) {
    unsigned h=(unsigned)x*374761393u+(unsigned)y*668265263u;
    h=(h^(h>>13))*1274126177u;return h^(h>>16);
}
static float gradient(int x,int y,float dx,float dy) {
    switch(hash(x,y)&7) {
        case 0:return dx;case 1:return -dx;case 2:return dy;case 3:return -dy;
        case 4:return (dx+dy)*0.7071f;case 5:return (dx-dy)*0.7071f;
        case 6:return (-dx+dy)*0.7071f;default:return (-dx-dy)*0.7071f;
    }
}
static float perlin(float x,float y) {
    int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
    return mix(mix(gradient(ix,iy,fx,fy),gradient(ix+1,iy,fx-1,fy),fade(fx)),
               mix(gradient(ix,iy+1,fx,fy-1),gradient(ix+1,iy+1,fx-1,fy-1),fade(fx)),fade(fy));
}
static unsigned clamp(int v) {return v<0?0:v>255?255:(unsigned)v;}

// Built once, before the first frame that needs them.
static void build_tables(void) {
    if(sine_ready) return;
    for(int i=0;i<256;i++) sine[i]=(int16_t)(sinf(i*6.2831853f/256)*256);
    const float widths[]={18,5,24};const float brightness[]={14,32,21};
    for(int l=0;l<3;l++)for(int d=0;d<64;d++)
        softness[l][d]=(uint8_t)(brightness[l]*expf(-d*d/(2*widths[l]*widths[l])));
    for(int x=0;x<LCD_W;x++)
        ocean_cols[x>>3][2][x&7]=(int16_t)(x<160?160-x:x-160);
    for(int l=0;l<3;l++) {
        for(int d=0;d<64;d++) {
            unsigned lo=softness[l][d];
            unsigned hi=l==0?lo/4:l==1?lo/3:lo/2;
            wave_lut[l][d]=lo|(hi<<16);
        }
        wave_lut[l][64]=0;
    }
    sine_ready=true;
}

// One row of the ocean, below the horizon. `depth` and `cross` are the row's
// two phases, `span` the half-width of the reflection and `haze` its distance
// fade — all constant across the row.
//
// Not called: ocean_row_pie below does this, and this is what it means. The
// assembly cannot be read without it, and the two were checked against each
// other over every input before the scalar one was retired, so anything that
// changes here has to change there.
static void __attribute__((unused))
ocean_row_scalar(uint16_t *row, int depth, int cross, int span, int haze) {
    for(int x=0;x<LCD_W;x++) {
        int bend=distortion[x];
        int swell=sine[(depth+bend)&255];
        int ripple=sine[(cross+x*2+bend*2)&255];
        int crest=swell-180+ripple/6;if(crest<0)crest=0;
        int dx=x-160;if(dx<0)dx=-dx;
        int reflection=dx<span?(span-dx)*128/span:0;
        int glint=crest*(40+reflection)/128;
        int shade=(swell+256)/32;
        int lift=shade+haze+glint;
        row[x]=board_rgb(clamp(3+glint),clamp(20+lift),clamp(39+lift));
    }
}

// The same row on the PIE unit, eight pixels a pass, without the sine tables.
// Not bit-exact any more: the sine is a parabola with one refinement, and
// over every input one pixel in ten moves by one RGB565 step, none by more
// than two in green (checked against ocean_row_scalar with a lane-exact model
// of the instructions below; the animation itself is unchanged). Everything
// else — the reciprocal, the clamps that are not needed, SAR=11 and the RGB565
// placement by multiply — is as before.
//
// The phases arrive as 16*phase. Multiplying by 32768 with SAR=11 gives x*16,
// and the low 16 bits of that (1.8.129 keeps only those) are exactly
// (phase mod 256 - 128) * 256: the index mask and the centering fall out of
// one multiply. Call it s = 256u. Then
//     sine*16  ~=  u*(128-|u|)  =  (s/32) * (32767-|s|) >> 11
// with |s| from EE.VPRELU.S16 against -32768 shifted by 15 (1.8.182), which
// negates the lanes that are <= 0 and leaves the rest alone. The +1 in the
// phase constants keeps s off -32768 itself. The swell is then refined as
// y*(0.775+0.225|y|), which takes the worst error from 15/256 to 1.4/256; the
// ripple is divided by 6 on its way in and does not need it. The divide by 6
// is 341/2048 on |s|. shade+haze+20 is a single add, because 512*(haze+20)
// rides through the >>9 without touching its floor; blue is green+19.
//
// Every hazard in TRM table 1.7-2 is scheduled away, so the body issues one
// instruction a cycle: nothing loads a constant on its own. Each arithmetic
// instruction that has a .LD.INCP form (1.8.71, 1.8.123, 1.8.129, 1.8.199)
// also fetches, into the register it has just finished with, the constant that
// will be wanted two instructions later, from a per-row table of the constants
// already broadcast to 16 bytes; the same instructions bring in the next
// block's planes, so the loop carries four values across the block boundary.
// q7 holds 32768 for the whole row; the rest were allocated so those four land
// where the next block reads them. The rewind of the constant walk is the one
// plain instruction in the body.
//
// Section numbers are the ESP32-S3 TRM's.
static void __attribute__((noinline))
ocean_row_pie(uint16_t *row, int depth, int cross, int span, int haze) {
    int16_t k[21] __attribute__((aligned(4))) = {
        (int16_t)(((depth & 255) << 4) + 1),   /* depth16: the phase x16, +1 keeps s off -32768 */
        (int16_t)(((cross & 255) << 4) + 1),   /* cross16 */
        64,                                    /* s*64>>11 = s/32 */
        32767,                                 /* 32768-|s|, one short */
        341,                                   /* 2048/6: |s|/6 */
        5461,                                  /* 32768/6 */
        230,                                   /* 0.225 at x2048, halved for |y| at x4096 */
        1587,                                  /* 0.775 at x2048 */
        -180*16,                               /* the crest offset, at the x16 scale */
        (int16_t)span,
        (int16_t)((262144 + span - 1) / span), /* ceil(128*2048/span) */
        40,
        (int16_t)(4096 + 512*(haze + 20)),     /* 256*16 and haze+20 in one add */
        4,                                     /* >>9 */
        3,
        19,                                    /* blue = green + 19 */
        0x00F8,
        0x00FC,
        16384,                                 /* x<<3 */
        256,                                   /* x>>3 */
        (int16_t)0x8000                        /* x<<4, and the sine fold; resident in q7 */
    };
    int16_t kv[21][8] __attribute__((aligned(16)));   /* each of the above, broadcast */
    /* kp and ks are early-clobber: they start equal to kv and k, and without
       the & GCC hands them the same registers, so the walks never rewind. */
    const int16_t *in=&ocean_cols[0][0][0];
    const int16_t *kp, *ks;
    const int16_t *k8=&kv[20][0];
    int zero=0, s15=15, nk=21, blocks=LCD_W/8, sar=11;
    __asm__ volatile(
        /* broadcast the 21 constants once: some 60 cycles a row, two a block */
        "mov            %[ks], %[k]\n"
        "mov            %[kp], %[kv]\n"
        "loopgtz        %[nk], 0f\n"
        "  ee.vldbc.16.ip  q0, %[ks], 2\n"            /*                                 1.8.95 */
        "  ee.vst.128.ip   q0, %[kp], 16\n"           /*                                 1.8.192 */
        "0:\n"
        "wsr.sar        %[sar]\n"                     /* every EE.VMUL below shifts by this */
        "mov            %[kp], %[kv]\n"
        "ee.vld.128.ip  q7, %[k8], 16\n"              /* 32768, for the row              1.8.88 */
        "ee.vld.128.ip  q0, %[in], 16\n"              /* block 0: bend*16 */
        "ee.vld.128.ip  q1, %[in], 16\n"              /* block 0: the ripple phase */
        "ee.vld.128.ip  q2, %[kp], 16\n"              /* depth16 */
        "ee.vld.128.ip  q3, %[kp], 16\n"              /* cross16 */
        "loopgtz        %[blocks], 1f\n"              /* 30 blocks of 8 pixels, no branch inside */
        /* on entry: q0 = bend*16, q1 = ripple phase, q2 = depth16, q3 = cross16, kp -> 64 */
        "  ee.vadds.s16.ld.incp   q2, %[kp], q0, q0, q2\n"  /* bend16 + depth16: the swell phase, x16; load 64 */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q1, q1, q3\n"  /* the ripple phase, x16; load 32767 */
        "  ee.vmul.u16            q0, q0, q7\n"  /* s = 256u: x*16 kept to 16 bits folds mod 4096 and signs it */
        "  ee.vmul.u16            q1, q1, q7\n"
        "  ee.vmul.s16            q4, q0, q2\n"  /* s/32 = 8u */
        "  ee.vmul.s16.ld.incp    q5, %[kp], q2, q1, q2\n"  /* load 341 = 2048/6 */
        "  ee.vprelu.s16          q0, q0, q7, %[s15]\n"  /* |s|: -32768*x >> 15 = -x on the lanes <= 0 */
        "  ee.vprelu.s16          q1, q1, q7, %[s15]\n"
        "  ee.vsubs.s16.ld.incp   q0, %[kp], q3, q3, q0\n"  /* 32767 - |s|; load 5461 = 32768/6 */
        "  ee.vmul.s16.ld.incp    q5, %[kp], q1, q1, q5\n"  /* |s|/6; load 230 */
        "  ee.vmul.s16.ld.incp    q3, %[kp], q4, q4, q3\n"  /* swell*16 = 8u*(32767-|s|) >> 11 ~ u*(128-|u|); load 1587 */
        "  ee.vsubs.s16.ld.incp   q1, %[kp], q0, q0, q1\n"  /* 5461 - |s|/6; load -180*16 */
        "  ee.vprelu.s16          q6, q4, q7, %[s15]\n"  /* |swell16| */
        "  ee.vmul.s16.ld.incp    q0, %[kp], q2, q2, q0\n"  /* (ripple/6)*16; load span */
        "  ee.vmul.s16.ld.incp    q5, %[in], q6, q6, q5\n"  /* 0.225*|y| at x2048; load |x-160| */
        "  ee.vadds.s16.ld.incp   q2, %[kp], q1, q2, q1\n"  /* ripple/6*16 - 180*16; load ceil(262144/span) */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q6, q6, q3\n"  /* 0.775 + 0.225|y| at x2048; load 40 */
        "  ee.vsubs.s16.ld.incp   q5, %[kp], q0, q0, q5\n"  /* span - |x-160|; load 4096+512*(haze+20) */
        "  ee.vmul.s16.ld.incp    q6, %[kp], q4, q4, q6\n"  /* swell16 = y*(0.775+0.225|y|): the refinement; load 4 */
        "  ee.vrelu.s16           q0, %[zero], %[zero]\n"  /* no reflection past the span */
        "  ee.vadds.s16           q1, q1, q4\n"  /* crest*16, maybe negative */
        "  ee.vmul.s16.ld.incp    q2, %[kp], q0, q0, q2\n"  /* (span-dx)*128/span; load 3 */
        "  ee.vrelu.s16           q1, %[zero], %[zero]\n"  /* max(crest,0) */
        "  ee.vadds.s16.ld.incp   q5, %[kp], q4, q4, q5\n"  /* swell16 + 4096 + 512*(haze+20); load 19 */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q0, q0, q3\n"  /* 40 + reflection; load 0xF8 */
        "  ee.vmul.s16.ld.incp    q1, %[kp], q0, q1, q0\n"  /* glint = crest*(40+refl)/128; load 0xFC */
        "  ee.vmul.s16.ld.incp    q6, %[kp], q4, q4, q6\n"  /* shade+haze+20: the 512*(haze+20) rides through >>9 exactly; load 16384 */
        "  ee.vadds.s16           q2, q0, q2\n"  /* red = 3+glint */
        "  ee.vadds.s16.ld.incp   q0, %[kp], q4, q4, q0\n"  /* green = 20+lift; load 256 */
        "  mov                    %[kp], %[kv]\n"  /* rewind the constant walk */
        "  ee.andq                q3, q2, q3\n"
        "  ee.vadds.s16.ld.incp   q2, %[kp], q5, q4, q5\n"  /* blue = 39+lift; load next block: depth16 */
        "  ee.vmul.u16            q3, q3, q7\n"
        "  ee.andq                q1, q4, q1\n"
        "  ee.vmul.u16.ld.incp    q3, %[kp], q4, q3, q7\n"  /* red field; load next block: cross16 */
        "  ee.vmul.u16.ld.incp    q0, %[in], q5, q5, q0\n"  /* blue field; load next block: bend16 */
        "  ee.vmul.u16.ld.incp    q1, %[in], q6, q1, q6\n"  /* green field; load next block: ripple phase */
        "  ee.orq                 q4, q4, q5\n"
        "  ee.orq                 q4, q4, q6\n"
        "  ee.vst.128.ip          q4, %[row], 16\n"  /* eight pixels out */
        "1:\n"
        : [row] "+a"(row), [in] "+a"(in), [kp] "=&a"(kp), [ks] "=&a"(ks)
        : [k] "a"(k), [kv] "a"(&kv[0][0]), [k8] "a"(k8), [zero] "a"(zero), [s15] "a"(s15),
          [nk] "a"(nk), [blocks] "a"(blocks), [sar] "a"(sar)
        : "memory");
}

// One row of the wave background, on the vector unit. Bit-exact with the loop
// it replaces: the three channel weights (/4, /3, /2) are folded into the
// lookup table's high half, so an indexed load fetches the light and its
// weighted form together and the unzip separates them. "Further than 64 rows
// from the ribbon" is min(d,64) into an entry that holds zero, which is how a
// per-pixel branch disappears.
//
// q0,q1 work out |y-ribbon| then carry red; q2 is the broadcast constant; q3
// and q4 accumulate the plain and weighted sums; q5 holds 64 throughout; q6
// and q7 take each layer's lookup. All eight are in use.
//
// The early-clobber on kp is load-bearing. Without it GCC gave kp and k the
// same register — they start equal — and the rewind at the top of each block
// became an increment, so the constants marched off the end of the array.
static void __attribute__((noinline))
wave_row_pie(uint16_t *row, int y, unsigned green, unsigned blue) {
    int16_t k[10] __attribute__((aligned(4))) = {
        64, (int16_t)y, 5, (int16_t)green, (int16_t)blue,
        0x00F8, (int16_t)0x8000, 0x00FC, 16384, 256
    };
    const int16_t *in = &wave_cols[0][0][0];
    const int16_t *kp;
    const uint32_t *t0 = wave_lut[0], *t1 = wave_lut[1], *t2 = wave_lut[2];
    int blocks = LCD_W / 8, sar = 11;
    __asm__ volatile(
        "wsr.sar        %[sar]\n"                        /* SAR=11 for the EE.VMUL.U16 pack (1.8.128) */
        "mov            %[kp], %[k]\n"
        "ee.vldbc.16.ip q5, %[kp], 2\n"                  /* q5 = 64, resident                    (1.8.95) */
        "loopgtz        %[blocks], 1f\n"
        "  addi         %[kp], %[k], 2\n"                /* constant walk restarts at k[1] */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* q2 = y */
        "  ee.vld.128.ip   q0, %[in], 16\n"              /* ribbons[0]                           (1.8.88) */
        "  ee.vld.128.ip   q1, %[in], 16\n"              /* ribbons[1] */
        "  ee.vsubs.s16    q6, q2, q0\n"                 /* y - r0            (q2 3 back, q0 2 back) (1.8.198) */
        "  ee.vsubs.s16    q7, q2, q1\n"                 /* y - r1 */
        "  ee.vsubs.s16    q0, q0, q2\n"                 /* r0 - y */
        "  ee.vsubs.s16    q1, q1, q2\n"                 /* r1 - y */
        "  ee.vmax.s16     q0, q0, q6\n"                 /* |y - r0|                             (1.8.104) */
        "  ee.vmax.s16     q1, q1, q7\n"                 /* |y - r1| */
        "  ee.vmin.s16     q0, q0, q5\n"                 /* idx0 = min(d,64)                     (1.8.113) */
        "  ee.vmin.s16     q1, q1, q5\n"                 /* idx1 */
        /* layer 0 -> q3/q4, layer 1 -> q6/q7, interleaved                              (1.8.37) */
        "  ee.ldxq.32      q3, q0, %[t0], 0, 0\n"
        "  ee.ldxq.32      q6, q1, %[t1], 0, 0\n"
        "  ee.ldxq.32      q3, q0, %[t0], 1, 1\n"
        "  ee.ldxq.32      q6, q1, %[t1], 1, 1\n"
        "  ee.ldxq.32      q3, q0, %[t0], 2, 2\n"
        "  ee.ldxq.32      q6, q1, %[t1], 2, 2\n"
        "  ee.ldxq.32      q3, q0, %[t0], 3, 3\n"
        "  ee.ldxq.32      q6, q1, %[t1], 3, 3\n"
        "  ee.ldxq.32      q4, q0, %[t0], 0, 4\n"
        "  ee.ldxq.32      q7, q1, %[t1], 0, 4\n"
        "  ee.ldxq.32      q4, q0, %[t0], 1, 5\n"
        "  ee.ldxq.32      q7, q1, %[t1], 1, 5\n"
        "  ee.ldxq.32      q4, q0, %[t0], 2, 6\n"
        "  ee.ldxq.32      q7, q1, %[t1], 2, 6\n"
        "  ee.ldxq.32      q4, q0, %[t0], 3, 7\n"
        "  ee.ldxq.32      q7, q1, %[t1], 3, 7\n"
        "  ee.vld.128.ip   q0, %[in], 16\n"              /* ribbons[2] */
        "  ee.vunzip.16    q3, q4\n"                     /* q3 = light0, q4 = light0/4  (q4 written 3 back) (1.8.207) */
        "  ee.vunzip.16    q6, q7\n"                     /* q6 = light1, q7 = light1/3  (q7 written 3 back) */
        "  ee.vsubs.s16    q1, q2, q0\n"                 /* y - r2            (q0 3 back) */
        "  ee.vsubs.s16    q0, q0, q2\n"                 /* r2 - y */
        "  ee.vadds.s16    q3, q3, q6\n"                 /* light0+light1                        (1.8.70) */
        "  ee.vadds.s16    q4, q4, q7\n"                 /* light0/4+light1/3 */
        "  ee.vmax.s16     q0, q0, q1\n"                 /* |y - r2| */
        "  ee.vmin.s16     q0, q0, q5\n"                 /* idx2 */
        "  ee.ldxq.32      q6, q0, %[t2], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[t2], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[t2], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[t2], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[t2], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[t2], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[t2], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[t2], 3, 7\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 5      (y no longer needed) */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"               /* green */
        "  ee.vunzip.16    q6, q7\n"                     /* q6 = light2, q7 = light2/2  (q7 written 3 back) */
        "  ee.vadds.s16    q6, q6, q3\n"                 /* light0+light1+light2 */
        "  ee.vadds.s16    q3, q3, q7\n"                 /* light0+light1+light2/2 */
        "  ee.vadds.s16    q4, q4, q7\n"                 /* light0/4+light1/3+light2/2 */
        "  ee.vldbc.16.ip  q7, %[kp], 2\n"               /* blue */
        "  ee.vadds.s16    q0, q4, q2\n"                 /* r = 5 + ...      (q2 loaded 6 back) */
        "  ee.vadds.s16    q3, q3, q1\n"                 /* g = green + ...  (q1 loaded 5 back) */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 0xF8 */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"               /* 32768 */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"               /* 0xFC */
        "  ee.vadds.s16    q6, q6, q7\n"                 /* b = blue + ...   (q7 loaded 4 back) */
        "  ee.vldbc.16.ip  q7, %[kp], 2\n"               /* 16384 */
        "  ee.andq         q0, q0, q2\n"                 /* r & 0xF8         (q2 4 back)         (1.8.1) */
        "  ee.andq         q3, q3, q4\n"                 /* g & 0xFC         (q4 3 back) */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 256 */
        "  ee.vmul.u16     q0, q0, q1\n"                 /* r*16                                 (1.8.128) */
        "  ee.vmul.u16     q3, q3, q7\n"                 /* g<<3             (q7 3 back) */
        "  ee.vmul.u16     q0, q0, q1\n"                 /* r*256            (q0 mul 2 back) */
        "  ee.vmul.u16     q6, q6, q2\n"                 /* b>>3             (q2 3 back) */
        "  ee.orq          q0, q0, q3\n"                 /*                  (q0 2 back, q3 3 back) (1.8.45) */
        "  ee.orq          q0, q0, q6\n"                 /*                  (q6 mul 2 back) */
        "  ee.vst.128.ip   q0, %[row], 16\n"             /*                                      (1.8.192) */
        "1:\n"
        : [row] "+a"(row), [in] "+a"(in), [kp] "=&a"(kp)
        : [k] "a"(k), [t0] "a"(t0), [t1] "a"(t1), [t2] "a"(t2),
          [blocks] "a"(blocks), [sar] "a"(sar)
        : "memory");
}

// The vector row reads ocean_cols, so both planes that change per frame have
// to be rebuilt whenever distortion does.
static void build_columns(void) {
    for(int x=0;x<LCD_W;x++) {
        ocean_cols[x>>3][0][x&7]=(int16_t)(distortion[x]*16);
        ocean_cols[x>>3][1][x&7]=(int16_t)((2*x+2*distortion[x])*16);
    }
}

static void pixel(int x,int y,uint16_t c) {
    if (x>=0 && x<LCD_W && y>=strip_y && y<strip_y+strip_h) strip[(y-strip_y)*LCD_W+x]=c;
}
// Every menu label is drawn twice (shadow, then face) for each of seventeen
// strips, so this runs some four hundred times a frame. It used to reach the
// buffer through pixel(), which re-tested four bounds for each lit dot; now the
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
                const char *detail=entry->values?entry->values[entry->get()]:NULL;
                menu_list(x,item_pos,&settings[0].label,sizeof settings[0],SETTING_N,
                          visibility*(1-depth_pos),detail);
            }
        }
    }
    if(depth_pos>0.005f) {
        int x=(int)lroundf(16+(1-depth_pos)*LCD_W);
        const setting_t *entry=&settings[setting];
        label(x,37,entry->label,1,depth_pos);
        menu_list(x,choice_pos,entry->values,sizeof entry->values[0],entry->count,
                  depth_pos,NULL);
    }
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
    float t=(started%3600000000LL)*0.000001f;
    // One owner draws the LCD; eight rows at a time. No full-screen framebuffer.
    const uint16_t white=board_rgb(237,246,255), muted=board_rgb(122,169,197);
    int tilt_x,tilt_y;motion_get(&tilt_x,&tilt_y);
    int level_slope=(int)(tanf(tilt_x/256.0f)*256);
    build_tables();
    if(mode==0)for(int l=0;l<3;l++)for(int x=0;x<LCD_W;x++) {
        const float speeds[]={0.20f,0.60f,0.32f};const int depths[]={4,12,28};
        const int centers[]={54,82,116};
        float px=x+tilt_x*depths[l]/256.0f;
        int a=(int)((px*(0.014f+l*0.004f)+t*speeds[l]+l*1.6f)*40.7437f);
        int b=(int)((px*0.009f-t*0.24f+l)*40.7437f);
        ribbons[l][x]=(int16_t)(centers[l]+tilt_y*depths[l]/256.0f
            +(x-LCD_W/2)*level_slope/256
            +(sine[a&255]*(9+l*4)+sine[b&255]*6)/256.0f);
        wave_cols[x>>3][l][x&7]=ribbons[l][x];
    }
    if(mode==1) {
        // Perspective compresses the swell spacing towards a visible horizon.
        // Noise only bends the coherent wave fronts; it no longer paints clouds.
        for(int x=0;x<LCD_W;x++)distortion[x]=(int16_t)(perlin(x*0.018f,t*0.12f)*28);
        build_columns();
        for(int y=37;y<LCD_H;y++) {
            float depth=800.0f/(y-28);
            depth_phase[y]=(int)((depth*1.8f+t*1.2f)*40.7437f);
            cross_phase[y]=(int)((depth*3.1f-t*0.7f)*40.7437f);
        }
    }
    if(mode==2)solar_sail_prepare(dt,tilt_x,tilt_y);
    if(mode!=2)for(int i=0;i<36;i++) {
        unsigned seed=hash(i,91);float speed=1+(seed%13)*0.3f;
        stars[i].x=(int)fmodf((seed%240)+t*speed,240);
        stars[i].y=(int)fmodf(((seed>>8)%135)+t*(0.4f+speed*0.2f),135);
        float twinkle=0.5f+0.5f*sinf(t*(0.5f+(i%5)*0.13f)+i*2.7f);
        stars[i].color=board_rgb(45+twinkle*100,80+twinkle*120,105+twinkle*130);
        if(mode==0){int depth=4+(i%3)*12;
            stars[i].x=(stars[i].x+tilt_x*depth/256+LCD_W)%LCD_W;
            stars[i].y=(stars[i].y+tilt_y*depth/256+LCD_H)%LCD_H;}
    }
    int64_t after_prep=esp_timer_get_time();
    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
        int64_t band=esp_timer_get_time();
        // Everything that depends only on the row is lifted out of the column
        // loop: it used to be recomputed 240 times a row, 32,400 times a frame.
        // That is the whole of the saving — 23.2 ms of pixels became 18.8, and
        // 18.1 became 12.9. Turning the divisions into shifts and reciprocals
        // was measured too and moved nothing: this core divides in hardware, so
        // what counted was how often the work ran, not what it cost each time.
        if(mode==2)solar_sail_draw(strip,strip_y,strip_h);
        else for(int y=strip_y;y<strip_y+strip_h;y++) {
            uint16_t *row=strip+(size_t)(y-strip_y)*LCD_W;
            if(mode==1) {
                if(y<=36) {
                    uint16_t sky=board_rgb(5+y/12,13+y/3,29+y/2);
                    for(int x=0;x<LCD_W;x++) row[x]=sky;
                    continue;
                }
                uint32_t c0=esp_cpu_get_cycle_count();
                ocean_row_pie(row,depth_phase[y],cross_phase[y],
                          12+(y-36)/3, 24-(y-36)/5);
                kernel_cycles+=esp_cpu_get_cycle_count()-c0;
            } else {
                uint32_t c0=esp_cpu_get_cycle_count();
                wave_row_pie(row,y,14+y/7,30+y/5);
                kernel_cycles+=esp_cpu_get_cycle_count()-c0;
            }
        }
        loop_us+=(unsigned)(esp_timer_get_time()-band);
        band=esp_timer_get_time();
        if(mode!=2)for(int i=0;i<36;i++) {
            int px=stars[i].x,py=stars[i].y;
            if(py+2<strip_y||py-2>=strip_y+strip_h)continue;
            uint16_t c=stars[i].color;
            if(mode==0) {
                int layer=i%3,radius=layer==2?2:0;
                for(int yy=-radius;yy<=radius;yy++)for(int xx=-radius;xx<=radius;xx++) {
                    int sx=px+xx,sy=py+yy,d=xx*xx+yy*yy;
                    if(d>5||sx<0||sx>=LCD_W||sy<strip_y||sy>=strip_y+strip_h)continue;
                    int strength=layer==2?(6-d)*14:layer==0?64:200;
                    uint16_t old=strip[(sy-strip_y)*LCD_W+sx];
                    unsigned r=(((old>>11)&31)*8*(256-strength)+((c>>11)&31)*8*strength)/256;
                    unsigned g=(((old>>5)&63)*4*(256-strength)+((c>>5)&63)*4*strength)/256;
                    unsigned b=((old&31)*8*(256-strength)+(c&31)*8*strength)/256;
                    pixel(sx,sy,board_rgb(r,g,b));
                }
                continue;
            }
            pixel(px,py,c);
            if(i%7==0){pixel(px-1,py,muted);pixel(px+1,py,muted);pixel(px,py-1,muted);pixel(px,py+1,muted);}
        }
        if(mode==2) {
            text(12,121,solar_sail_time_label(),1,board_rgb(61,88,105));
            text(166,121,solar_sail_target(),1,board_rgb(87,125,144));
        }
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
        ESP_LOGI("background",
            "PERF mode=%u fps=%.1f draw=%.2f prep=%.2f loop=%.2f kernel=%.2f hud=%.2f send=%.2f",
            mode,fps,(double)draw_sum/samples/1000.0,
            (double)prep_sum/samples/1000.0,(double)loop_sum/samples/1000.0,
            (double)kernel_cycles/samples/240000.0,
            (double)hud_sum/samples/1000.0,(double)present_sum/samples/1000.0);
        samples=0;draw_sum=0;present_sum=0;prep_sum=0;loop_sum=0;hud_sum=0;kernel_cycles=0;
        max_us=0;window_start=now;
    }
}
