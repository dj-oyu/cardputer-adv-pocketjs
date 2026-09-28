#include "ksn_procedural.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ksn_proc_frame frame;
static uint16_t expected[KSN_PROC_W*8],actual[KSN_PROC_W*8];

/* Frozen pre-optimization replay, including its tie and overwrite order. */
static void reference_band(const ksn_proc_segment *s,int y,int height,uint16_t *pixels){
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    for(;;){
        if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)
            pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;}
    }
}

static unsigned cases;
static void check(ksn_proc_segment segment){
    frame.count=1;frame.ready=true;frame.segments[0]=segment;
    static const int starts[]={0,6,7,8,16,32,64,96,120,127,128,134};
    for(unsigned n=0;n<sizeof starts/sizeof starts[0];n++){
        int y=starts[n];
        for(int height=1;height<=8;height=height==1?3:height==3?8:9){
            if(height>KSN_PROC_H-y)continue;
            for(unsigned i=0;i<KSN_PROC_W*8;i++)expected[i]=actual[i]=0x4321;
            reference_band(&segment,y,height,expected);
            assert(ksn_proc_render_band(&frame,actual,y,height));
            if(memcmp(expected,actual,sizeof expected)){
                fprintf(stderr,"band jump mismatch: (%d,%d)-(%d,%d), y=%d h=%d\n",
                        segment.x0,segment.y0,segment.x1,segment.y1,y,height);
                abort();
            }
            cases++;
        }
    }
}

int main(void){
    const int xs[]={12,120,228},ys[]={-16,0,7,64,128,146};
    for(unsigned xi=0;xi<sizeof xs/sizeof xs[0];xi++)
      for(unsigned yi=0;yi<sizeof ys/sizeof ys[0];yi++)
        for(int dx=0;dx<=20;dx++)for(int dy=0;dy<=20;dy++)
          for(int sx=-1;sx<=1;sx+=2)for(int sy=-1;sy<=1;sy+=2)
              check((ksn_proc_segment){xs[xi],ys[yi],
                                        xs[xi]+sx*dx,ys[yi]+sy*dy,0xf81f});
    check((ksn_proc_segment){-200,-200,400,200,0x07e0});
    check((ksn_proc_segment){400,200,-200,-200,0x07e0});
    check((ksn_proc_segment){-400,134,400,0,0x07e0});
    check((ksn_proc_segment){400,0,-400,134,0x07e0});
    printf("procedural band jump: %u band comparisons PASS\n",cases);
    return 0;
}
