#include "ui/kasane/frame_lane_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static uint16_t bounded[240*8],full[240*8];
static float depth[240];
int main(void){
    unsigned owner=1;clock_t bounded_ticks=0,full_ticks=0;
    for(unsigned scenario=0;scenario<32;scenario++){
        hone_alpha_packet packets[4];unsigned count=1+scenario%4;
        for(unsigned i=0;i<count;i++){
            float x=(float)((int)(scenario*37%640)-200+(int)i),y=(float)((int)(scenario*23%335)-100);
            packets[i]=(hone_alpha_packet){.v={{x,y,1,0},{x+20.5f,y,2,0},{x,y+30.25f,3,0}},
                .rgb={.25f,.5f,1},.alpha=.4f,.order=i};
        }
        ksn_mesh_snapshot opaque={.enabled=true};
        hone_alpha_snapshot alpha={.packets=packets,.count=count,.capacity=4,.owner=&owner,.generation=1,.ready=1};
        hone_frame_lane lane={.opaque=&opaque,.alpha=&alpha,.owner=&owner,.generation=1};
        assert(hone_frame_preflight_pin(&lane,&owner,1,240));
        hone_frame_lane reference=lane;reference.x0=reference.y0=0;reference.x1=240;reference.y1=135;
        for(unsigned band=1;band<=8;band*=2)for(unsigned y=0;y<135;y+=band){
            unsigned rows=135-y<band?135-y:band;
            for(unsigned j=0;j<rows*240;j++)bounded[j]=full[j]=0x1234;
            clock_t t=clock();assert(hone_frame_band(&lane,&owner,1,bounded,y,rows,depth));bounded_ticks+=clock()-t;
            t=clock();assert(hone_frame_band(&reference,&owner,1,full,y,rows,depth));full_ticks+=clock()-t;
            assert(!memcmp(bounded,full,rows*240*sizeof(*full)));
        }
        assert(hone_frame_unpin(&lane,&owner,1));
    }
    printf("PASS alpha AABB vs fullscan exact, 32 clipped/offscreen scenes x 1/2/4/8 rows; host bounded_us=%.0f fullscan_us=%.0f\n",1e6*bounded_ticks/CLOCKS_PER_SEC,1e6*full_ticks/CLOCKS_PER_SEC);
}
