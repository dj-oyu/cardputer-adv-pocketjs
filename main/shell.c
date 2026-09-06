#include "shell.h"
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
#include "esp_log.h"

static uint16_t *strip;
static int strip_y, strip_h;
static unsigned mode;
static const char *names[]={"LEVEL WAVE","OCEAN + STARS"};
static int16_t ribbons[3][LCD_W];
static uint8_t softness[3][64];
static int16_t sine[256], distortion[LCD_W];
// Laid out for the vector row below: eight columns at a time, three planes it
// walks in order with one pointer — the bend, the second phase's x term, and
// |x-160|. The 16-byte alignment is not optional: a 128-bit load forces the low
// four address bits to zero rather than faulting.
static int16_t ocean_cols[LCD_W/8][3][8] __attribute__((aligned(16)));
// The same sine, pre-scaled by 16 and pre-divided by 6, as 32-bit entries
// because the indexed load reads four bytes per lane. Baking C's truncation
// into the table is what keeps the vector row bit-exact on negative values.
static int32_t ocean_sine16[256], ocean_sine6[256];
static int depth_phase[LCD_H], cross_phase[LCD_H];
static bool sine_ready;
static struct {int x,y;uint16_t color;} stars[36];
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t present_sum, prep_sum, loop_sum, hud_sum;
static uint64_t draw_sum;
static float fps;
static unsigned category,setting,app;
static const char *apps[]={"HELLO WORLD","SKK PRACTICE","PLAYGROUND","TUTORIAL"};
static const char *app_details[]={"JAVASCRIPT / POCKETJS","JAPANESE INPUT DRILL",
                                  "WRITE AND RUN JAVASCRIPT","LEARN TO WRITE IT"};
