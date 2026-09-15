// Render one flower frame on the host and write it as a PPM (P6, 240x135).
//
//   gcc -O2 tools/flower_frame_dump.c -lm -o /tmp/dump_float && /tmp/dump_float > /tmp/f.ppm
//   gcc -O2 -DFLOWER_FIXED_SQRT tools/flower_frame_dump.c -lm -o /tmp/dump_fixed && /tmp/dump_fixed > /tmp/x.ppm
//
// Same shape as tools/flower_shot_cost.c: the scene's own sources, the garden
// frame the renderer draws over, one call to flower_draw. Written for the one
// question the numbers cannot answer -- whether a change shows on the glass --
// so it prints the frame and nothing else; tools/flower_frame_diff.py compares
// two of them.
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include "../main/scene/flower_species.c"
#include <stdio.h>
#include <string.h>

static uint16_t full[W*H];

// The species and the moment are arguments so the two builds can be pointed at
// the same frame; the defaults are the middle of the first shot.
int main(int argc,char**argv) {
    int species=argc>1?atoi(argv[1]):0;
    float when=argc>2?(float)atof(argv[2]):20.0f;
    static unsigned char out[W*H*3];
    GardenFrame g={0};
    garden_prepare(&g,when);
    for(int y=0;y<H;y++)garden_row(full+y*W,y,&g);
    bloom_species=(flower_species_t)species;
    bloom_view=0;
    bloom_elapsed=FLOWER_SHOTS[0].hold*0.5f;
    elapsed=when;
    flower_prepare_rotating(0.0f,0,0);
    flower_draw(full,0,H);
    printf("P6\n%d %d\n255\n",W,H);
    for(int i=0;i<W*H;i++) {
        uint16_t p=full[i];
        out[i*3+0]=(unsigned char)(((p>>11)&31)*255/31);
        out[i*3+1]=(unsigned char)(((p>>5)&63)*255/63);
        out[i*3+2]=(unsigned char)((p&31)*255/31);
    }
    fwrite(out,3,(size_t)W*H,stdout);
    return 0;
}
