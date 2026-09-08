#include "../main/scene/garden.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    unsigned thin_reveal=0,wide_reveal=0,thin_cut=0,wide_cut=0,rims=0;
    for(unsigned seed=0;seed<2048;seed++) {
        GardenDecor thin={.seed=seed,.radius=18},wide=thin;wide.radius=25;
        thin_reveal+=garden_decor_arrival(&thin,40,90)==0;
        wide_reveal+=garden_decor_arrival(&wide,40,90)==0;
        for(int y=0;y<101;y++) {
            GardenDecorOcclusion a=garden_decor_occlusion(&thin,y);
            GardenDecorOcclusion b=garden_decor_occlusion(&wide,y);
            thin_cut+=a.amount>0;wide_cut+=b.amount>0;rims+=b.rim>0;
            assert(a.amount>=0&&a.amount<=255&&b.amount>=0&&b.amount<=255);
        }
        int previous=0;
        for(unsigned tick=0;tick<=3072;tick+=16) {
            thin.tick=tick;
            int top=garden_decor_arrival(&thin,20,90),bottom=garden_decor_arrival(&thin,60,90);
            assert(top>=bottom&&bottom>=previous);previous=bottom;
        }
        assert(previous==256);
    }
    assert(thin_reveal>wide_reveal*3&&wide_cut>thin_cut*3&&rims);
    printf("EVENTS_OK reveal thin/wide=%u/%u cut thin/wide=%u/%u rims=%u\n",
           thin_reveal,wide_reveal,thin_cut,wide_cut,rims);
    // In-scattering follows the local lit colour, not a fixed RGB addition.
    uint16_t cool=(uint16_t)(4<<11|14<<5|8),warm=(uint16_t)(12<<11|24<<5|8);
    uint16_t c=garden_decor_mix(cool,192,0,128),w=garden_decor_mix(warm,192,0,128);
    assert((w>>11)-(warm>>11)>(c>>11)-(cool>>11));
    assert((c&31)==(w&31)); // equal blue illumination, equal blue response
    for(unsigned p=0;p<65536;p++)assert(garden_decor_mix((uint16_t)p,0,0,128)==p);
    unsigned counts[5]={0},replaced=0;
    for(unsigned p=0;p<65536;p++) {
        int active=0;
        for(int slot=0;slot<4;slot++) {
            GardenDecor a=garden_decor(p,slot),b=garden_decor(p+1,slot);
            GardenDecor loop=garden_decor(p+65536,slot);
            assert(a.fade==loop.fade&&a.seed==loop.seed&&a.slope==loop.slope);
            assert(abs(a.fade-b.fade)<=3);
            if(a.seed==b.seed)assert(b.tick==a.tick+1); // one-way advection
            if(a.seed!=b.seed) {assert(!a.fade&&!b.fade);replaced++;}
            active+=a.fade>32;
        }
        counts[active]++;
    }
    assert(counts[0]&&counts[1]&&counts[2]&&counts[3]&&replaced);
    printf("LIFETIME_OK counts 0..4: %u %u %u %u %u; %u invisible replacements\n",
           counts[0],counts[1],counts[2],counts[3],counts[4],replaced);
    unsigned changed=0,darker=0,brighter=0,grazing=0;
    clock_t start=clock();
    for(int frame=0;frame<256;frame++) {
        GardenFrame f={0};garden_prepare(&f,frame*.5f);
        GardenFrame saved=f;
        for(int y=0;y<135;y++) {
            struct { uint16_t left[8],row[240],right[8]; } out;
            uint16_t base[240],repeat[240],wrap[240];
            memset(&out,0xa5,sizeof out);
            garden_pixels_row(base,y,&f);
            memcpy(out.row,base,sizeof base);memcpy(repeat,base,sizeof base);
            memcpy(wrap,base,sizeof base);
            garden_decor_row(out.row,y,&f);garden_decor_row(repeat,y,&f);
            GardenFrame loop=f;loop.phase+=65536;
            garden_decor_row(wrap,y,&loop);
            assert(!memcmp(repeat,out.row,sizeof repeat));
            assert(!memcmp(wrap,out.row,sizeof wrap));
            int center,half;garden_shaft(y,&f,&center,&half);
            for(int x=0;x<240;x++) {
                if(abs(x-center)<=half/2-6||y>=101)assert(out.row[x]==base[x]);
                if(abs(x-center)<half/2+12&&out.row[x]!=base[x])grazing++;
                changed+=out.row[x]!=base[x];
                darker+=((out.row[x]>>5)&63)<((base[x]>>5)&63);
                brighter+=((out.row[x]>>5)&63)>((base[x]>>5)&63);
            }
            for(int i=0;i<8;i++)assert(out.left[i]==0xa5a5&&out.right[i]==0xa5a5);
        }
        assert(!memcmp(&f,&saved,sizeof f));
    }
    assert(changed>10000&&darker>1000&&brighter>1000&&grazing>1000);
    printf("GRAZING_OK %u shoulder pixels, protected core unchanged\n",grazing);
    printf("DECOR_OK changed=%u shadow=%u light=%u sweep=%.3fs\n",
           changed,darker,brighter,(double)(clock()-start)/CLOCKS_PER_SEC);
    // Decorated garden at four moments.
    FILE *fp=fopen(".cache/garden-decor.ppm","wb");assert(fp);
    fprintf(fp,"P6\n240 540\n255\n");
    for(int t=0;t<4;t++) {
        GardenFrame f={0};garden_prepare(&f,3+t*1.25f);
        for(int y=0;y<135;y++) {
            uint16_t row[240];
            garden_row(row,y,&f);
            for(int x=0;x<240;x++) {
                unsigned char rgb[3]={(row[x]>>11)*255/31,
                    ((row[x]>>5)&63)*255/63,(row[x]&31)*255/31};
                fwrite(rgb,1,3,fp);
            }
        }
    }
    fclose(fp);
}
