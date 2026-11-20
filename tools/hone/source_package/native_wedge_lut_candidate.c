/* Experimental continuous candidate: measured synthetic Blender wedge response. */
static const float response[65]={1.0f,0.997890262f,0.995804583f,0.993742383f,0.991703695f,0.989687679f,0.987694534f,0.985723598f,0.98377485f,0.981847669f,0.979941994f,0.978057386f,0.976193535f,0.974350087f,0.972526836f,0.970723469f,0.968939641f,0.967174839f,0.965429022f,0.963701666f,0.961992745f,0.960301615f,0.958627997f,0.956972059f,0.955332792f,0.953710271f,0.952104041f,0.950514007f,0.948939662f,0.947380625f,0.945836768f,0.944307656f,0.942792934f,0.941292551f,0.939806f,0.938332768f,0.936872832f,0.93542574f,0.933991313f,0.932569152f,0.931158857f,0.929760261f,0.928372821f,0.926996581f,0.925630957f,0.924275605f,0.922930348f,0.921594866f,0.920268809f,0.918951847f,0.917643606f,0.916343848f,0.9150523f,0.913768654f,0.912492456f,0.911223477f,0.909961356f,0.908705847f,0.907456744f,0.906213469f,0.90497575f,0.903743379f,0.902516128f,0.901293536f,0.900070944f};
/* Experimental host-only polygon normal candidate, NOT runtime admitted.
 * Triangle/quad polygons, no sharp/custom normals. Independent from Blender. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
static inline double measured_angle_weight(double d){double q=fmin(1.,fabs(d)),u=q*64.;int idx=(int)u;if(idx>63)idx=63;double f=u-idx;double a=sqrt(1.-q)*(response[idx]*(1.-f)+response[idx+1]*f);double angle=d<0?2.-a:a;return angle;}
static double normalize(double a[3]){double l=sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);if(l>1e-30){for(int j=0;j<3;j++)a[j]/=l;}return l;}
int polygon_normals(const float *p,size_t nv,const uint32_t *offset,const uint32_t *vi,const uint8_t *smooth,size_t np,size_t nc,double *work,float *out){
 if(!p||!offset||!vi||!smooth||!work||!out||offset[0]||offset[np]!=nc)return 0;
 for(size_t i=0;i<3*nv;i++){if(!isfinite(p[i]))return 0;work[i]=0;}
 for(size_t f=0;f<np;f++){
  size_t a=offset[f],b=offset[f+1];if(b<a||b-a<3||b-a>4)return 0;
  for(size_t k=a;k<b;k++)if(vi[k]>=nv)return 0;
 }
 for(size_t f=0;f<np;f++){
  size_t a=offset[f],b=offset[f+1];double n[3]={0,0,0};
  /* Binary32 last-to-first cyclic ordering matches the reference's cached
   * polygon path, which differs numerically from its quad inspection path. */
  float nf[3]={0,0,0};
  for(size_t k=a;k<b;k++){const float *v=p+3*vi[k==a?b-1:k-1],*w=p+3*vi[k];nf[0]+=(v[1]-w[1])*(v[2]+w[2]);nf[1]+=(v[2]-w[2])*(v[0]+w[0]);nf[2]+=(v[0]-w[0])*(v[1]+w[1]);}
  float length=sqrtf(nf[0]*nf[0]+nf[1]*nf[1]+nf[2]*nf[2]);
  if(length==0){nf[0]=nf[1]=0;nf[2]=1;}else{float inverse=1.0f/length;for(int j=0;j<3;j++)nf[j]*=inverse;}
  for(int j=0;j<3;j++)n[j]=nf[j];
  for(size_t k=a;k<b;k++){
   for(int j=0;j<3;j++)out[3*k+j]=(float)n[j];
   if(smooth[f]){const float *v=p+3*vi[k],*prev=p+3*vi[k==a?b-1:k-1],*next=p+3*vi[k+1==b?a:k+1];double x[3],y[3];for(int j=0;j<3;j++){x[j]=prev[j]-v[j];y[j]=next[j]-v[j];}normalize(x);normalize(y);double d=x[0]*y[0]+x[1]*y[1]+x[2]*y[2];double angle=measured_angle_weight(d);for(int j=0;j<3;j++)work[3*vi[k]+j]+=angle*n[j];}
  }
 }
 for(size_t i=0;i<nv;i++)if(normalize(work+3*i)==0){work[3*i]=work[3*i+1]=0;work[3*i+2]=0;}
 for(size_t f=0;f<np;f++)if(smooth[f])for(size_t k=offset[f];k<offset[f+1];k++){if(work[3*vi[k]]==0 && work[3*vi[k]+1]==0 && work[3*vi[k]+2]==0)return 0;for(int j=0;j<3;j++)out[3*k+j]=(float)work[3*vi[k]+j];}
 return 1;
}
