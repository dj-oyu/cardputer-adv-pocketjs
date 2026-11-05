/* ksn_proc_render_band_carry (docs/platform/lcd-dma-driver.md 9.9): an 8-row
 * band drawn as two 4-row pieces with the carry gives exactly the pixels of
 * ksn_proc_render_band_range, piece by piece -- for frames with and without
 * pattern lines, sub-ranges [first, end), another frame drawn between the two
 * pieces, a carry dropped by its owner, a second piece asked for twice (the
 * second time with no carry), and pieces that start anywhere. */
#include "ksn_shiki_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ksn_proc_frame f,g;
static ksn_proc_carry carry;
static uint16_t a[KSN_PROC_W*8],b[KSN_PROC_W*8];
static uint32_t st=777;static int rnd(int n){st=st*1103515245u+12345u;return (int)((st>>8)%(unsigned)n);}
static int16_t coord(int lim){int k=rnd(10);if(k<6)return (int16_t)(rnd(lim+40)-20);if(k<8)return (int16_t)(rnd(2000)-1000);return (int16_t)(rnd(65536)-32768);}
static void make(ksn_proc_frame*fr){
 memset(fr,0,sizeof *fr);fr->ready=true;fr->ext=(uint8_t)(rnd(2));unsigned n=0,want=1+rnd(200);
 while(n<want&&n<KSN_PROC_SEGMENTS-1){ksn_proc_segment*s=&fr->segments[n];
  s->x0=(int16_t)(rnd(280)-20);s->x1=coord(240);s->y0=coord(135);s->y1=coord(135);if(rnd(4)==0)s->y1=s->y0;s->color=(uint16_t)rnd(65536);
  if(fr->ext&&rnd(3)==0){ s->x0=(int16_t)(rnd(1200)-480+KSN_PROC_EXT_PATTERN); ksn_proc_segment*q=&fr->segments[n+1];
   unsigned period=1+rnd(KSN_PROC_PATTERN_BITS);
   q->x0=(int16_t)rnd(65536);q->y0=(int16_t)(0x8000|rnd(256)|period<<8|(rnd(2)<<13));q->x1=(int16_t)rnd(period*256);
   q->y1=(int16_t)(0x8000|rnd(period*256));q->color=(uint16_t)rnd(65536);n+=2;}
  else n++;}
 fr->count=(uint16_t)n;}
static int check(const ksn_proc_frame*fr,int y,int h,unsigned first,unsigned end,const char*what){
 memset(a,0x5a,sizeof a);memset(b,0x5a,sizeof b);
 ksn_proc_render_band_range(fr,a,y,h,first,end);ksn_proc_render_band_carry(fr,b,y,h,first,end,&carry);
 if(memcmp(a,b,sizeof a)){printf("MISMATCH %s y=%d h=%d\n",what,y,h);return 1;}return 0;}
int main(void){
 long n=0;
 for(int it=0;it<6000;it++){
  make(&f);make(&g);
  unsigned first=0,end=f.count;
  if(rnd(2)){first=(unsigned)rnd(f.count+1);end=first+(unsigned)rnd(f.count-first+1);
   if(f.ext&&first>0&&f.segments[first-1].x0>=KSN_PROC_EXT_MIN)first--; /* first must start an entry */
   if(end<first)end=first;}
  for(int y=0;y<KSN_PROC_H;y+=8){
   int rest=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
   int mode=rnd(5);
   if(check(&f,y,4<rest?4:rest,first,end,"p1"))return 1;
   if(mode==0&&check(&g,y,4,0,g.count,"other"))return 1;      /* another frame in between */
   if(mode==1){f.segments[0].color^=1u;carry.valid=false;}    /* owner drops it on a change */
   if(rest>4&&check(&f,y+4,rest-4,first,end,"p2"))return 1;
   if(mode==2&&rest>4&&check(&f,y+4,rest-4,first,end,"p2-again"))return 1; /* stale: no carry */
   n+=2;}
  for(int y=0;y<KSN_PROC_H;y+=1+rnd(9)){int h=1+rnd(8);if(y+h>KSN_PROC_H)h=KSN_PROC_H-y;if(check(&f,y,h,first,end,"any"))return 1;n++;}
 }
 printf("procedural carry: %ld calls equal\n",n);return 0;}
