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
static int depth_phase[LCD_H], cross_phase[LCD_H];
static bool sine_ready;
static struct {int x,y;uint16_t color;} stars[36];
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t present_sum;
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
void shell_init(void) {
    if(nvs_flash_init()==ESP_OK && nvs_open("home",NVS_READWRITE,&prefs)==ESP_OK) {
        prefs_ready=true;uint8_t v;
        if(nvs_get_u8(prefs,"background",&v)==ESP_OK)mode=v%2;
        if(nvs_get_u8(prefs,"fps",&v)==ESP_OK)show_fps=v!=0;
        if(nvs_get_u8(prefs,"sound",&v)==ESP_OK)sfx=v!=0;
    }
    sound_set_enabled(sfx);
    ESP_LOGI("settings","LOADED background=%u fps=%d sound=%d",mode,show_fps,sfx);
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
static void pixel(int x,int y,uint16_t c) {
    if (x>=0 && x<LCD_W && y>=strip_y && y<strip_y+strip_h) strip[(y-strip_y)*LCD_W+x]=c;
}
static void text(int x,int y,const char *s,int scale,uint16_t color) {
    if(y>=strip_y+strip_h || y+7*scale<=strip_y)return;
    for(;*s;s++,x+=6*scale) {
        unsigned c=(unsigned char)*s;
        if(c<32 || c>126)c='?';
        for(int yy=0;yy<7;yy++) for(int xx=0;xx<5;xx++)
            if(font_rows[(c-32)*7+yy] & (1<<(4-xx)))
                for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++)pixel(x+xx*scale+sx,y+yy*scale+sy,color);
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
    unsigned present_us=0;
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
    if(!sine_ready) {
        for(int i=0;i<256;i++)sine[i]=(int16_t)(sinf(i*6.2831853f/256)*256);
        const float widths[]={18,5,24};const float brightness[]={14,32,21};
        for(int l=0;l<3;l++)for(int d=0;d<64;d++)
            softness[l][d]=(uint8_t)(brightness[l]*expf(-d*d/(2*widths[l]*widths[l])));
        sine_ready=true;
    }
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
    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
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
                int depth=depth_phase[y], cross=cross_phase[y];
                int span=12+(y-36)/3;
                int haze=24-(y-36)/5;
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
    if(elapsed>max_us)max_us=elapsed;
    int64_t now=esp_timer_get_time();
    if(now-window_start>=2000000) {
        fps=samples*1000000.0f/(now-window_start);
        ESP_LOGI("background",
            "PERF mode=%u fps=%.1f draw_ms=%.2f pixels_ms=%.2f send_ms=%.2f max_ms=%.2f",
            mode,fps,(double)draw_sum/samples/1000.0,
            (double)(draw_sum-present_sum)/samples/1000.0,
            (double)present_sum/samples/1000.0,max_us/1000.0);
        samples=0;draw_sum=0;present_sum=0;max_us=0;window_start=now;
    }
}
