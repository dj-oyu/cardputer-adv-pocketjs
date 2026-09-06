#include "shell.h"
#include "board.h"
#include "fonts.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_log.h"

static uint16_t strip[LCD_W*STRIP_H];
static int strip_y, strip_h;
static unsigned mode;
static const char *names[]={"WAVE + STARS","OCEAN + STARS"};
static int16_t sine[256], distortion[LCD_W];
static int depth_phase[LCD_H], cross_phase[LCD_H];
static bool sine_ready;
static struct {int x,y;uint16_t color;} stars[36];
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t draw_sum;
static float fps;
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
void shell_draw(const char *error, unsigned phase) {
    (void)phase;
    int64_t started=esp_timer_get_time();
    float t=(started%3600000000LL)*0.000001f;
    // One owner draws the LCD; eight rows at a time. No full-screen framebuffer.
    const uint16_t white=board_rgb(237,246,255), muted=board_rgb(122,169,197);
    int wave[LCD_W];
    for(int x=0;x<LCD_W;x++)wave[x]=83+sinf(x*0.020f+t*0.6f)*13+sinf(x*0.009f-t*0.27f)*8;
    if(!sine_ready) {
        for(int i=0;i<256;i++)sine[i]=(int16_t)(sinf(i*6.2831853f/256)*256);
        sine_ready=true;
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
    }
    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
        for(int y=strip_y;y<strip_y+strip_h;y++)for(int x=0;x<LCD_W;x++) {
            int distance=y-wave[x];if(distance<0)distance=-distance;
            unsigned glow=distance<15 ? (15-distance)*2:0;
            if(mode==1) {
                if(y<=36) {
                    strip[(y-strip_y)*LCD_W+x]=board_rgb(5+y/12,13+y/3,29+y/2);
                    continue;
                }
                int swell=sine[(depth_phase[y]+distortion[x])&255];
                int ripple=sine[(cross_phase[y]+x*2+distortion[x]*2)&255];
                int crest=swell-180+ripple/6;if(crest<0)crest=0;
                int span=12+(y-36)/3,dx=x-160;if(dx<0)dx=-dx;
                int reflection=dx<span?(span-dx)*128/span:0;
                int glint=crest*(40+reflection)/128;
                int shade=(swell+256)/32;
                int haze=24-(y-36)/5;
                strip[(y-strip_y)*LCD_W+x]=board_rgb(clamp(3+glint),clamp(20+shade+haze+glint),clamp(39+shade+haze+glint));
            } else strip[(y-strip_y)*LCD_W+x]=board_rgb(5+glow/3,14+y/7+glow,30+y/5+glow);
        }
        for(int i=0;i<36;i++) {
            int px=stars[i].x,py=stars[i].y;
            if(py+1<strip_y||py-1>=strip_y+strip_h)continue;
            uint16_t c=stars[i].color;
            pixel(px,py,c);
            if(i%7==0){pixel(px-1,py,muted);pixel(px+1,py,muted);pixel(px,py-1,muted);pixel(px,py+1,muted);}
        }
        text(12,8,"POCKET / CARDPUTER",1,muted);
        char meter[16];snprintf(meter,sizeof(meter),"%2.0f FPS",fps);
        text(194,8,meter,1,muted);
        text(12,20,names[mode],1,muted);
        // Minimal apps category icon, drawn from primitives.
        for(int y=32;y<48;y++)for(int x=27;x<43;x++)
            if(x<30||x>39||y<35||y>44)pixel(x,y,white);
        text(56,36,"APPS",1,white);
        text(20,72,">",2,board_rgb(119,233,255));
        text(44,70,error?"APP ERROR":"HELLO WORLD",2,white);
        text(44,92,error?error:"JAVASCRIPT / POCKETJS",1,muted);
        text(12,121,error?"ESC / ENTER TO RETURN":"ENTER OPEN  </> BACKGROUND",1,muted);
        ESP_ERROR_CHECK(board_present(strip_y,strip_h,strip));
    }
    unsigned elapsed=(unsigned)(esp_timer_get_time()-started);
    if(!window_start)window_start=started;
    samples++;draw_sum+=elapsed;if(elapsed>max_us)max_us=elapsed;
    int64_t now=esp_timer_get_time();
    if(now-window_start>=2000000) {
        fps=samples*1000000.0f/(now-window_start);
        ESP_LOGI("background","PERF mode=%u fps=%.1f draw_ms=%.2f max_ms=%.2f",mode,fps,
            (double)draw_sum/samples/1000.0,max_us/1000.0);
        samples=0;draw_sum=0;max_us=0;window_start=now;
    }
}
