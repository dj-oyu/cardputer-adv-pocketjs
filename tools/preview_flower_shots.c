// Host-only: renders every FLOWER_SHOTS entry for every species, so the shot
// list can be judged by eye without touching the board. It drives the same
// public path the firmware does (flower_prepare_rotating), then parks the
// rotation on one (shot, species) pair so the picture is the one the cut
// actually shows -- mid-dwell, fully faded in.
//
//   bash tools/preview_flower_shots.sh          # driven from there, per variant
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#ifndef PFS_FLOWER_SRC
#define PFS_FLOWER_SRC "../main/scene/flower.c"
#endif
#include PFS_FLOWER_SRC
#include "../main/scene/flower_species.c"
#include <assert.h>
#include <stdio.h>
#include "flower_catalog.h"

static uint16_t frame[W*H];

static void ppm(const char *path,const uint16_t *p) {
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(int i=0;i<W*H;i++) {
        unsigned c=p[i];
        unsigned char rgb[3]={((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(fclose(f)==0);
}

int main(int argc,char **argv) {
    const char *dir=argc>1?argv[1]:".cache";
    setvbuf(stdout,NULL,_IOLBF,0);
    // A settled rotation first, so the layout RNG is in the same regime the
    // device is in rather than at its seed.
    for(int i=0;i<400;i++)flower_prepare_rotating(0.04f,0,0);
    for(int v=0;v<FLOWER_VIEWS;v++) {
        for(int s=0;s<FLOWER_SPECIES_COUNT;s++) {
            // Park mid-dwell: fade is 1 there, which is the frame a viewer
            // spends nearly the whole cut looking at.
            elapsed=4;
            bloom_view=v;
            bloom_species=(flower_species_t)s;
            bloom_elapsed=FLOWER_SHOTS[v].hold*0.5f;
            flower_prepare_rotating(0.0f,0,0);
            assert(flower_fade()>0.999f);
            flower_draw(frame,0,H);
            char path[128];
            snprintf(path,sizeof path,"%s/shot-%d-%s.ppm",dir,v,flower_names[s]);
            ppm(path,frame);
        }
        printf("SHOT %d: pitch %+.2f zoom %.2f aim %.2f hold %.1fs\n",v,
               (double)FLOWER_SHOTS[v].pitch,(double)FLOWER_SHOTS[v].zoom,
               (double)FLOWER_SHOTS[v].aim,(double)FLOWER_SHOTS[v].hold);
    }
    return 0;
}
