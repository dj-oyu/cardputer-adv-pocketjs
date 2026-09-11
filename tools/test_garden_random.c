#include "../main/scene/garden.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    GardenFrame a={0},b={0},other={0};
    garden_random_init(&a,1);garden_random_init(&b,1);garden_random_init(&other,2);
    unsigned births=0,first=0;
    for(unsigned tick=0;tick<65536u*3;tick+=16) {
        GardenFrame previous=a;
        float time=(float)(tick&65535u)/512;
        garden_prepare(&a,time);garden_prepare(&b,time);garden_prepare(&other,time);
        assert(!memcmp(a.decor_seed,b.decor_seed,sizeof a.decor_seed));
        assert(memcmp(a.decor_seed,other.decor_seed,sizeof a.decor_seed));
        if(!tick)first=a.decor_seed[0];
        if(tick==65536)assert(a.decor_seed[0]!=first);
        if(tick)for(int slot=0;slot<4;slot++) {
            GardenDecor before=garden_decor_seeded(previous.phase,slot,previous.decor_seed[slot]);
            GardenDecor after=garden_decor_seeded(a.phase,slot,a.decor_seed[slot]);
            if(previous.decor_seed[slot]!=a.decor_seed[slot]) {
                assert(!before.fade&&!after.fade);births++;
            }
            assert(abs(before.fade-after.fade)<=4);
        }
        if(!(tick%4096)) {
            GardenFrame saved=a;
            uint16_t row[240],again[240];
            for(int y=0;y<135;y++) {
                garden_row(row,y,&a);garden_row(again,y,&a);
                assert(!memcmp(row,again,sizeof row));
            }
            assert(!memcmp(&a,&saved,sizeof a));
        }
    }
    assert(births>=44);
    printf("GARDEN_RANDOM_OK: %u invisible births, repeatable seeds, no 128s repeat, immutable draws; frame=%zu bytes\n",births,sizeof a);
}
