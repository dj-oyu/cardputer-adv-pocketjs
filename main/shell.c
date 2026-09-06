#include "shell.h"
#include "board.h"
#include "fonts.h"
#include <math.h>
#include <string.h>

static uint16_t strip[LCD_W*STRIP_H];
static int strip_y, strip_h;
static void pixel(int x,int y,uint16_t c) {
    if (x>=0 && x<LCD_W && y>=strip_y && y<strip_y+strip_h) strip[(y-strip_y)*LCD_W+x]=c;
}
static void text(int x,int y,const char *s,int scale,uint16_t color) {
    for(;*s;s++,x+=6*scale) {
        unsigned c=(unsigned char)*s;
        if(c<32 || c>126)c='?';
        for(int yy=0;yy<7;yy++) for(int xx=0;xx<5;xx++)
            if(font_rows[(c-32)*7+yy] & (1<<(4-xx)))
                for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++)pixel(x+xx*scale+sx,y+yy*scale+sy,color);
    }
}
void shell_draw(const char *error, unsigned phase) {
    // One owner draws the LCD; eight rows at a time. No full-screen framebuffer.
    const uint16_t white=board_rgb(237,246,255), muted=board_rgb(122,169,197);
    float wave[LCD_W];
    for(int x=0;x<LCD_W;x++)wave[x]=sinf(x*0.020f+phase*0.045f)*13+sinf(x*0.009f-phase*0.02f)*8;
    for(strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        strip_h=LCD_H-strip_y<STRIP_H ? LCD_H-strip_y:STRIP_H;
        for(int y=strip_y;y<strip_y+strip_h;y++)for(int x=0;x<LCD_W;x++) {
            float distance=fabsf(y-(83+wave[x]));
            unsigned glow=distance<15 ? (unsigned)((15-distance)*2):0;
            pixel(x,y,board_rgb(5+glow/3,14+y/7+glow,30+y/5+glow));
        }
        text(12,8,"POCKET / CARDPUTER",1,muted);
        text(176,8,"M1",1,muted);
        // Minimal apps category icon, drawn from primitives.
        for(int y=32;y<48;y++)for(int x=27;x<43;x++)
            if(x<30||x>39||y<35||y>44)pixel(x,y,white);
        text(56,36,"APPS",1,white);
        text(20,72,">",2,board_rgb(119,233,255));
        text(44,70,error?"APP ERROR":"HELLO WORLD",2,white);
        text(44,92,error?error:"JAVASCRIPT / POCKETJS",1,muted);
        text(12,121,error?"ESC / ENTER TO RETURN":"ENTER OPEN       ESC BACK",1,muted);
        board_present(strip_y,strip_h,strip);
    }
}
