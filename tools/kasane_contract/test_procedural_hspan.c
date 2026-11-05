/* The band renderer's horizontal-line fill (g_ksn_proc_hspan, ksn_shiki_frame.h)
 * against its own Bresenham walk (g_ksn_proc_hspan = 0) on random frames:
 * many lines overwriting each other in order, half of them horizontal, ends
 * anywhere in -480..720 (off the panel on either side, odd and even columns
 * for the 32-bit stores' alignment), points, every band start and height,
 * both the plain loop and the one a frame with pattern lines takes
 * (frame.ext). The band sits between guard pixels so a fill that runs past
 * its row or its band is caught. */
#include "ksn_shiki_frame.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 64
static ksn_proc_frame frame;
static uint16_t walk[GUARD+KSN_PROC_W*8+GUARD],fill[GUARD+KSN_PROC_W*8+GUARD];
static uint32_t seed=0x2545f491u;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static int pick(int lo,int hi){return lo+(int)(next()%(uint32_t)(hi-lo+1));}
/* Mostly on and near the panel, sometimes anywhere in the engine's range. */
static int coord(int size){
    switch(next()%8){
    case 0: return pick(-480,720);
    case 1: return pick(-20,-1);
    case 2: return pick(size,size+20);
    default: return pick(0,size-1);
    }
}

int main(void){
    unsigned long long bands=0,horizontal=0;
    for(unsigned trial=0;trial<4000;trial++){
        const unsigned count=1+next()%300;
        frame.count=(uint16_t)count;frame.ready=true;frame.ext=(uint8_t)(trial&1);
        for(unsigned i=0;i<count;i++){
            ksn_proc_segment *s=&frame.segments[i];
            s->x0=(int16_t)coord(KSN_PROC_W);s->y0=(int16_t)coord(KSN_PROC_H);
            switch(next()%4){
            case 0: case 1: s->x1=(int16_t)coord(KSN_PROC_W);s->y1=s->y0;horizontal++;break;  /* horizontal */
            case 2: s->x1=s->x0;s->y1=s->y0;break;                                            /* a point */
            default: s->x1=(int16_t)coord(KSN_PROC_W);s->y1=(int16_t)coord(KSN_PROC_H);break;
            }
            s->color=(uint16_t)next();
        }
        for(int y=0;y<KSN_PROC_H;y+=1+(int)(next()%9)){
            const int height=1+(int)(next()%8);
            if(height>KSN_PROC_H-y)continue;
            const uint16_t back=(uint16_t)next();
            for(unsigned i=0;i<sizeof walk/sizeof walk[0];i++)walk[i]=fill[i]=back;
            g_ksn_proc_hspan=0;
            assert(ksn_proc_render_band(&frame,walk+GUARD,y,height));
            g_ksn_proc_hspan=1;
            assert(ksn_proc_render_band(&frame,fill+GUARD,y,height));
            if(memcmp(walk,fill,sizeof walk)){
                for(unsigned i=0;i<sizeof walk/sizeof walk[0];i++)if(walk[i]!=fill[i]){
                    fprintf(stderr,"hspan mismatch: trial %u ext %u y=%d h=%d at %d (row %d col %d): walk %04x fill %04x\n",
                            trial,frame.ext,y,height,(int)i-GUARD,((int)i-GUARD)/KSN_PROC_W,((int)i-GUARD)%KSN_PROC_W,
                            walk[i],fill[i]);
                    break;
                }
                return 1;
            }
            bands++;
        }
    }
    printf("procedural hspan: PASS (%llu bands of random frames, %llu horizontal lines, both loops)\n",
           bands,horizontal);
    return 0;
}
