// Generate host-rendered frames; none of these images are firmware assets.
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include "../main/scene/flower_species.c"
#include "../main/scene/glass_rain.c"
#include <stdio.h>
#include <assert.h>
static uint16_t frame[W*H];
int main(void) {
    for(int step=0;step<2700;step++) {
        glass_rain_prepare(1.0f/30,12345);
        unsigned mature=0;
        for(int i=0;i<RAIN_N;i++)mature+=drops[i].life>0&&drops[i].age>1.5f;
        if(mature>=3)break;
    }
    for(int i=0;i<45;i++) {
        flower_prepare(1.0f/15,0,0,FLOWER_SUNFLOWER);
        glass_rain_prepare(1.0f/15,12345);
        flower_draw(frame,0,H);glass_rain_draw(frame,0,H);
        char path[80];snprintf(path,sizeof path,".cache/rain-%02d.ppm",i);
        FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n240 135\n255\n");
        for(int p=0;p<W*H;p++) {
            unsigned v=frame[p];unsigned char rgb[3]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};
            assert(fwrite(rgb,1,3,f)==3);
        }
        assert(fclose(f)==0);
    }
}