#define APP_N (sizeof(apps)/sizeof(apps[0]))
static float app_pos;
unsigned shell_app(void) { return app; }
static bool choices;
static unsigned choice;
static float category_pos, item_pos, choice_pos, depth_pos;
static int64_t animation_time;
static const char *categories[]={"APPS","SETTINGS"};
static const char *labels[]={"BACKGROUND","FPS DISPLAY","SOUND"};
static bool show_fps,sfx=true;
static nvs_handle_t prefs;
static bool prefs_ready;
// Defined below, next to the two implementations they choose between.
static void (*ocean_row)(uint16_t *,int,int,int,int);
static void (*ocean_row_vector)(uint16_t *,int,int,int,int);
void shell_init(void) {
    if(nvs_flash_init()==ESP_OK && nvs_open("home",NVS_READWRITE,&prefs)==ESP_OK) {
        prefs_ready=true;uint8_t v;
        if(nvs_get_u8(prefs,"background",&v)==ESP_OK)mode=v%2;
        if(nvs_get_u8(prefs,"fps",&v)==ESP_OK)show_fps=v!=0;
        if(nvs_get_u8(prefs,"sound",&v)==ESP_OK)sfx=v!=0;
    }
    sound_set_enabled(sfx);
    ESP_LOGI("settings","LOADED background=%u fps=%d sound=%d",mode,show_fps,sfx);
    // The vector row is claimed to be bit-exact, so nothing less is accepted.
    // Every row is compared, because span and haze differ down the screen and
    // the reflection is only on the centre ones.
    int worst=shell_ocean_selftest();
    if(worst==0) {
        ocean_row=ocean_row_vector;
        ESP_LOGI("background","ocean row: PIE");
    } else {
        ESP_LOGW("background","ocean row: scalar (PIE differs by %d)",worst);
    }
}
bool shell_key(board_key_t key) {
    if(key==KEY_BACK) {
        choices=false;sound_play(2);
        ESP_LOGI("shell","HOME_READY");
    } else if(choices&&(key==KEY_UP||key==KEY_DOWN)) {
        unsigned next=key==KEY_DOWN?1:0;
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
        if(key==KEY_DOWN&&next<2)next++;
        if(key==KEY_UP&&next>0)next--;
        if(next!=setting){setting=next;sound_play(0);}
        ESP_LOGI("settings","SELECT %u",setting);
    } else if(key==KEY_ENTER) {
        if(category==0){sound_play(1);return true;}
        if(!choices) {
            choices=true;choice=setting==0?mode:setting==1?show_fps:sfx;
            choice_pos=choice;sound_play(1);
            ESP_LOGI("settings","OPEN %u choice=%u",setting,choice);
            return false;
        }
        if(setting==0&&mode!=choice)shell_change_background(1);
        if(setting==1)show_fps=choice!=0;
        if(setting==2){sfx=choice!=0;sound_set_enabled(sfx);}
        choices=false;
        sound_play(1);
        if(prefs_ready) {
            esp_err_t err=nvs_set_u8(prefs,"background",mode);
            if(!err)err=nvs_set_u8(prefs,"fps",show_fps);
            if(!err)err=nvs_set_u8(prefs,"sound",sfx);
            if(!err)err=nvs_commit(prefs);
            if(err)ESP_LOGW("settings","Save failed: %s",esp_err_to_name(err));
        }
        ESP_LOGI("settings","VALUE background=%u fps=%d sound=%d",mode,show_fps,sfx);
    }
    return false;
}
void shell_change_background(int direction) {
    mode=(mode+2+direction)%2;
    window_start=0;samples=0;max_us=0;draw_sum=0;fps=0;
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

// Built once, and needed before the first frame so the start-up comparison
// between the scalar and vector rows has something real to work on.
static void build_tables(void) {
    if(sine_ready) return;
    for(int i=0;i<256;i++) sine[i]=(int16_t)(sinf(i*6.2831853f/256)*256);
    const float widths[]={18,5,24};const float brightness[]={14,32,21};
    for(int l=0;l<3;l++)for(int d=0;d<64;d++)
        softness[l][d]=(uint8_t)(brightness[l]*expf(-d*d/(2*widths[l]*widths[l])));
    for(int i=0;i<256;i++) {
        ocean_sine16[i]=sine[i]*16;
        ocean_sine6[i]=(sine[i]/6)*16;
    }
    for(int x=0;x<LCD_W;x++)
        ocean_cols[x>>3][2][x&7]=(int16_t)(x<160?160-x:x-160);
    sine_ready=true;
}

// One row of the ocean, below the horizon. Pulled out of the strip loop as a
// function of its own because it is the candidate for a hand-written vector
// version: keeping the scalar one intact gives that something to be checked
// against, byte for byte, before it is trusted with the screen.
//
// `depth` and `cross` are the row's two phases, `span` the half-width of the
// reflection and `haze` its distance fade — all constant across the row.
static void ocean_row_scalar(uint16_t *row, int depth, int cross,
                             int span, int haze) {
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

// The same row on the PIE unit, eight pixels a pass. Bit-exact with the scalar
// version above rather than approximate: both divisions were removed without
// losing a value — ripple/6 by baking C's truncation into a table, and the
// divide by the row's span by a reciprocal whose error is smaller than the
// result's own step. The clamps are gone because the channels provably stay
// inside 0..255 (3..157, 3..214, 3..233).
//
// SAR is set once, to 11, and every shift in the body is a multiply by a
// constant: the vector multiply keeps the low 16 bits of a 32-bit product
// shifted right by SAR, so a multiplier above 2048 shifts left and one below
// shifts right. That is also how the RGB565 fields are placed, because there
// is no 16-bit-lane shift instruction.
//
// q0,q1 hold indices then the red channel; q2 is the broadcast constant, read
// in order from k[]; q3 carries the swell, q4 the ripple then green, q5 the
// indexed-load scratch then blue. q6 and q7 are unused.
//
// Section numbers are the ESP32-S3 TRM's.
static void __attribute__((noinline))
ocean_row_pie(uint16_t *row, int depth, int cross, int span, int haze) {
    int16_t k[17] __attribute__((aligned(4))) = {
        (int16_t)(depth & 255),                /* the index mask makes the rest moot */
        (int16_t)(cross & 255),
        0x00FF,
        -180*16,                               /* the crest offset, at the x16 scale */
        (int16_t)span,
        (int16_t)((262144 + span - 1) / span), /* ceil(128*2048/span) */
        40,
        4096,                                  /* 256*16 */
        4,
        3,
        (int16_t)(haze + 20),
        (int16_t)(haze + 39),
        0x00F8,
        (int16_t)0x8000,                       /* x*32768>>11 = x*16, applied twice for r<<8 */
        0x00FC,
        16384,                                 /* x<<3 */
        256                                    /* x>>3 */
    };
    const int16_t *in=&ocean_cols[0][0][0];
    const int16_t *kp;
    const int32_t *ta=ocean_sine16, *tb=ocean_sine6;
    int zero=0, blocks=LCD_W/8, sar=11;
    __asm__ volatile(
        "wsr.sar        %[sar]\n"                     /* every EE.VMUL below shifts by this */
        "loopgtz        %[blocks], 1f\n"              /* 30 blocks of 8 pixels, no branch inside */
        "  mov          %[kp], %[k]\n"                /* rewind the constant walk */
        "  ee.vld.128.ip   q0, %[in], 16\n"           /* distortion[x..x+7]              1.8.88 */
        "  ee.vld.128.ip   q1, %[in], 16\n"           /* 2x + 2*distortion */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* depth                           1.8.95 */
        "  ee.vadds.s16    q0, q0, q2\n"              /*                                 1.8.70 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* cross */
        "  ee.vadds.s16    q1, q1, q2\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 0x00FF */
        "  ee.andq         q0, q0, q2\n"              /* (depth+bend)&255                1.8.1  */
        "  ee.andq         q1, q1, q2\n"
        /* One 32-bit lane per instruction; the unzip then packs the low halves. 1.8.37 */
        "  ee.ldxq.32      q3, q0, %[ta], 0, 0\n"
        "  ee.ldxq.32      q3, q0, %[ta], 1, 1\n"
        "  ee.ldxq.32      q3, q0, %[ta], 2, 2\n"
        "  ee.ldxq.32      q3, q0, %[ta], 3, 3\n"
        "  ee.ldxq.32      q4, q0, %[ta], 0, 4\n"
        "  ee.ldxq.32      q4, q0, %[ta], 1, 5\n"
        "  ee.ldxq.32      q4, q0, %[ta], 2, 6\n"
        "  ee.ldxq.32      q4, q0, %[ta], 3, 7\n"
        "  ee.vunzip.16    q3, q4\n"                  /* q3 = swell*16                   1.8.210 */
        "  ee.ldxq.32      q4, q1, %[tb], 0, 0\n"
        "  ee.ldxq.32      q4, q1, %[tb], 1, 1\n"
        "  ee.ldxq.32      q4, q1, %[tb], 2, 2\n"
        "  ee.ldxq.32      q4, q1, %[tb], 3, 3\n"
        "  ee.ldxq.32      q5, q1, %[tb], 0, 4\n"
        "  ee.ldxq.32      q5, q1, %[tb], 1, 5\n"
        "  ee.ldxq.32      q5, q1, %[tb], 2, 6\n"
        "  ee.ldxq.32      q5, q1, %[tb], 3, 7\n"
        "  ee.vunzip.16    q4, q5\n"                  /* q4 = (ripple/6)*16 */
        "  ee.vadds.s16    q4, q4, q3\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* -2880 */
        "  ee.vadds.s16    q4, q4, q2\n"              /* crest*16, still possibly negative */
        "  ee.vrelu.s16    q4, %[zero], %[zero]\n"    /* max(crest,0) without a register 1.8.184 */
        "  ee.vld.128.ip   q0, %[in], 16\n"           /* |x-160| */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"            /* span */
        "  ee.vsubs.s16    q1, q1, q0\n"              /*                                 1.8.198 */
        "  ee.vrelu.s16    q1, %[zero], %[zero]\n"    /* dx>=span leaves no reflection */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* ceil(262144/span) */
        "  ee.vmul.s16     q1, q1, q2\n"              /* reflection                      1.8.122 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 40 */
        "  ee.vadds.s16    q1, q1, q2\n"
        "  ee.vmul.s16     q1, q4, q1\n"              /* glint = crest*(40+refl)/128 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 4096 */
        "  ee.vadds.s16    q3, q3, q2\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 4 */
        "  ee.vmul.s16     q3, q3, q2\n"              /* shade = (swell+256)/32 */
        "  ee.vadds.s16    q3, q3, q1\n"              /* shade+glint */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 3 */
        "  ee.vadds.s16    q0, q1, q2\n"              /* red */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* haze+20 */
        "  ee.vadds.s16    q4, q3, q2\n"              /* green */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* haze+39 */
        "  ee.vadds.s16    q5, q3, q2\n"              /* blue */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 0xF8 */
        "  ee.andq         q0, q0, q2\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 32768 */
        "  ee.vmul.u16     q0, q0, q2\n"              /*                                 1.8.128 */
        "  ee.vmul.u16     q0, q0, q2\n"              /* red field in place */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 0xFC */
        "  ee.andq         q4, q4, q2\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 16384 */
        "  ee.vmul.u16     q4, q4, q2\n"              /* green field */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 256 */
        "  ee.vmul.u16     q5, q5, q2\n"              /* blue field */
        "  ee.orq          q0, q0, q4\n"              /*                                 1.8.42 */
        "  ee.orq          q0, q0, q5\n"
        "  ee.vst.128.ip   q0, %[row], 16\n"          /* eight pixels out                1.8.190 */
        "1:\n"
        : [row] "+a"(row), [in] "+a"(in), [kp] "=&a"(kp)
        : [k] "a"(k), [ta] "a"(ta), [tb] "a"(tb), [zero] "a"(zero),
          [blocks] "a"(blocks), [sar] "a"(sar)
        : "memory");
}

// Swapped for the vector implementation once it has agreed with the scalar row
// above at start-up.
static void (*ocean_row)(uint16_t *,int,int,int,int)=ocean_row_scalar;
static void (*ocean_row_vector)(uint16_t *,int,int,int,int)=ocean_row_pie;


// Rows differ in phase, span and haze, so one row proves little; these cover
// the horizon, the middle and the bottom, where span and haze are furthest
// apart and the reflection is on and off the centre.
static int compare_row(int y) {
    static uint16_t a[LCD_W] __attribute__((aligned(16)));
    static uint16_t b[LCD_W] __attribute__((aligned(16)));
    int depth=depth_phase[y], cross=cross_phase[y];
    int span=12+(y-36)/3, haze=24-(y-36)/5;
    ocean_row_scalar(a,depth,cross,span,haze);
    ocean_row_vector(b,depth,cross,span,haze);
    int worst=0;
    for(int x=0;x<LCD_W;x++) {
        int dr=((a[x]>>11)&31)-((b[x]>>11)&31);
        int dg=((a[x]>>5)&63)-((b[x]>>5)&63);
        int db=(a[x]&31)-(b[x]&31);
        if(dr<0) dr=-dr;
        if(dg<0) dg=-dg;
        if(db<0) db=-db;
        if(dr>worst) worst=dr;
        if(dg>worst) worst=dg;
        if(db>worst) worst=db;
    }
    return worst;
}

// The vector row reads ocean_cols, so both planes that change per frame have
// to be rebuilt whenever distortion does.
static void build_columns(void) {
    for(int x=0;x<LCD_W;x++) {
        ocean_cols[x>>3][0][x&7]=distortion[x];
        ocean_cols[x>>3][1][x&7]=(int16_t)(2*x+2*distortion[x]);
    }
}

int shell_ocean_selftest(void) {
    if(!ocean_row_vector) return -1;
    build_tables();
    // The phase tables are filled by the first draw; without them both sides
    // would agree on zeroes and prove nothing.
    for(int x=0;x<LCD_W;x++) distortion[x]=(int16_t)(perlin(x*0.018f,0.4f)*28);
    build_columns();
    for(int y=37;y<LCD_H;y++) {
        float d=800.0f/(y-28);
        depth_phase[y]=(int)((d*1.8f+1.2f)*40.7437f);
        cross_phase[y]=(int)((d*3.1f-0.7f)*40.7437f);
    }
    int worst=0;
    for(int y=37;y<LCD_H;y++) {
        int d=compare_row(y);
        if(d>worst) worst=d;
    }
    return worst;
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
static void menu_list(int x,float position,const char *const *items,unsigned count,
                      float opacity,const char *detail) {
    for(unsigned i=0;i<count;i++) {
        float distance=fabsf(i-position);
        float strength=1-fminf(distance,1)*0.70f;
        float y=item_y(i-position);
        // Fade while crossing the category text, so two lines never collide.
        float clearance=fminf(fabsf(y-34)/20,1);
        label(x,(int)lroundf(y),items[i],2,opacity*strength*clearance);
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
                menu_list(x,app_pos,apps,APP_N,visibility,app_details[app]);
            } else {
                const char *detail=setting==0?names[mode]:setting==1?(show_fps?"ON":"OFF"):(sfx?"ON":"OFF");
                menu_list(x,item_pos,labels,3,visibility*(1-depth_pos),detail);
            }
        }
    }
    if(depth_pos>0.005f) {
        int x=(int)lroundf(16+(1-depth_pos)*LCD_W);
        const char *toggles[]={"OFF","ON"};
        label(x,37,labels[setting],1,depth_pos);
        menu_list(x,choice_pos,setting==0?names:toggles,2,depth_pos,NULL);
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
    for(int i=0;i<36;i++) {
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
        for(int y=strip_y;y<strip_y+strip_h;y++) {
            uint16_t *row=strip+(size_t)(y-strip_y)*LCD_W;
            if(mode==1) {
                if(y<=36) {
                    uint16_t sky=board_rgb(5+y/12,13+y/3,29+y/2);
                    for(int x=0;x<LCD_W;x++) row[x]=sky;
                    continue;
                }
                ocean_row(row,depth_phase[y],cross_phase[y],
                          12+(y-36)/3, 24-(y-36)/5);
            } else {
                unsigned green=14+y/7, blue=30+y/5;
                for(int x=0;x<LCD_W;x++) {
                    unsigned light[3];
                    for(int l=0;l<3;l++) {
                        int d=y-ribbons[l][x];if(d<0)d=-d;
                        light[l]=d<64?softness[l][d]:0;
                    }
                    unsigned sum=light[0]+light[1];
                    row[x]=board_rgb(5+light[0]/4+light[1]/3+light[2]/2,
                                     green+sum+light[2]/2,
                                     blue+sum+light[2]);
                }
            }
        }
        loop_us+=(unsigned)(esp_timer_get_time()-band);
        band=esp_timer_get_time();
        for(int i=0;i<36;i++) {
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
            "PERF mode=%u fps=%.1f draw=%.2f prep=%.2f loop=%.2f hud=%.2f send=%.2f",
            mode,fps,(double)draw_sum/samples/1000.0,
            (double)prep_sum/samples/1000.0,(double)loop_sum/samples/1000.0,
            (double)hud_sum/samples/1000.0,(double)present_sum/samples/1000.0);
        samples=0;draw_sum=0;present_sum=0;prep_sum=0;loop_sum=0;hud_sum=0;
        max_us=0;window_start=now;
    }
}
