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
    // ---------------------------------------------------------------------
    // The contract that replaces bit-exactness for the noise.
    //
    // The PIE kernel is allowed to move pixels -- the field is procedural mist
    // and a two-bit dither, and neither carries information a viewer can miss.
    // What it is NOT allowed to move is how bright the scene is, or how coarse
    // the noise is, because those are visible without anyone being able to say
    // which pixel changed. So the licence is written down as numbers here
    // rather than as a sentence in a commit message: same per-channel range,
    // same per-channel mean, and a dither that stays uniform and uncorrelated.
    //
    // A future kernel that drifts the mean dark, or a hash whose neighbours
    // agree too often, fails this and not a code review.
    // ---------------------------------------------------------------------
    double sum[3]={0,0,0};int lo[3]={99,99,99},hi[3]={0,0,0};unsigned n=0;
    unsigned long rough=0;
    for(int f=0;f<12;f++) {
        GardenFrame g;garden_prepare(&g,f*2.9f);
        for(int y=0;y<135;y++) {
            garden_row(before+y*240,y,&g);
            for(int x=0;x<240;x++) {
                uint16_t p=before[y*240+x];
                int c[3]={(p>>11)&31,(p>>5)&63,p&31};
                for(int k=0;k<3;k++) {
                    sum[k]+=c[k];
                    if(c[k]<lo[k])lo[k]=c[k];
                    if(c[k]>hi[k])hi[k]=c[k];
                }
                n++;
            }
            // The field's own spatial frequency, not just the dither's. A
            // kernel that quietly coarsens the noise -- one octave instead of
            // two, say -- keeps the mean and the range and would otherwise
            // pass everything above; this is what notices.
            for(int x=16;x<240;x++) {
                int a2=(before[y*240+x]>>5)&63,b2=(before[y*240+x-16]>>5)&63;
                rough+=abs(a2-b2);
            }
        }
    }
    double mr=sum[0]/n,mg=sum[1]/n,mb=sum[2]/n;
    // Measured on the scalar reference; the lane model drifts 0.13% dark, so
    // 1% is loose enough for an approximation and tight enough to catch a
    // scene that has actually changed brightness.
    assert(lo[0]==1&&hi[0]==13&&lo[1]==7&&hi[1]==25&&lo[2]==3&&hi[2]==10);
    assert(mr>4.752&&mr<4.848&&mg>13.822&&mg<14.101&&mb>7.086&&mb<7.229);
    // ---------------------------------------------------------------------
    // How far the lanes are allowed to be from the loop they replace. The
    // licence is a bound, not a hope: every pixel of twelve frames is compared
    // against garden_row_scalar, and both the share that moves and the largest
    // step in any channel are printed and asserted. For scale, the ocean
    // approximation this project already ships (d266ecf) moves 10.1% with the
    // same per-channel bound.
    // ---------------------------------------------------------------------
    unsigned moved=0,total=0,step[3]={0,0,0};
    for(int fr=0;fr<12;fr++) {
        GardenFrame g;garden_prepare(&g,fr*2.9f);
        for(int y=0;y<135;y++) {
            uint16_t lane[240],ref[240];
            garden_pixels_row(lane,y,&g);garden_row_scalar(ref,y,&g);
            for(int x=0;x<240;x++) {
                total++;
                if(lane[x]==ref[x])continue;
                moved++;
                int c[3]={(lane[x]>>11)&31,(lane[x]>>5)&63,lane[x]&31};
                int d2[3]={(ref[x]>>11)&31,(ref[x]>>5)&63,ref[x]&31};
                for(int k=0;k<3;k++) {
                    unsigned e=(unsigned)abs(c[k]-d2[k]);
                    if(e>step[k])step[k]=e;
                }
            }
        }
    }
    assert(step[0]<=1&&step[1]<=1&&step[2]<=1);
    assert(moved*100<total*12);
    printf("APPROX_OK: %u/%u pixels move (%.2f%%), max step r=%u g=%u b=%u\n",
           moved,total,100.0*moved/total,step[0],step[1],step[2]);
    // The dither: uniform over its four values, and uncorrelated with the
    // pixel to its left and the pixel above. A cheaper hash may replace the
    // one in garden.c, but not a worse one.
    unsigned bin[4]={0,0,0,0},same_h=0,same_v=0;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++) {
        int d=garden_dither(x,y);
        assert(d>=0&&d<4);bin[d]++;
        if(x)same_h+=d==garden_dither(x-1,y);
        if(y)same_v+=d==garden_dither(x,y-1);
    }
    double expect=135*240/4.0,chi=0;
    for(int k=0;k<4;k++)chi+=(bin[k]-expect)*(bin[k]-expect)/expect;
    double ph=(double)same_h/(135*239),pv=(double)same_v/(134*240);
    double edge=(double)rough/(12.0*135*224);
    // 1.5131 on the reference. This catches a change in the field's amplitude
    // or its scale; it does NOT catch swapping the fine octave for a second
    // coarse one, which moves it 1.4% because the coarse octave already carries
    // three quarters of the weight. That much is below any cheap statistic and
    // is left to the eye.
    assert(edge>1.437&&edge<1.589);
    assert(chi<20);                       // 3 dof; ~11.3 is the 99% point
    assert(ph>0.235&&ph<0.265&&pv>0.235&&pv<0.265);
    printf("CONTRACT_OK: mean r=%.4f g=%.4f b=%.4f  range %d..%d/%d..%d/%d..%d"
           "  edge=%.4f dither chi=%.2f P(left)=%.4f P(up)=%.4f\n",
           mr,mg,mb,lo[0],hi[0],lo[1],hi[1],lo[2],hi[2],edge,chi,ph,pv);
    printf("GARDEN_OK: changing=%u gradients=%u lit_left=%u rain_pixels=%u; frame parameters=%zu bytes; no persistent garden arrays\n",
           changing,gradient,lit_left,modified,sizeof(GardenFrame));
}
