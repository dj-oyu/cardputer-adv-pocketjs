#include "ui/kasane/ksn_mesh_q7_certified.h"
#include <math.h>
#include <limits.h>
#include <string.h>
static int64_t fld(int64_t n,int64_t d){int64_t q=n/d,r=n%d;return q-((r!=0)&&((r<0)!=(d<0)));}
static int64_t cld(int64_t n,int64_t d){return -fld(-n,d);}
bool baseline_ksn_mesh_q7_span(const ksn_mesh_q7_extra *t,int y,int *lo,int *hi){
 int64_t l=0,r=239;
 for(unsigned k=0;k<3;k++) {int64_t a=t->abc[k][0],v=(int64_t)t->abc[k][1]*y+t->abc[k][2];
  if(a>0){int64_t z=cld(-v,a);if(z>l)l=z;}else if(a<0){int64_t z=fld(-v,a);if(z<r)r=z;}else if(v<0)return false;
 }
 if(l>r)return false;
 *lo=(int)l;*hi=(int)r;return true;
}
size_t baseline_ksn_mesh_q7_bytes(const ksn_mesh_prepared *p){return !p||!p->valid||p->capacity>64||p->count>p->capacity||p->count>64?0:sizeof(ksn_mesh_q7_snapshot)+p->count*sizeof(ksn_mesh_q7_triangle);}
bool baseline_ksn_mesh_q7_prepare(const ksn_mesh_prepared *p,ksn_mesh_q7_snapshot *out,size_t bytes){
 if(!out||bytes<sizeof *out)return false;
 size_t need=baseline_ksn_mesh_q7_bytes(p);
 if(p){size_t src=sizeof *p;if(p->count<=64)src+=p->count*sizeof p->triangles[0];uintptr_t a=(uintptr_t)p,b=(uintptr_t)out;
  // Reject any source/output overlap before invalidating output/source.
  if(a>UINTPTR_MAX-src||b>UINTPTR_MAX-bytes|| (a<b+bytes&&b<a+src))return false;
 }
 out->valid=false;out->count=0;out->capacity=0;
 if(!need||bytes<need)return false;
 size_t capacity=(bytes-sizeof *out)/sizeof(ksn_mesh_q7_triangle);out->capacity=(unsigned)(capacity>64?64:capacity);
 out->count=p->count;out->fallback=false;memset(out->bands,0,sizeof out->bands);
 for(unsigned f=0;f<p->count;f++){
  ksn_mesh_q7_triangle *t=&out->triangles[f];t->original=p->triangles[f];const co_vertex *v=t->original.v;int64_t q[3][2];bool ok=true;
  memset(&t->fixed,0,sizeof t->fixed);
  for(unsigned k=0;k<3;k++)for(unsigned j=0;j<2;j++){double val=(j?v[k].y:v[k].x)*128.;if(!isfinite(val)||fabs(val)>524288)ok=false;q[k][j]=ok?(int64_t)floor(val+.5):0;}
  int64_t area=(q[1][0]-q[0][0])*(q[2][1]-q[0][1])-(q[1][1]-q[0][1])*(q[2][0]-q[0][0]);if(area<=0)ok=false;
  for(unsigned k=0;k<3;k++){
   unsigned u=(k+1)%3,w=(k+2)%3;int64_t dx=q[w][0]-q[u][0],dy=q[w][1]-q[u][1];int64_t a=-dy*128,b=dx*128,c=dy*q[u][0]-dx*q[u][1]-!(dy<0||(dy==0&&dx>0));
   if(a<INT32_MIN||a>INT32_MAX||b<INT32_MIN||b>INT32_MAX||c<INT32_MIN||c>INT32_MAX)ok=false;
   for(int x=0;x<=243;x+=243)for(int y=0;y<=134;y+=134){int64_t z=a*x+b*y+c;if(z<INT32_MIN||z>INT32_MAX)ok=false;}
   if(ok){t->fixed.abc[k][0]=(int32_t)a;t->fixed.abc[k][1]=(int32_t)b;t->fixed.abc[k][2]=(int32_t)c;}
  }
  for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++){
   double *plane=t->fixed.rgb[pal][ch];unsigned shift=ch==0?11:ch==1?5:0,mask=ch==1?63:31;
   for(unsigned k=0;k<3;k++){unsigned color=pal?v[k].color_b:v[k].color_a;double n=(color>>shift)&mask;const ksn_prepared_edge *e=&t->original.edge[k];plane[0]+=n*e->sign*e->a*t->original.inverse;plane[1]+=n*e->sign*e->b*t->original.inverse;plane[2]+=n*e->sign*e->c*t->original.inverse;}
   double dx=round(plane[0]*65536.);if(!isfinite(dx)||fabs(dx)>INT32_MAX/4)ok=false;else t->fixed.dx[pal][ch]=(int32_t)dx;
   for(int x=0;x<=239;x+=239)for(int y=0;y<=134;y+=134){double n=(plane[0]*x+plane[1]*y+plane[2])*65536.;if(!isfinite(n)||fabs(n)>INT32_MAX-16777216.)ok=false;}
  }
  if(!ok)out->fallback=true;
  int top=t->original.top-1,bottom=t->original.bottom+1;if(top<0)top=0;if(bottom>134)bottom=134;
  if(top<=bottom)for(unsigned b=(unsigned)top/8;b<=(unsigned)bottom/8;b++)out->bands[b]|=UINT64_C(1)<<f;
 }
 // A whole-frame fallback avoids mixing coverage conventions on shared edges.

 out->valid=true;return true;
}
static double edge(const ksn_prepared_edge *e,int x,int y){return e->sign*(e->a*x+e->b*y+e->c);}
static uint16_t original_color(const co_vertex*v,const double*w,bool second){
 uint16_t a=second?v[0].color_b:v[0].color_a,b=second?v[1].color_b:v[1].color_a,c=second?v[2].color_b:v[2].color_a;
 const float cw[3]={(float)w[0],(float)w[1],(float)w[2]};
 unsigned r=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a>>11)+cw[1]*(b>>11)+cw[2]*(c>>11)))),g=(unsigned)lroundf(fmaxf(0,fminf(63,cw[0]*((a>>5)&63)+cw[1]*((b>>5)&63)+cw[2]*((c>>5)&63)))),bl=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a&31)+cw[1]*(b&31)+cw[2]*(c&31))));return (uint16_t)((r<<11)|(g<<5)|bl);
}
bool baseline_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot*s,uint16_t*pixels,unsigned y,unsigned height,float*z,uint8_t*owner,unsigned rows,ksn_mesh_q7_stats*stats){
 if(!s||!s->valid||s->count>s->capacity||s->capacity>64||!pixels||!z||!owner||!stats||(rows!=1&&rows!=8)||y>135||height>135-y)return false;
 for(unsigned strip=0;strip<height;strip+=rows){unsigned n=height-strip;if(n>rows)n=rows;unsigned sy=y+strip;float *zz=z;uint8_t *oo=owner;uint16_t*pp=pixels+strip*240;
  for(unsigned i=0;i<n*240;i++){zz[i]=-INFINITY;oo[i]=255;}
  if(s->fallback)stats->frame_fallback++;
  for(unsigned f=0;f<s->count;f++){uint64_t bands=0;for(unsigned band=sy/8;band<=(sy+n-1)/8;band++)bands|=s->bands[band];if(!(bands&(UINT64_C(1)<<f)))continue;const ksn_mesh_q7_triangle*t=&s->triangles[f];const ksn_prepared_triangle*o=&t->original;const co_vertex*v=o->v;
   for(int yy=(int)sy;yy<(int)(sy+n);yy++){int lo,hi;bool original=s->fallback;
    if(original){if(yy<o->top||yy>o->bottom)continue;lo=o->left;hi=o->right;}else if(!baseline_ksn_mesh_q7_span(&t->fixed,yy,&lo,&hi))continue;
    stats->rows++;int64_t color[2][3];for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++){const double *pl=t->fixed.rgb[pal][ch];color[pal][ch]=original?0:llround((pl[0]*lo+pl[1]*yy+pl[2])*65536.);}
    for(int x=lo;x<=hi;x++){
     double w[3];bool inside=true;for(unsigned k=0;k<3;k++){w[k]=edge(&o->edge[k],x,yy);if(w[k]<0||(w[k]==0&&!o->edge[k].closed))inside=false;w[k]*=o->inverse;}
     if(original){stats->original_pixels++;if(!inside)continue;}else stats->span_pixels++;
     float d=(float)(w[0]*v[0].depth+w[1]*v[1].depth+w[2]*v[2].depth);if(!isfinite(d))return false;unsigned at=(unsigned)(yy-(int)sy)*240+(unsigned)x;
     if(d<=zz[at])stats->depth_reject++;else{zz[at]=d;oo[at]=(uint8_t)f;stats->mask_evals++;bool second=w[0]*v[0].mask+w[1]*v[1].mask+w[2]*v[2].mask>=0;
      if(original){pp[at]=original_color(v,w,second);stats->color_fallback++;}else{unsigned c[3];for(unsigned ch=0;ch<3;ch++){int64_t a=color[second][ch],max=ch==1?63:31;if(a<0)a=0;if(a>max*65536)a=max*65536;c[ch]=(unsigned)((a+32768)>>16);}pp[at]=(uint16_t)((c[0]<<11)|(c[1]<<5)|c[2]);}
     }
     if(!original)for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++)color[pal][ch]+=t->fixed.dx[pal][ch];
    }
   }
  }
 }
 return true;
}
