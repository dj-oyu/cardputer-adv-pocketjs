#define _POSIX_C_SOURCE 200809L
#include "ui/kasane/ksn_mesh_q7_certified.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <time.h>
extern uint8_t *test_owner;
extern bool baseline_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot*,uint16_t*,unsigned,unsigned,float*,uint8_t*,unsigned,ksn_mesh_q7_stats*);
static unsigned seed=72;static unsigned rnd(void){return seed=seed*1664525+1013904223;}
static void append(ksn_mesh_snapshot*s,float x,float y,float w,float h,unsigned mode){
 co_vertex v[3]={{x,y,-1,0,63488,31},{x+w,y,1,0,2016,65535},{x,y+h,0,0,31,0}};
 for(unsigned k=0;k<3;k++)v[k].depth=mode==1?1:mode==2?nextafterf(1,2):mode==3?(float)((int)(rnd()%200)-100)/13:0;
 uint16_t ix[3]={0,1,2};co_input in={v,ix,0,3,1};assert(co_append(&s->packets,&in));
}
static uint64_t samples,coverage,owners,depths,colors,color_gt1,shared_colors,shared_gt1;static unsigned maxcolor;static ksn_mesh_q7_stats stats;
static void compare(ksn_mesh_snapshot*s,ksn_mesh_prepared*p,ksn_mesh_q7_snapshot*q){
 uint16_t a[1920],b[1920],ref[1920];float za[1920],zb[1920],zr[1920];uint8_t oa[1920],ob[1920],oc[1920];uint16_t c[1920];float zc[1920];ksn_mesh_q7_stats baseline_stats={0};
 for(unsigned rows=1;rows<=8;rows*=8)for(unsigned y=0;y<135;y+=rows){unsigned n=rows;if(n>135-y)n=135-y;for(unsigned i=0;i<1920;i++)a[i]=b[i]=c[i]=ref[i]=0x1234;memset(oa,255,sizeof oa);test_owner=oa;
  assert(ksn_mesh_prepared_render(p,a,y,n,za,rows));test_owner=NULL;assert(ksn_mesh_render(s,ref,y,n,zr,rows));assert(!memcmp(a,ref,n*240*2));assert(!memcmp(za,zr,n*240*4));assert(ksn_mesh_q7_render(q,b,y,n,zb,ob,rows,&stats));assert(baseline_ksn_mesh_q7_render(q,c,y,n,zc,oc,rows,&baseline_stats));assert(!memcmp(b,c,n*240*2)&&!memcmp(zb,zc,n*240*4)&&!memcmp(ob,oc,n*240));
  for(unsigned i=0;i<n*240;i++){samples++;coverage+=(oa[i]==255)!=(ob[i]==255);owners+=oa[i]!=ob[i];depths+=memcmp(za+i,zb+i,4)!=0;colors+=a[i]!=b[i];
   unsigned delta=0;for(unsigned ch=0;ch<3;ch++){int shift=ch==0?11:ch==1?5:0,mask=ch==1?63:31;unsigned d=abs(((a[i]>>shift)&mask)-((b[i]>>shift)&mask));if(d>delta)delta=d;}
   if(delta>maxcolor)maxcolor=delta;
   color_gt1+=delta>1;
   if(oa[i]==ob[i]){shared_colors+=a[i]!=b[i];shared_gt1+=delta>1;assert(!memcmp(za+i,zb+i,4));}
   if(q->fallback){assert(a[i]==b[i]);assert(oa[i]==ob[i]);assert(!memcmp(za+i,zb+i,4));}
  }
 }
}
static void arbitrary(ksn_mesh_snapshot*s){
 co_vertex v[3];for(unsigned k=0;k<3;k++)v[k]=(co_vertex){((int)(rnd()%60000)-20000)/128.f+.001f,((int)(rnd()%40000)-10000)/128.f+.002f,(float)((int)k-1),(float)k,63488,2016};
 double ar=((double)v[1].x-v[0].x)*((double)v[2].y-v[0].y)-((double)v[1].y-v[0].y)*((double)v[2].x-v[0].x);if(ar<0){co_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;}if(fabs(ar)<1e-5)return;
 uint16_t ix[3]={0,1,2};co_input in={v,ix,0,3,1};assert(co_append(&s->packets,&in));
}
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){uint8_t data[65536];ksn_mesh_snapshot s={.packets={data,sizeof data,0,0},.enabled=true};size_t bytes=sizeof(ksn_mesh_prepared)+64*sizeof(ksn_prepared_triangle);ksn_mesh_prepared*p=malloc(bytes);size_t qbytes=sizeof(ksn_mesh_q7_snapshot)+64*sizeof(ksn_mesh_q7_triangle);ksn_mesh_q7_snapshot*q=malloc(qbytes);assert(p&&q);unsigned fallback=0;
 for(unsigned scene=0;scene<160;scene++){s.packets.size=s.packets.packets=0;
  for(unsigned i=0;i<1+scene%12;i++){float x=(int)(rnd()%280)-20+.12345f,y=(int)(rnd()%160)-15+.10001f,w=1+rnd()%120,h=1+rnd()%90;
   if(scene%8==0){x=0;y=0;w=239;h=134;}if(scene%8==1){x=-4096;y=-4096;w=4200;h=4200;}if(scene%8==2){w=.0001f;h=90;}if(scene%8==3){x=20.0f;y=10.0f;w=80;h=80;}
   append(&s,x,y,w,h,scene%4);
  }
  assert(ksn_mesh_prepare(&s,p,bytes));assert(ksn_mesh_q7_prepare(p,q,qbytes));fallback+=q->fallback;compare(&s,p,q);
 }
 // Shared diagonal and adjacent-F32 depths in both submission orders.
 for(unsigned mode=0;mode<4;mode++){s.packets.size=s.packets.packets=0;append(&s,20,20,80,80,mode%2?2:1);append(&s,20,20,80,80,mode%2?1:2);assert(ksn_mesh_prepare(&s,p,bytes)&&ksn_mesh_q7_prepare(p,q,qbytes));compare(&s,p,q);}
 s.packets.size=s.packets.packets=0;append(&s,20,20,80,80,1);co_vertex diagonal[3]={{100,100,0,1,65535,31},{20,100,0,1,65535,31},{100,20,0,1,65535,31}};uint16_t dix[3]={0,1,2};co_input din={diagonal,dix,0,3,1};assert(co_append(&s.packets,&din));assert(ksn_mesh_prepare(&s,p,bytes)&&ksn_mesh_q7_prepare(p,q,qbytes));compare(&s,p,q);
 printf("DIFF scenes=165 rows1and8 samples=%llu fallback_frames=%u coverage=%llu owner=%llu depth=%llu RGB=%llu RGB_gt1=%llu maxchannel=%u same_owner_RGB=%llu same_owner_gt1=%llu\n",(unsigned long long)samples,fallback,(unsigned long long)coverage,(unsigned long long)owners,(unsigned long long)depths,(unsigned long long)colors,(unsigned long long)color_gt1,maxcolor,(unsigned long long)shared_colors,(unsigned long long)shared_gt1);
 printf("MEMORY retained_header=%zu retained_triangle=%zu ksn_mesh_q7_triangle=%zu ksn_mesh_q7_snapshot=%zu stripe8_color_z_owner=13440\n",sizeof *p,sizeof p->triangles[0],sizeof(ksn_mesh_q7_triangle),qbytes);
 assert(shared_gt1==0);
 // Brute-force halfspaces independently check all row spans, including negative divisions.
 for(unsigned scene=0;scene<1000;scene++){s.packets.size=s.packets.packets=0;arbitrary(&s);assert(ksn_mesh_prepare(&s,p,bytes));assert(ksn_mesh_q7_prepare(p,q,qbytes));if(q->fallback||!q->count)continue;
  if(scene<100)compare(&s,p,q);
  for(int y=0;y<135;y++){int lo=0,hi=-1;bool yes=ksn_mesh_q7_span(&q->triangles[0].fixed,y,&lo,&hi);for(int x=0;x<240;x++){bool inside=true;for(unsigned k=0;k<3;k++){int32_t*a=q->triangles[0].fixed.abc[k];if((int64_t)a[0]*x+(int64_t)a[1]*y+a[2]<0)inside=false;}assert(inside==(yes&&x>=lo&&x<=hi));}}
 }
 puts("SPAN_PASS scenes=1000 general_non_axis_triangles all_135x240_pixel_halfspace_compare_on_admitted plus100_exact_oracle_comparisons");
 uint16_t pixels[1920];float z[1920];uint8_t owner[1920];s.packets.size=s.packets.packets=0;for(unsigned i=0;i<8;i++)append(&s,(i%4)*40+.1f,10+(i/4)*45+.1f,60,65,3);
 for(unsigned rows=1;rows<=8;rows*=8){double times[3][9];for(unsigned rep=0;rep<9;rep++)for(unsigned order=0;order<3;order++){unsigned mode=(order+rep)%3;double start=now();for(unsigned frame=0;frame<100;frame++){if(mode)assert(ksn_mesh_prepare(&s,p,bytes));if(mode==2)assert(ksn_mesh_q7_prepare(p,q,qbytes));for(unsigned y=0;y<135;y+=rows){unsigned n=rows;if(n>135-y)n=135-y;memset(pixels,0,sizeof pixels);if(mode==0)assert(ksn_mesh_render(&s,pixels,y,n,z,rows));else if(mode==1)assert(ksn_mesh_prepared_render(p,pixels,y,n,z,rows));else assert(ksn_mesh_q7_render(q,pixels,y,n,z,owner,rows,&stats));}}times[mode][rep]=(now()-start)*1e6/100;}
 for(unsigned mode=0;mode<3;mode++){qsort(times[mode],9,sizeof(double),cmp);printf("BENCH rows=%u mode=%u us_median=%.3f min=%.3f max=%.3f full_prepare_render_host_only\n",rows,mode,times[mode][4],times[mode][0],times[mode][8]);}}
 // Empty and invalid rendering never dereference caller output.
 s.packets.size=s.packets.packets=0;assert(ksn_mesh_prepare(&s,p,bytes)&&ksn_mesh_q7_prepare(p,q,qbytes));uint16_t sentinel=0xbeef;float zv=77;uint8_t own=99;
 assert(ksn_mesh_q7_render(q,&sentinel,135,0,&zv,&own,1,&stats));assert(sentinel==0xbeef&&zv==77&&own==99);assert(!ksn_mesh_q7_render(q,&sentinel,136,0,&zv,&own,1,&stats));assert(!ksn_mesh_q7_render(q,&sentinel,135,1,&zv,&own,1,&stats));
 printf("FINAL_DIFF samples=%llu coverage=%llu owners=%llu depth=%llu RGB=%llu same_owner_RGB=%llu same_owner_gt1=%llu\n",(unsigned long long)samples,(unsigned long long)coverage,(unsigned long long)owners,(unsigned long long)depths,(unsigned long long)colors,(unsigned long long)shared_colors,(unsigned long long)shared_gt1);assert(shared_gt1==0);

 free(p);free(q);return 0;
}
