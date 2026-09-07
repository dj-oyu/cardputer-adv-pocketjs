#include "../main/scene/garden.c"
#include "../main/scene/glass_rain.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t before[240*135],after[240*135],wet[240*135];
int main(void) {
    // Noise stays bounded and continuous at lattice and period boundaries.
    for(unsigned p=0;p<65536;p++) {
        int v=garden_motion(p,419),next=garden_motion(p+1,419);
        assert(v>=0&&v<=255&&abs(v-next)<=2);
        assert(v==garden_motion(p+65536,419));
    }
    // Seed changes rearrange vegetation, while drawing the same seed is stable.
    GardenFrame fixed;garden_prepare(&fixed,12);
    for(int y=0;y<135;y++)garden_row(before+y*240,y,&fixed);
    fixed.seed=12345;
    for(int y=0;y<135;y++)garden_row(after+y*240,y,&fixed);
    assert(memcmp(before,after,sizeof before));
    for(int y=0;y<135;y++)garden_row(wet+y*240,y,&fixed);
    assert(!memcmp(wet,after,sizeof wet));
    GardenFrame a,b;garden_prepare(&a,4);garden_prepare(&b,9);
    unsigned changing=0,gradient=0,lit_left=0;
    for(int y=0;y<135;y++) {
        struct {uint16_t lo[8],row[240],hi[8];} guard;
        memset(&guard,0xa5,sizeof guard);garden_row(guard.row,y,&a);
        for(int i=0;i<8;i++)assert(guard.lo[i]==0xa5a5&&guard.hi[i]==0xa5a5);
        memcpy(before+y*240,guard.row,480);garden_row(after+y*240,y,&b);
        for(int x=0;x<240;x++) {
            changing+=before[y*240+x]!=after[y*240+x];
            if(x<239)gradient+=guard.row[x]!=guard.row[x+1];
            if(x<120)lit_left+=((guard.row[x]>>5)&63)>6;
        }
    }
    assert(changing>10000&&gradient>12000&&lit_left>14000);
    // The lens has background features to sample, including behind the menu.
    drops[0]=(RainDrop){.x=44,.y=57,.start=25,.age=2,.life=3,.radius=4};
    drops[1]=(RainDrop){.x=175,.y=80,.start=47,.age=2,.life=3,.radius=4};
    memcpy(wet,before,sizeof wet);glass_rain_draw(wet,0,135);
    unsigned modified=0;for(int i=0;i<240*135;i++)modified+=wet[i]!=before[i];
    assert(modified>80&&modified<500);
    memcpy(after,before,sizeof after);
    for(int y=0;y<135;y+=8)glass_rain_draw(after+y*240,y,135-y<8?135-y:8);
    assert(!memcmp(wet,after,sizeof wet));
    printf("GARDEN_OK: changing=%u gradients=%u lit_left=%u rain_pixels=%u; frame parameters=%zu bytes; no persistent garden arrays\n",
           changing,gradient,lit_left,modified,sizeof(GardenFrame));
}
