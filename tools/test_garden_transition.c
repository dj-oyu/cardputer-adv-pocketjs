#include "../main/scene/garden.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    unsigned changed=0;
    for(unsigned seed=1;seed<=8;seed++) {
        unsigned from=seed*137u,to=seed*8191u;
        GardenFrame begin={0},finish={0},old={0},next={0};
        garden_prepare_layout(&begin,12,from,to,0);
        garden_prepare_layout(&old,12,from,from,256);
        garden_prepare_layout(&finish,12,from,to,256);
        garden_prepare_layout(&next,12,to,to,256);
        assert(begin.sun==old.sun&&begin.spread==old.spread&&begin.slant==old.slant);
        assert(finish.sun==next.sun&&finish.spread==next.spread&&finish.slant==next.slant);
        int last[3]={begin.sun,begin.spread,begin.slant};
        for(unsigned mix=0;mix<=256;mix++) {
            GardenFrame f={0};garden_prepare_layout(&f,12,from,to,mix);
            int shape[3]={f.sun,f.spread,f.slant};
            for(int i=0;i<3;i++){assert(abs(shape[i]-last[i])<=1);last[i]=shape[i];}
            if(mix%32)continue;
            GardenFrame a=f,b=f,saved=f;a.seed=from;b.seed=to;
            for(int y=0;y<135;y+=7) {
                uint16_t left[240],right[240];
                struct {uint16_t lo[8],row[240],hi[8];} out;
                memset(&out,0xa5,sizeof out);
                garden_row(left,y,&a);garden_row(right,y,&b);
                garden_row_blend(out.row,y,&f,from,mix);
                for(int j=0;j<8;j++)assert(out.lo[j]==0xa5a5&&out.hi[j]==0xa5a5);
                for(int x=0;x<240;x++) {
                    if(!mix)assert(out.row[x]==left[x]);
                    if(mix==256)assert(out.row[x]==right[x]);
                    if(left[x]==right[x])assert(out.row[x]==left[x]);
                    changed+=left[x]!=right[x];
                    const int shift[3]={11,5,0},mask[3]={31,63,31};
                    for(int k=0;k<3;k++) {
                        int l=(left[x]>>shift[k])&mask[k],r=(right[x]>>shift[k])&mask[k];
                        int v=(out.row[x]>>shift[k])&mask[k];
                        assert(v>=(l<r?l:r)&&v<=(l>r?l:r));
                    }
                }
            }
            assert(!memcmp(&f,&saved,sizeof f));
        }
    }
    assert(changed>1000);
    FILE *fp=fopen(".cache/garden-transition.ppm","wb");assert(fp);
    fprintf(fp,"P6\n240 675\n255\n");
    GardenFrame preview={0};
    for(unsigned step=0;step<=4;step++) {
        float t=step/4.0f;
        unsigned mix=(unsigned)(t*t*(3-2*t)*256);
        garden_prepare_layout(&preview,12+t*3,137,8191,mix);
        for(int y=0;y<135;y++) {
            uint16_t row[240];garden_row_blend(row,y,&preview,137,mix);
            for(int x=0;x<240;x++) {
                unsigned char rgb[3]={(row[x]>>11)*255/31,((row[x]>>5)&63)*255/63,(row[x]&31)*255/31};
                fwrite(rgb,1,3,fp);
            }
        }
    }
    fclose(fp);
    puts("TRANSITION_OK: exact endpoints, bounded RGB, light geometry continuity, row guards, unchanged state");
}
