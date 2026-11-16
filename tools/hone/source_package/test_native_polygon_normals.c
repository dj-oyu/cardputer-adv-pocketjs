#include <assert.h>
#include <math.h>
#include "native_polygon_normals.c"
int main(void){
 float p[12]={0,0,0,1,0,0,1,0,0,0,0,0},out[12];double work[12];uint32_t off[2]={0,4},vi[4]={0,1,2,3};uint8_t smooth[1]={1};
 assert(polygon_normals(p,4,off,vi,smooth,1,4,work,out));
 for(int i=0;i<4;i++){assert(out[3*i]==0&&out[3*i+1]==0&&out[3*i+2]==1);}
 p[7]=p[10]=1;assert(polygon_normals(p,4,off,vi,smooth,1,4,work,out));for(int i=0;i<4;i++)assert(out[3*i+2]==1);
 p[8]=.1f;assert(polygon_normals(p,4,off,vi,smooth,1,4,work,out));for(int i=0;i<4;i++)assert(fabsf(out[3*i]*out[3*i]+out[3*i+1]*out[3*i+1]+out[3*i+2]*out[3*i+2]-1)<1e-6);
 vi[2]=9;assert(!polygon_normals(p,4,off,vi,smooth,1,4,work,out));vi[2]=2;p[0]=NAN;assert(!polygon_normals(p,4,off,vi,smooth,1,4,work,out));
 return 0;
}
