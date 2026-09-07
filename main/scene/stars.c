#include "stars.h"
#include "board.h"
#include <math.h>

#define W LCD_W
#define H LCD_H

// The same integer hash the ocean's noise uses. Duplicated rather than shared:
// four lines against a header whose only purpose would be to hold them, and the
// two uses are unrelated -- one seeds a star, the other picks a gradient.
static unsigned hash(int x,int y) {
    unsigned h=(unsigned)x*374761393u+(unsigned)y*668265263u;
    h=(h^(h>>13))*1274126177u;return h^(h>>16);
}

void stars_prepare(star_t *stars,double clock,int tilt_x,int tilt_y,bool parallax) {
    if(!stars)return;
    for(int i=0;i<STARS_N;i++) {
        unsigned seed=hash(i,91);
        double speed=1+(seed%13)*0.3;
        // The drift is folded here, in double, so the position is exact however
        // long the device has been on; only the fraction reaches a float.
        stars[i].x=(int)fmod((seed%240)+clock*speed,240);
        stars[i].y=(int)fmod(((seed>>8)%135)+clock*(0.4+speed*0.2),135);
        float twinkle=0.5f+0.5f*sinf((float)fmod(clock*(0.5+(i%5)*0.13),6.283185307)+i*2.7f);
        stars[i].color=board_rgb(45+twinkle*100,80+twinkle*120,105+twinkle*130);
        if(parallax) {
            int depth=4+(i%3)*12;
            stars[i].x=(stars[i].x+tilt_x*depth/256+W)%W;
            stars[i].y=(stars[i].y+tilt_y*depth/256+H)%H;
        }
    }
}

void stars_draw_points(const star_t *stars,uint16_t *strip,int y,int height) {
    if(!stars||!strip)return;
    const uint16_t muted=board_rgb(122,169,197);
    for(int i=0;i<STARS_N;i++) {
        int px=stars[i].x,py=stars[i].y;
        if(py+2<y||py-2>=y+height)continue;
        #define PUT(sx,sy,c) do{ int _x=(sx),_y=(sy); \
            if(_x>=0&&_x<W&&_y>=y&&_y<y+height)strip[(_y-y)*W+_x]=(c); }while(0)
        PUT(px,py,stars[i].color);
        if(i%7==0){PUT(px-1,py,muted);PUT(px+1,py,muted);
                   PUT(px,py-1,muted);PUT(px,py+1,muted);}
        #undef PUT
    }
}

void stars_draw_layers(const star_t *stars,uint16_t *strip,int y,int height) {
    if(!stars||!strip)return;
    for(int i=0;i<STARS_N;i++) {
        int px=stars[i].x,py=stars[i].y;
        if(py+2<y||py-2>=y+height)continue;
        uint16_t c=stars[i].color;
        int layer=i%3,radius=layer==2?2:0;
        for(int yy=-radius;yy<=radius;yy++)for(int xx=-radius;xx<=radius;xx++) {
            int sx=px+xx,sy=py+yy,d=xx*xx+yy*yy;
            if(d>5||sx<0||sx>=W||sy<y||sy>=y+height)continue;
            int strength=layer==2?(6-d)*14:layer==0?64:200;
            uint16_t old=strip[(sy-y)*W+sx];
            unsigned r=(((old>>11)&31)*8*(256-strength)+((c>>11)&31)*8*strength)/256;
            unsigned g=(((old>>5)&63)*4*(256-strength)+((c>>5)&63)*4*strength)/256;
            unsigned b=((old&31)*8*(256-strength)+(c&31)*8*strength)/256;
            strip[(sy-y)*W+sx]=board_rgb(r,g,b);
        }
    }
}
