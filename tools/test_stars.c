// Host: gcc -O2 -Wall -Wextra -Werror -Itools/scene_stub tools/test_stars.c -lm -o .cache/test_stars
//
// scene/stars.c is shared by LEVEL WAVE and OCEAN + STARS and holds no state of
// its own -- the caller keeps the array inside its own scene_mem block. That
// makes it the one part of the two moved backgrounds that is pure C and can be
// checked on a host: the vector rows cannot, and are covered instead by
// tools/pie/test_kernels.py, which simulates their actual instructions.
//
// What is checked here is what the move could have broken: the drift is now
// folded in double against an absolute float clock, and both writers now clip
// against the strip they are handed rather than against shell.c's statics.
#include "../main/scene/stars.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define W LCD_W
#define H LCD_H
static star_t s[STARS_N];

// Both writers, over a frame drawn whole and the same frame drawn in strips.
static void frame(uint16_t *out, void (*draw)(const star_t *, uint16_t *, int, int)) {
    for(int i=0;i<W*H;i++) out[i]=0x1234;
    draw(s,out,0,H);
}
static void strips(uint16_t *out, void (*draw)(const star_t *, uint16_t *, int, int)) {
    struct { uint16_t head[8], data[W*8], tail[8]; } band;
    for(int y=0;y<H;y+=8) {
        int h=H-y<8?H-y:8;
        memset(&band,0xa5,sizeof band);
        for(int i=0;i<W*h;i++) band.data[i]=0x1234;
        draw(s,band.data,y,h);
        // Nothing outside the rows it was given.
        for(int k=0;k<8;k++) assert(band.head[k]==0xa5a5 && band.tail[k]==0xa5a5);
        for(int k=W*h;k<W*8;k++) assert(band.data[k]==0xa5a5);
        memcpy(out+y*W,band.data,(size_t)W*h*2);
    }
}

int main(void) {
    uint16_t whole[W*H], assembled[W*H];
    unsigned long lit_points=0, lit_layers=0;

    for(int f=0;f<400;f++) {
        double clock=f*0.37;
        int tx=(f%7)*60-180, ty=(f%5)*90-180;

        // Every star is on screen, whichever way it was wrapped.
        for(int parallax=0;parallax<2;parallax++) {
            stars_prepare(s,clock,tx,ty,parallax!=0);
            for(int i=0;i<STARS_N;i++) {
                assert(s[i].x>=0 && s[i].x<W);
                assert(s[i].y>=0 && s[i].y<H);
            }
            frame(whole,parallax?stars_draw_layers:stars_draw_points);
            strips(assembled,parallax?stars_draw_layers:stars_draw_points);
            assert(memcmp(whole,assembled,sizeof whole)==0);
            for(int i=0;i<W*H;i++)
                if(whole[i]!=0x1234) { if(parallax) lit_layers++; else lit_points++; }
        }
    }
    // Something was actually drawn, both ways, rather than a field of 0x1234.
    assert(lit_points>400*STARS_N && lit_layers>400*STARS_N);

    // The drift is exact after a long uptime. This is the reason the clock is
    // folded in double: at ten hours a float clock's step is coarser than one
    // frame's advance, and the field would freeze. A hundred seconds apart at
    // t=0 and at t=36000 must move the stars by the same amount.
    stars_prepare(s,0,0,0,false);
    star_t a[STARS_N]; memcpy(a,s,sizeof a);
    stars_prepare(s,100,0,0,false);
    star_t b[STARS_N]; memcpy(b,s,sizeof b);
    stars_prepare(s,36000,0,0,false);
    star_t c[STARS_N]; memcpy(c,s,sizeof c);
    stars_prepare(s,36100,0,0,false);
    unsigned moved_early=0,moved_late=0;
    for(int i=0;i<STARS_N;i++) {
        moved_early += a[i].x!=b[i].x || a[i].y!=b[i].y;
        moved_late  += c[i].x!=s[i].x || c[i].y!=s[i].y;
    }
    assert(moved_early==STARS_N && moved_late==STARS_N);

    // A null array is a no-op, not a crash: a scene whose block could not be
    // allocated calls straight through with NULL.
    stars_prepare(NULL,1,0,0,true);
    stars_draw_points(NULL,whole,0,H);
    stars_draw_layers(NULL,whole,0,H);
    stars_draw_points(s,NULL,0,H);

    printf("STARS_OK: 400 frames x2 writers, strip==frame, all on screen, "
           "%lu/%lu lit, drift exact at t=36000, null-safe\n",
           lit_points,lit_layers);
    return 0;
}
