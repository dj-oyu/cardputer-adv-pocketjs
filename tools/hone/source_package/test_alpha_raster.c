#include <assert.h>
#include <time.h>
#include <stdio.h>
#include "alpha_raster_candidate.h"
int main(void){unsigned owner=1;hone_alpha_packet p[5];p[0]=(hone_alpha_packet){{{0,0,1,0},{4,0,1,4},{0,4,1,0}},{1,0,0},.5f,0,0,0,1,0};for(int i=1;i<5;i++)p[i]=p[0];
hone_alpha_snapshot s={p,1,5,&owner,1,0,1};uint16_t a[25]={0},b[25]={0},c[25]={0},old[25];float z[25];for(int i=0;i<25;i++)z[i]=-10;
assert(hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));assert(hone_alpha_render(&s,&owner,1,b,z,5,5,4,5));assert(hone_alpha_render(&s,&owner,1,c,z,5,5,8,5));assert(!memcmp(a,b,sizeof a)&&!memcmp(a,c,sizeof a));assert(a[6]==0x8000);
memcpy(old,a,sizeof a);s.count=5;assert(!hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));assert(!memcmp(a,old,sizeof a));s.count=1;z[6]=2;assert(hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));assert(a[6]==old[6]);
memcpy(old,a,sizeof a);z[24]=NAN;assert(!hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));assert(!memcmp(a,old,sizeof a));z[24]=-10;assert(hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));
s.count=0;assert(hone_alpha_render(&s,&owner,1,NULL,NULL,0,0,0,0));s.count=1;assert(!hone_alpha_render(&s,&owner,1,a,(float*)a,5,5,1,5));
clock_t begin=clock();for(int i=0;i<10000;i++)assert(hone_alpha_render(&s,&owner,1,a,z,5,5,1,5));printf("host synthetic 1 triangle 5x5 preflight+replay %.1f ns/frame\n",1e9*(clock()-begin)/CLOCKS_PER_SEC/10000.);return 0;}
