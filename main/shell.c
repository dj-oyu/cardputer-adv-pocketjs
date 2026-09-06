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
static const char *names[]={"WAVE","OCEAN","STARS","SEA + STARS"};
static int16_t field[24][41];
static struct {int x,y;uint16_t color;} stars[36];
static int64_t window_start;
static unsigned samples, max_us;
static uint64_t draw_sum;
static float fps;
void shell_change_background(int direction) {
    mode=(mode+4+direction)%4;
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
    if(mode==1||mode==3)for(int y=0;y<24;y++)for(int x=0;x<41;x++) {
        float nx=x*0.12f,ny=y*0.19f;
        field[y][x]=(perlin(nx+t*0.13f,ny-t*0.09f)+0.38f*perlin(nx*2.1f-t*0.09f,ny*2.1f+t*0.17f))*512;
    }
    if(mode>=2)for(int i=0;i<36;i++) {
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
            if(mode==1||mode==3) {
                int gx=x/6,gy=y/6,fx=x%6,fy=y%6;
                int top=field[gy][gx]*(6-fx)+field[gy][gx+1]*fx;
                int bottom=field[gy+1][gx]*(6-fx)+field[gy+1][gx+1]*fx;
                int n=(top*(6-fy)+bottom*fy)/36;
                int light=(bottom-top)/2+n/4;if(light<0)light=0;
                strip[(y-strip_y)*LCD_W+x]=board_rgb(clamp(4+light/16),clamp(20+y*14/100+(n*12+light*76)/512),clamp(39+y*22/100+(n*18+light*92)/512));
            } else strip[(y-strip_y)*LCD_W+x]=board_rgb(5+glow/3,14+y/7+glow,30+y/5+glow);
        }
        if(mode>=2)for(int i=0;i<36;i++) {
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
