#include "../../main/scene/scene_mem.c"
#include "../../main/scene/garden.c"
#include "../../main/scene/flower.c"
#include <stdio.h>
#include <time.h>
static uint16_t a[W*H],b[W*H];
static unsigned long long fnv(const uint16_t*p){unsigned long long h=1469598103934665603ULL;
 for(int i=0;i<W*H;i++){h^=p[i];h*=1099511628211ULL;}return h;}
// second copy, span narrowing disabled
#define ray_row ray_row_ref
#define flower_draw flower_draw_ref
#include "flower_ref_body.h"
int main(void){
    unsigned long diff=0,frames=0,maxstep=0;
    for(int sp=1;sp<=7;sp++)for(int f=0;f<40;f++){
        elapsed=f*0.83f;flower_prepare(1.0f/30,f%3?180:-180,f%2?180:-180,(flower_species_t)sp);
        flower_draw(a,0,H);
        flower_draw_ref(b,0,H);
        frames++;
        for(int i=0;i<W*H;i++)if(a[i]!=b[i]){diff++;
            int dr=abs((int)((a[i]>>11)&31)-(int)((b[i]>>11)&31));
            int dg=abs((int)((a[i]>>5)&63)-(int)((b[i]>>5)&63));
            int db=abs((int)(a[i]&31)-(int)(b[i]&31));
            unsigned m=dr>dg?dr:dg; if((unsigned)db>m)m=db; if(m>maxstep)maxstep=m;}
    }
    printf("frames=%lu differing pixels=%lu  max channel step=%lu\n",frames,diff,maxstep);
    const char*nm[]={"","VALLEY","SUNFLOWER","SNOWDROP","TULIP","DAFFODIL","CROCUS","CALLA"};
    double tn=0,to=0;
    puts("");
    for(int sp=1;sp<=7;sp++){
        elapsed=7.0f;flower_prepare(1.0f/30,180,180,(flower_species_t)sp);
        const int N=500;clock_t t0,t1;
        t0=clock();for(int i=0;i<N;i++)flower_draw(a,0,H);t1=clock();
        double nw=(double)(t1-t0)/CLOCKS_PER_SEC/N*1000;
        t0=clock();for(int i=0;i<N;i++)flower_draw_ref(b,0,H);t1=clock();
        double od=(double)(t1-t0)/CLOCKS_PER_SEC/N*1000;
        tn+=nw;to+=od;
        printf("%-10s flower_draw before %6.3f after %6.3f | ray %6.3f -> %6.3f\n",
               nm[sp],od,nw,od-0.378,nw-0.378);
    }
    printf("mean before %6.3f after %6.3f\n",to/7,tn/7);
    // The decisive number is not host time but how many per-pixel visits the
    // narrowing actually removes.
    puts("");
    for(int sp=1;sp<=7;sp++){
        elapsed=7.0f;flower_prepare(1.0f/30,180,180,(flower_species_t)sp);
        unsigned long vo=0,vn=0,pr=0;
        for(int y=12;y<=119;y++){
            for(unsigned i=0;i<count;i++){
                const Petal *p=&petals[i];if(y<p->ymin||y>p->ymax)continue;
                pr++;
                vo+=p->xmax-p->xmin+1;
                float dy=(65-(y+.5f))/SCALE-p->c.y;
                if(p->shape){vn+=p->xmax-p->xmin+1;continue;}
                int x0=p->xmin,x1=p->xmax;
                float A=p->q[4]*p->q[4]-p->q[2]*p->q[0];
                float B=dy*(p->q[4]*p->q[5]-p->q[2]*p->q[3]);
                float C=dy*dy*(p->q[5]*p->q[5]-p->q[2]*p->q[1])+p->q[2];
                if(A<0){float disc=B*B-A*C;
                    if(disc<0)continue;
                    float sd=sqrtf(disc),r1=(-B+sd)/A,r2=(-B-sd)/A;
                    float lo=r1<r2?r1:r2,hi=r1<r2?r2:r1;
                    int cl=(int)floorf((lo+p->c.x)*SCALE+179.5f)-1;
                    int ch=(int)ceilf ((hi+p->c.x)*SCALE+179.5f)+1;
                    if(cl>x0)x0=cl; if(ch<x1)x1=ch;}
                if(x1>=x0)vn+=x1-x0+1;
            }
        }
        printf("%-10s petal-rows %5lu  visits %6lu -> %6lu  (%4.1f%% removed, %.2f sqrt per visit removed)\n",
               nm[sp],pr,vo,vn,100.0*(vo-vn)/vo,vo>vn?(double)pr/(vo-vn):0.0);
    }
    return diff?1:0;
}
