/* Frozen bbee585a source, differential-test symbols renamed. */
#include "ksn_mesh_q7_certified.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include <float.h>
#include <fenv.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define Q7_SPAN_HOT IRAM_ATTR
#else
#define Q7_SPAN_HOT
#endif
#if defined(__FAST_MATH__) || FLT_RADIX != 2 || DBL_MANT_DIG != 53 || FLT_MANT_DIG != 24 || FLT_EVAL_METHOD != 0 || FLT_MAX_EXP != 128 || FLT_MIN_EXP != -125 || DBL_MAX_EXP != 1024 || DBL_MIN_EXP != -1021
#error Certified path requires strict IEEE binary32/binary64 arithmetic
#endif
_Static_assert(sizeof(float)==4 && sizeof(double)==8,"IEEE storage required");
#ifndef Q7_CERT_MODE
#define Q7_CERT_MODE 3
#endif
static int64_t fld(int64_t n,int64_t d){int64_t q=n/d,r=n%d;return q-((r!=0)&&((r<0)!=(d<0)));}
static int64_t cld(int64_t n,int64_t d){return -fld(-n,d);}
bool old_q7_span(const ksn_mesh_q7_extra *t,int y,int *lo,int *hi){
 int64_t l=0,r=239;
 for(unsigned k=0;k<3;k++) {int64_t a=t->abc[k][0],v=(int64_t)t->abc[k][1]*y+t->abc[k][2];
  if(a>0){int64_t z=cld(-v,a);if(z>l)l=z;}else if(a<0){int64_t z=fld(-v,a);if(z<r)r=z;}else if(v<0)return false;
 }
 if(l>r)return false;
 *lo=(int)l;*hi=(int)r;return true;
}
static bool ieee_environment(void){
 if(fegetround()!=FE_TONEAREST)return false;
 volatile double normal=DBL_MIN,half=.5;volatile float tiny=FLT_TRUE_MIN;
 double sub=normal*half,converted=tiny;
 return sub>0 && sub==0x1p-1023 && converted==0x1p-149;
}
static double upadd(double a,double b){return nextafter(a+b,INFINITY);}
static double upmul(double a,double b){return nextafter(a*b,INFINITY);}
static void make_certificate(ksn_mesh_q7_triangle *t){
 ksn_mesh_q7_certificate *c=&t->cert;memset(c,0,sizeof *c);if(!ieee_environment())return;const ksn_prepared_triangle *o=&t->original;
 if(!(o->inverse>0&&o->inverse<=100001))return;
 for(unsigned k=0;k<3;k++){const ksn_prepared_edge*e=&o->edge[k];if((e->sign!=1&&e->sign!=-1)||!isfinite(e->a)||!isfinite(e->b)||!isfinite(e->c)||fabs(e->a)>8192||fabs(e->b)>8192||fabs(e->c)>67108864)return;}
 for(unsigned attr=0;attr<2;attr++){double magnitude=0;
  for(unsigned k=0;k<3;k++){double value=attr?o->v[k].mask:o->v[k].depth;if(!isfinite(value)||fabs(value)>FLT_MAX)return;const ksn_prepared_edge*e=&o->edge[k];double coeff[3]={e->a,e->b,e->c};
   for(unsigned j=0;j<3;j++)c->p[attr][j]+=e->sign*coeff[j]*o->inverse*value;
   double edgeabs=upadd(upadd(upmul(fabs(e->a),239),upmul(fabs(e->b),134)),fabs(e->c));
   magnitude=upadd(magnitude,upmul(upmul(edgeabs,o->inverse),fabs(value)));
  }
  c->error[attr]=upadd(upmul(128*DBL_EPSILON,magnitude),1e-250);
  if(!isfinite(c->error[attr]))return;
  for(unsigned j=0;j<3;j++)if(!isfinite(c->p[attr][j]))return;
 }
 c->valid=true;
}
static bool certified_depth(const ksn_mesh_q7_certificate*c,int x,double row_product,float *out){
 double v=c->p[0][0]*x+row_product+c->p[0][2],err=c->error[0];
 float lo=(float)(v-err),hi=(float)(v+err);
 /* C union representation reads retain signed-zero equality without the two
  * out-of-line memcpy calls emitted by the target's no-builtin build. */
 union {float f;uint32_t u;} lo_bits={.f=lo},hi_bits={.f=hi};
 if(lo_bits.u!=hi_bits.u||!isfinite(lo)||(lo!=0&&fabsf(lo)<FLT_MIN))return false;
 *out=lo;return true;
}
static bool certified_mask(const ksn_mesh_q7_certificate*c,int x,double row_product,bool *out){
 double v=c->p[1][0]*x+row_product+c->p[1][2],err=c->error[1];
 if(v-err>=0){*out=true;return true;}if(v+err<0){*out=false;return true;}return false;
}
size_t old_q7_bytes(const ksn_mesh_prepared *p){return !p||!p->valid||p->capacity>64||p->count>p->capacity||p->count>64?0:sizeof(ksn_mesh_q7_snapshot)+p->count*sizeof(ksn_mesh_q7_triangle);}
bool old_q7_prepare(const ksn_mesh_prepared *p,ksn_mesh_q7_snapshot *out,size_t bytes){
 if(!out||bytes<sizeof *out)return false;
 size_t need=old_q7_bytes(p);
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
  make_certificate(t);
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
static uint16_t fixed_color(const int64_t rgb[3]){
 unsigned c[3];for(unsigned ch=0;ch<3;ch++){int64_t a=rgb[ch],max=ch==1?63:31;if(a<0)a=0;if(a>max*65536)a=max*65536;c[ch]=(unsigned)((a+32768)>>16);}
 return (uint16_t)((c[0]<<11)|(c[1]<<5)|c[2]);
}
/* A row covers at most 240 pixels. Accumulate exactly in 32 bits, then
 * merge into the public wrapping uint64 counters once per row. This aims to reduce
 * 64-bit loads, carries and stores in the S3 pixel loop. */
typedef struct {uint32_t depth_fast,depth_exact,original_pixels,span_pixels,depth_reject,mask_evals,mask_fast,mask_exact,color_fallback;} q7_row_stats;
static void row_stats_commit(ksn_mesh_q7_stats *out,const q7_row_stats *r){
 out->depth_fast+=r->depth_fast;
 out->depth_exact+=r->depth_exact;
 out->original_pixels+=r->original_pixels;
 out->span_pixels+=r->span_pixels;
 out->depth_reject+=r->depth_reject;
 out->mask_evals+=r->mask_evals;
 out->mask_fast+=r->mask_fast;
 out->mask_exact+=r->mask_exact;
 out->color_fallback+=r->color_fallback;
}
/* Continue a row only after its x-invariant mask has been certified on an
 * actual depth winner. Keep the frequent flat span separate from generic
 * interpolation storage. A failed depth certificate returns the first
 * untouched x to the original loop; no pixel or counter is lost. */
typedef struct {
 const ksn_mesh_q7_certificate *cert;
 float *z;uint8_t *owner;uint16_t *pixels;
 double depth_y;uint16_t color;uint8_t face;
} q7_flat_span;
/* Keep the small target loop in internal instruction RAM; generic fallback
 * and its larger interpolation state remain outside this hot function. */
__attribute__((noinline))
static Q7_SPAN_HOT int flat_span_run(const q7_flat_span *span,int x,int hi,q7_row_stats *stats){
 int start=x;unsigned rejected=0;
 for(;x<=hi;x++){
  float depth;
  if(!certified_depth(span->cert,x,span->depth_y,&depth))break;
  if(depth<=span->z[x])rejected++;
  else{span->z[x]=depth;span->owner[x]=span->face;span->pixels[x]=span->color;}
 }
 unsigned count=(unsigned)(x-start),won=count-rejected;
 stats->depth_fast+=count;stats->span_pixels+=count;stats->depth_reject+=rejected;
 stats->mask_evals+=won;stats->mask_fast+=won;
 return x;
}
bool old_q7_render(const ksn_mesh_q7_snapshot*s,uint16_t*pixels,unsigned y,unsigned height,float*z,uint8_t*owner,unsigned rows,ksn_mesh_q7_stats*stats){
 if(!s||!s->valid||s->count>s->capacity||s->capacity>64||!pixels||!z||!owner||!stats||(rows!=1&&rows!=8)||y>135||height>135-y)return false;
 bool rounding=ieee_environment();
 for(unsigned strip=0;strip<height;strip+=rows){unsigned n=height-strip;if(n>rows)n=rows;unsigned sy=y+strip;float *zz=z;uint8_t *oo=owner;uint16_t*pp=pixels+strip*240;
  for(unsigned i=0;i<n*240;i++){zz[i]=-INFINITY;oo[i]=255;}
  if(s->fallback)stats->frame_fallback++;
  for(unsigned f=0;f<s->count;f++){uint64_t bands=0;for(unsigned band=sy/8;band<=(sy+n-1)/8;band++)bands|=s->bands[band];if(!(bands&(UINT64_C(1)<<f)))continue;const ksn_mesh_q7_triangle*t=&s->triangles[f];const ksn_prepared_triangle*o=&t->original;const co_vertex*v=o->v;
   for(int yy=(int)sy;yy<(int)(sy+n);yy++){int lo,hi;bool original=s->fallback;
    if(original){if(yy<o->top||yy>o->bottom)continue;lo=o->left;hi=o->right;}else if(!old_q7_span(&t->fixed,yy,&lo,&hi))continue;
    stats->rows++;double row_x=lo,row_y=yy;int64_t color[2][3];for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++){const double *pl=t->fixed.rgb[pal][ch];color[pal][ch]=original?0:llround((pl[0]*row_x+pl[1]*row_y+pl[2])*65536.);}
    /* Hoist only the y product, not its addition with the constant: retaining
     * (p0*x + row_product) + p2 preserves the certificate's rounding order. */
    bool fast=!original&&rounding&&t->cert.valid;
    double depth_y=((Q7_CERT_MODE&1)&&fast)?t->cert.p[0][1]*row_y:0,mask_y=0;
    bool mask_row_ready=false,mask_cached=false,cached_second=false;
    bool flat=!original;
    if(flat)for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++)if(t->fixed.dx[pal][ch])flat=false;
    uint16_t flat_rgb[2]={0,0};if(flat){flat_rgb[0]=fixed_color(color[0]);flat_rgb[1]=fixed_color(color[1]);}
    q7_row_stats row_stats={0};
    for(int x=lo;x<=hi;x++){
     double w[3]={0};bool have_weights=false,inside=true;float d=0;
     if((Q7_CERT_MODE&1)&&fast&&certified_depth(&t->cert,x,depth_y,&d)){row_stats.depth_fast++;}else{
      row_stats.depth_exact++;for(unsigned k=0;k<3;k++){w[k]=edge(&o->edge[k],x,yy);if(w[k]<0||(w[k]==0&&!o->edge[k].closed))inside=false;w[k]*=o->inverse;}have_weights=true;
      if(original){row_stats.original_pixels++;if(!inside)continue;}
      d=(float)(w[0]*v[0].depth+w[1]*v[1].depth+w[2]*v[2].depth);
     }
     if(!original)row_stats.span_pixels++;
     if(!isfinite(d)){row_stats_commit(stats,&row_stats);return false;}
     unsigned at=(unsigned)(yy-(int)sy)*240+(unsigned)x;
     if(d<=zz[at])row_stats.depth_reject++;else{zz[at]=d;oo[at]=(uint8_t)f;row_stats.mask_evals++;bool second;
      /* Delay mask work until a depth winner, as before. Cache only a
       * certified x-constant result; an uncertain sign keeps exact fallback. */
      if((Q7_CERT_MODE&2)&&fast&&!mask_row_ready){mask_y=t->cert.p[1][1]*row_y;mask_row_ready=true;}
      if(mask_cached){second=cached_second;row_stats.mask_fast++;}
      else if((Q7_CERT_MODE&2)&&fast&&certified_mask(&t->cert,x,mask_y,&second)){
       row_stats.mask_fast++;if(t->cert.p[1][0]==0){cached_second=second;mask_cached=true;}
      }
      else{row_stats.mask_exact++;if(!have_weights)for(unsigned k=0;k<3;k++)w[k]=edge(&o->edge[k],x,yy)*o->inverse;second=w[0]*v[0].mask+w[1]*v[1].mask+w[2]*v[2].mask>=0;}
      if(original){pp[at]=original_color(v,w,second);row_stats.color_fallback++;}else pp[at]=flat?flat_rgb[second]:fixed_color(color[second]);
     }
     if(flat&&mask_cached&&(Q7_CERT_MODE&1)&&fast&&x<hi){
      unsigned row=(unsigned)(yy-(int)sy)*240;
      q7_flat_span span={&t->cert,zz+row,oo+row,pp+row,depth_y,flat_rgb[cached_second],(uint8_t)f};
      x=flat_span_run(&span,x+1,hi,&row_stats)-1;
     }
     if(!original&&!flat)for(unsigned pal=0;pal<2;pal++)for(unsigned ch=0;ch<3;ch++)color[pal][ch]+=t->fixed.dx[pal][ch];
    }
    row_stats_commit(stats,&row_stats);
   }
  }
 }
 return true;
}
