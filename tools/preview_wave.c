// Host preview of LEVEL WAVE (main/scene/wave.c), drawn by the scene's own C
// with the scalar row standing in for the vector one. Not a firmware asset.
//
//   python tools/make_scene_tables.py .cache/generated
//   gcc -O2 -Wall -Wextra -Werror -Itools/scene_stub -Imain/scene
//       -I.cache/generated tools/preview_wave.c -lm -o .cache/preview_wave
//   .cache/preview_wave [frames] [outdir] [still]
//
// The board lies still for a second, then tips 0.35 rad right and back left,
// so the frames show the slosh, the bend of the shafts and the caustics.
// Each frame is drawn in 8-row strips, as the shell draws it.
#include "../main/scene/fxmath.c"
#include "../main/scene/scene_mem.c"
#include "../main/scene/stars.c"
#include "../main/scene/wave.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t frame[W*H];

int main(int argc, char **argv) {
    int frames=argc>1?atoi(argv[1]):90;
    const char *dir=argc>2?argv[2]:".cache";
    // A third argument of "still" keeps the board level throughout, to judge
    // the swell and ripples on their own.
    bool still=argc>3&&!strcmp(argv[3],"still");
    for(int i=0;i<frames;i++) {
        float t=i/30.0f, tilt=0;
        if(still);
        else if(t>1.0f&&t<2.0f)tilt=0.35f;
        else if(t>=2.0f&&t<3.0f)tilt=-0.2f;
        wave_prepare(1.0f/30,(int)(tilt*256),0,0);
        for(int y=0;y<H;y+=STRIP_H) {
            int h=H-y<STRIP_H?H-y:STRIP_H;
            wave_draw(frame+y*W,y,h);
            wave_overlay(frame+y*W,y,h);
        }
        char path[256];snprintf(path,sizeof path,"%s/wave-%03d.ppm",dir,i);
        FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n240 135\n255\n");
        for(int p=0;p<W*H;p++) {
            unsigned v=frame[p];
            unsigned char rgb[3]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};
            assert(fwrite(rgb,1,3,f)==3);
        }
        assert(fclose(f)==0);
    }
    return 0;
}
