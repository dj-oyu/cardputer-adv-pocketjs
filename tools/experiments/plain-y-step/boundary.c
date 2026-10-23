#include "ksn_procedural.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern bool plain_step_candidate;
enum { GUARD=16, N=KSN_PROC_W*KSN_PROC_H+2*GUARD };
static uint16_t expected[N],actual[N];
/* Walk from the original endpoint without band_entry or coarse clipping. */
static void oracle(const ksn_proc_segment *s,int y,int h){
 int x=s->x0,yy=s->y0,dx=abs(s->x1-x),dy=abs(s->y1-yy);
 int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
 for(;;){
  if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+h)expected[GUARD+(yy-y)*KSN_PROC_W+x]=s->color;
  if(x==s->x1&&yy==s->y1)break;
  int twice=2*err;
  if(twice>-dy){err-=dy;x+=sx;}
  if(twice<dx){err+=dx;yy+=sy;}
 }
}
static unsigned checks;
static void check(ksn_proc_frame *f,int y,int h){
 for(unsigned i=0;i<N;i++)expected[i]=0x39e7;
 for(unsigned i=0;i<f->count;i++)oracle(&f->segments[i],y,h);
 for(int mode=0;mode<2;mode++){
  for(unsigned i=0;i<N;i++)actual[i]=0x39e7;
  plain_step_candidate=mode;
  assert(ksn_proc_render_band(f,actual+GUARD,y,h));
  if(memcmp(actual,expected,sizeof actual)){
   fprintf(stderr,"pixel mismatch mode=%d ext=%u y=%d h=%d line=%d,%d -> %d,%d\n",mode,f->ext,y,h,f->segments[0].x0,f->segments[0].y0,f->segments[0].x1,f->segments[0].y1);exit(1);
  }
  checks++;
 }
}
int main(int argc,char **argv){
 bool positive=argc>1&&!strcmp(argv[1],"--positive");
 static ksn_proc_frame f;
 const int xs[]={-480,-1,0,239,240,720},ys[]={-480,-1,0,7,8,134,135,720};
 const int bands[]={0,1,7,8,64,127,134,135};
 f.ready=true;f.ext=1;f.count=1;
 for(unsigned a=0;a<48;a++)for(unsigned b=0;b<48;b++){
  f.segments[0]=(ksn_proc_segment){xs[a%6],ys[a/6],xs[b%6],ys[b/6],0xffff};
  for(unsigned k=0;k<sizeof bands/sizeof *bands;k++){
   int y=bands[k],hs[]={0,1,8,KSN_PROC_H-y};
   for(unsigned j=0;j<4;j++){
    if(hs[j]>KSN_PROC_H-y||(positive&&!hs[j]))continue;
    bool duplicate=false;for(unsigned q=0;q<j;q++)if(hs[q]==hs[j])duplicate=true;
    if(!duplicate)check(&f,y,hs[j]);
   }
  }
 }
 /* Overdraw and the unchanged non-ext path use the same full-walk oracle. */
 f.count=3;f.segments[0]=(ksn_proc_segment){-1,-1,240,135,1};
 f.segments[1]=(ksn_proc_segment){240,-1,-1,135,2};
 f.segments[2]=(ksn_proc_segment){0,67,239,67,3};
 for(int ext=0;ext<2;ext++){f.ext=ext;for(int y=0;y<=(positive?134:135);y++)check(&f,y,135-y);}
 printf("PLAIN_BOUNDARY PASS checks=%u pointer_bytes=%zu\n",checks,sizeof(void*));
}
