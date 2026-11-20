#include "ayatsuri_smooth_normals.h"
#include <assert.h>
#include <stdio.h>
int main(void){float p[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}},out[4][3],before[4][3];double w[4][3];uint32_t ix[]={0,1,2,0,3,1};
memset(out,42,sizeof out);assert(ayatsuri_smooth_normals(p,4,ix,6,w,out)==AYATSURI_OK);memcpy(before,out,sizeof out);
ix[5]=4;assert(ayatsuri_smooth_normals(p,4,ix,6,w,out)==AYATSURI_INVALID);assert(!memcmp(out,before,sizeof out));ix[5]=1;
p[3][2]=0;assert(ayatsuri_smooth_normals(p,4,ix,6,w,out)==AYATSURI_RANGE);assert(!memcmp(out,before,sizeof out));p[3][2]=1;
p[3][0]=NAN;assert(ayatsuri_smooth_normals(p,4,ix,6,w,out)==AYATSURI_INVALID);assert(!memcmp(out,before,sizeof out));p[3][0]=0;
assert(ayatsuri_smooth_normals(p,4,ix,3,w,out)==AYATSURI_RANGE);assert(!memcmp(out,before,sizeof out));
assert(ayatsuri_smooth_normals(p,4,ix,5,w,out)==AYATSURI_INVALID);assert(!memcmp(out,before,sizeof out));
assert(ayatsuri_smooth_normals(p,4,ix,6,w,p)==AYATSURI_INVALID);
assert(ayatsuri_smooth_normals(p,0,ix,6,w,out)==AYATSURI_INVALID);
{float plane[3][3]={{0,0,0},{1,0,0},{0,1,0}},n[3][3];double work[3][3];uint32_t tri[]={0,1,2};
assert(ayatsuri_smooth_normals(plane,3,tri,3,work,n)==AYATSURI_OK);
for(unsigned v=0;v<3;v++){assert(n[v][0]==0&&n[v][1]==0&&n[v][2]==1);}
tri[1]=2;tri[2]=1;assert(ayatsuri_smooth_normals(plane,3,tri,3,work,n)==AYATSURI_OK);assert(n[0][2]==-1);
plane[1][0]=FLT_MAX;plane[2][1]=FLT_MAX;assert(ayatsuri_smooth_normals(plane,3,tri,3,work,n)==AYATSURI_OK);assert(n[0][2]==-1);
plane[1][0]=0x1p-149f;plane[2][1]=0x1p-149f;assert(ayatsuri_smooth_normals(plane,3,tri,3,work,n)==AYATSURI_OK);assert(n[0][2]==-1);
assert(ayatsuri_smooth_normals(NULL,3,tri,3,work,n)==AYATSURI_INVALID);
assert(ayatsuri_smooth_normals(plane,SIZE_MAX,tri,3,work,n)==AYATSURI_INVALID);
}
printf("PASS smooth normal synthetic, invalid index/NaN/late degenerate/unreferenced/alias/count atomic\n");}
