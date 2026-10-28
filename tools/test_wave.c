// Host: python tools/make_scene_tables.py .cache/generated
//       gcc -O2 -Wall -Wextra -Werror -Itools/scene_stub -Imain/scene
//           -I.cache/generated tools/test_wave.c -lm -o .cache/test_wave
//
// LEVEL WAVE (main/scene/wave.c) on the host, with the C row standing in for
// the vector one (tools/pie/test_kernels.py checks that the two agree).
// What is checked is what a picture would not show reliably (build it with
// -fsanitize=address,undefined as well, for the last one):
//   - the water's volume is conserved through tilts in both directions while
//     the slosh stays inside its clamp, and nothing goes non-finite;
//   - the surface comes to rest at the tilt's level slope once the slosh dies;
//   - drawing in 8-row strips gives the same frame as drawing it whole;
//   - the IMU's whole tilt range draws without leaving any table.
#include "../main/scene/fxmath.c"
#include "../main/scene/scene_mem.c"
#include "../main/scene/stars.c"
#include "../main/scene/wave.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t whole[W*H], strips[W*H];

static float volume(void) {
    float v=0;
    for(int i=0;i<SIM_N;i++)v+=wb->h[i];
    return v;
}

int main(void) {
    wave_prepare(1.0f/30,0,0,0);
    float v0=volume(), worst=0;
    for(int i=0;i<600;i++) {
        int tilt=(i/90)%2?-60:60;   // about +-0.23 rad, swapped every 3 s
        wave_prepare(1.0f/30,tilt,0,0);
        for(int k=0;k<SIM_N;k++)assert(isfinite(wb->h[k])&&fabsf(wb->h[k])<SLOSH_LIMIT);
        float d=fabsf(volume()-v0);
        if(d>worst)worst=d;
    }
    assert(worst<0.05f);
    printf("water: volume held to %.4f px*node over 600 frames of tilting\n",(double)worst);

    for(int i=0;i<900;i++)wave_prepare(1.0f/30,60,0,0);
    float level=fx_tanf(60/256.0f), err=0;
    for(int j=1;j<SIM_N;j++) {
        float e=fabsf((wb->h[j]-wb->h[j-1])/SIM_DX-level);
        if(e>err)err=e;
    }
    assert(err<0.01f);
    printf("water: at rest the surface slope is within %.4f of tan(tilt)=%.4f\n",(double)err,(double)level);

    wave_draw(whole,0,H);
    for(int y=0;y<H;y+=STRIP_H) {
        int h=H-y<STRIP_H?H-y:STRIP_H;
        wave_draw(strips+y*W,y,h);
    }
    assert(memcmp(whole,strips,sizeof whole)==0);
    printf("draw: strips match the whole frame\n");

    // The IMU's whole range, slammed from end to end, drawn every frame: the
    // water leaves the screen at one edge and fills it at the other, and no
    // index may leave its table (run this under -fsanitize to see that).
    static const int tilts[]={180,-180,0,180,-90,90};
    for(int i=0;i<360;i++) {
        wave_prepare(1.0f/30,tilts[i/60],(i%7-3)*40,0);
        for(int y=0;y<H;y+=STRIP_H) {
            int h=H-y<STRIP_H?H-y:STRIP_H;
            wave_draw(strips+y*W,y,h);
            wave_overlay(strips+y*W,y,h);
        }
        for(int k=0;k<SIM_N;k++)assert(isfinite(wb->h[k]));
    }
    printf("extremes: full tilt range drawn without a non-finite node\n");
    printf("WAVE_OK\n");
    return 0;
}
