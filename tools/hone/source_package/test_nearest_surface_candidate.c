#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "nearest_surface_candidate.c"
static void close3(const double*a,const double*b){for(unsigned i=0;i<3;i++)assert(fabs(a[i]-b[i])<1e-10);}
static volatile double sink;
int main(void){
 const double vertices[]={0,0,0,2,0,0,0,2,0};uint32_t triangles[]={0,1,2};
 double bary[3],out[3],p[]={.5,.5,1};uint32_t face;
 assert(nearest_surface(p,vertices,3,triangles,1,out,bary,&face));close3(out,(double[]){.5,.5,0});
 assert(nearest_surface_offset(p,vertices,3,triangles,1,.125,out));close3(out,(double[]){.5,.5,.125});
 p[2]=-.01;assert(nearest_surface_offset(p,vertices,3,triangles,1,.125,out));close3(out,(double[]){.5,.5,-.125});
 p[2]=0;assert(nearest_surface_offset(p,vertices,3,triangles,1,.125,out));close3(out,p);
 p[0]=2;p[1]=2;p[2]=1;assert(nearest_surface(p,vertices,3,triangles,1,out,bary,&face));close3(out,(double[]){1,1,0});
 triangles[2]=1;assert(nearest_surface(p,vertices,3,triangles,1,out,bary,&face));close3(out,(double[]){2,0,0});
 double saved[3];for(unsigned i=0;i<3;i++)saved[i]=out[i];triangles[2]=3;assert(!nearest_surface_offset(p,vertices,3,triangles,1,.1,out));close3(out,saved);triangles[2]=2;assert(nearest_surface_offset(p,vertices,3,triangles,1,.1,out));
 clock_t start=clock();for(unsigned i=0;i<1000000;i++){p[0]=(i%100)*.02;assert(nearest_surface(p,vertices,3,triangles,1,out,bary,&face));sink+=out[0];}
 printf("PASS nearest interior/edge/degenerate/offset sides/coincident/failure retry; synthetic host %.1f ns/query triangle\n",1e9*(clock()-start)/CLOCKS_PER_SEC/1000000.);
}
