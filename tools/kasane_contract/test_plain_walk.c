#include <assert.h>
#include <stdio.h>
#include <string.h>
/* plain_entry's walk against the one it replaced (plain_entry_ref), pixel for
 * pixel: g_ksn_proc_plain_walk 1 and 0 must paint the same band for every
 * line, band and piece height. Lines run far off the panel on every side, so
 * the early stop and the reference's fallback (entry short of the band) are
 * both reached; horizontal ones go to hspan in both arms. */
#include "../../main/ui/kasane/ksn_shiki_frame.c"

static uint32_t rng=0x2545f491u;
static int rnd(int lo,int hi){
    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
    return lo+(int)(rng%(uint32_t)(hi-lo+1));
}
int main(void){
    static uint16_t a[KSN_PROC_W*8],b[KSN_PROC_W*8];
    unsigned long lines=0,bands=0,painted=0;
    for(unsigned n=0;n<60000;n++){
        g_ksn_proc_hspan=n&1;   /* odd lines: horizontal ones take the walk too */
        /* Mostly near the panel, some far out, some steep, some flat. */
        const int span=n%3==0?1200:n%3==1?400:60;
        ksn_proc_segment s={(int16_t)rnd(-span,240+span),(int16_t)rnd(-span/2,135+span/2),
                            (int16_t)rnd(-span,240+span),(int16_t)rnd(-span/2,135+span/2),
                            (uint16_t)rnd(1,0xffff)};
        if(n%7==0)s.x1=s.x0;                     /* vertical */
        if(n%11==0)s.y1=(int16_t)(s.y0+rnd(-1,1)); /* near-flat */
        lines++;
        for(int height=1;height<=8;height++)
            for(int y=0;y+height<=KSN_PROC_H;y+=height==8?8:(height==4?4:height+3)){
                /* render_band's range test: only lines that reach the rows. */
                const int ymin=s.y0<s.y1?s.y0:s.y1,ymax=s.y0>s.y1?s.y0:s.y1;
                if(ymax<y||ymin>=y+height)continue;
                memset(a,0,sizeof a);memset(b,0,sizeof b);
                g_ksn_proc_plain_walk=1;plain_entry(&s,a,y,height);
                g_ksn_proc_plain_walk=0;plain_entry(&s,b,y,height);
                if(memcmp(a,b,sizeof a)){
                    fprintf(stderr,"MISMATCH line %d,%d -> %d,%d band y=%d height=%d\n",
                            s.x0,s.y0,s.x1,s.y1,y,height);
                    return 1;
                }
                for(unsigned i=0;i<KSN_PROC_W*8u;i++)painted+=a[i]!=0;
                bands++;
            }
    }
    g_ksn_proc_plain_walk=1;g_ksn_proc_hspan=1;
    printf("plain walk PASS: %lu lines, %lu bands, %lu pixels, both walks alike\n",lines,bands,painted);
    return 0;
}
