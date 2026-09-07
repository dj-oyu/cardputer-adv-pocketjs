#include "paint.h"
#include "fonts.h"

static uint16_t *strip;
static int strip_y, strip_h;

void paint_begin(uint16_t *pixels, int y, int rows) {
    strip=pixels; strip_y=y; strip_h=rows;
}

void paint_ascii(int x, int y, const char *s, uint16_t colour) {
    if(!strip || y>=strip_y+strip_h || y+7<=strip_y) return;
    for(;*s;s++,x+=6) {
        unsigned c=(unsigned char)*s;
        if(c<32||c>126) c='?';
        for(int gy=0;gy<7;gy++) {
            int py=y+gy-strip_y;
            if(py<0||py>=strip_h) continue;
            for(int gx=0;gx<5;gx++)
                if(font_rows[(c-32)*7+gy]&(1<<(4-gx))) {
                    int px=x+gx;
                    if(px>=0&&px<LCD_W) strip[py*LCD_W+px]=colour;
                }
        }
    }
}

void paint_fill(int x, int y, int w, int h, uint16_t colour) {
    if(!strip) return;
    for(int r=0;r<h;r++) {
        int py=y+r-strip_y;
        if(py<0||py>=strip_h) continue;
        for(int c=0;c<w;c++) {
            int px=x+c;
            if(px>=0&&px<LCD_W) strip[py*LCD_W+px]=colour;
        }
    }
}
