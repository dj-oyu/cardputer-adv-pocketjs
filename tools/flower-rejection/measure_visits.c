#include "../../main/scene/scene_mem.c"
#include "../../main/scene/garden.c"
#define ray_row ray_row_orig
#include "../../main/scene/flower.c"
#undef ray_row
#include <stdio.h>
#include <time.h>
static uint16_t fb[W*H];
static unsigned long visits,miss_d,miss_depth,shaded,bell_visits,bell_miss,petalrows,ellrows;
// Re-implementation of ray_row's classification only -- same arithmetic, no writes.
static void classify(int y){
    for(unsigned i=0;i<count;i++){
        const Petal *p=&petals[i];if(y<p->ymin||y>p->ymax)continue;
        petalrows++;if(!p->shape)ellrows++;
        float dy=(65-(y+.5f))/SCALE-p->c.y;
        for(int x=p->xmin;x<=p->xmax;x++){
            float dx=(x+.5f-180)/SCALE-p->c.x;
            if(p->shape){bell_visits++;
                float z=depth[x-X0];V n;
                if(!bell_hit(p,dx,dy,&z,&n))bell_miss++;else depth[x-X0]=z;
                continue;}
            visits++;
            float b=p->q[4]*dx+p->q[5]*dy;
            float c=p->q[0]*dx*dx+2*p->q[3]*dx*dy+p->q[1]*dy*dy-1;
            float d=b*b-p->q[2]*c;
            if(d<0){miss_d++;continue;}
            float dz=(-b+sqrtf(d))*p->invzz,z=dz+p->c.z;
            if(z<=depth[x-X0]){miss_depth++;continue;}
            depth[x-X0]=z;shaded++;
        }
    }
}
int main(void){
    const char*nm[]={"","VALLEY","SUNFLOWER","SNOWDROP","TULIP","DAFFODIL","CROCUS","CALLA"};
    for(int sp=1;sp<=7;sp++){
        visits=miss_d=miss_depth=shaded=bell_visits=bell_miss=petalrows=ellrows=0;
        for(int f=0;f<30;f++){
            elapsed=f*1.3f;flower_prepare(1.0f/30,180,180,(flower_species_t)sp);
            flower_draw(fb,0,H);
            for(int y=12;y<=119;y++){for(int x=0;x<FW;x++)depth[x]=-1000;classify(y);}
        }
        unsigned long tot=visits+bell_visits;
        printf("%-10s visits/frame %7lu  ellipsoid %6lu (d<0 %4.1f%%  behind %4.1f%%  shaded %4.1f%%)  bell %6lu (miss %4.1f%%)  part-rows %5lu (ellipsoid %5lu, %5.1f visits each)\n",
            nm[sp],tot/30,visits/30,
            visits?100.0*miss_d/visits:0,visits?100.0*miss_depth/visits:0,visits?100.0*shaded/visits:0,
            bell_visits/30, bell_visits?100.0*bell_miss/bell_visits:0,
            petalrows/30, ellrows/30, ellrows?(double)visits/ellrows:0);
    }
    // How much of ray_row is the rejectable quadratic, and how much is what
    // happens after a hit? classify() is ray_row's arithmetic with the sqrt,
    // normal, shade and dissolve removed.
    puts("");
    for(int sp=1;sp<=7;sp++){
        elapsed=7.0f;flower_prepare(1.0f/30,180,180,(flower_species_t)sp);
        const int N=400;clock_t t0,t1;
        t0=clock();for(int i=0;i<N;i++){for(int y=12;y<=119;y++){for(int x=0;x<FW;x++)depth[x]=-1000;ray_row_orig(fb+y*W,y);}}t1=clock();
        double full=(double)(t1-t0)/CLOCKS_PER_SEC/N*1000;
        t0=clock();for(int i=0;i<N;i++){for(int y=12;y<=119;y++){for(int x=0;x<FW;x++)depth[x]=-1000;classify(y);}}t1=clock();
        double setup=(double)(t1-t0)/CLOCKS_PER_SEC/N*1000;
        printf("%-10s ray_row %6.3f ms   quadratic+depth only %6.3f (%4.1f%%)   after-hit %6.3f (%4.1f%%)\n",
            nm[sp],full,setup,100*setup/full,full-setup,100*(full-setup)/full);
    }
    return 0;
}
