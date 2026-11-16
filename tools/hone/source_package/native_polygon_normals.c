/* Experimental host-only polygon normal candidate, NOT runtime admitted.
 * Triangle/quad polygons, no sharp/custom normals. Independent from Blender. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
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
  /* Newell polygon normal preserves one face contribution across its diagonal. */
  for(size_t k=a;k<b;k++){const float *v=p+3*vi[k],*w=p+3*vi[k+1==b?a:k+1];n[0]+=((double)v[1]-w[1])*((double)v[2]+w[2]);n[1]+=((double)v[2]-w[2])*((double)v[0]+w[0]);n[2]+=((double)v[0]-w[0])*((double)v[1]+w[1]);}
  if(normalize(n)==0){n[0]=n[1]=0;n[2]=1;}
  for(size_t k=a;k<b;k++){
   for(int j=0;j<3;j++)out[3*k+j]=(float)n[j];
   if(smooth[f]){const float *v=p+3*vi[k],*prev=p+3*vi[k==a?b-1:k-1],*next=p+3*vi[k+1==b?a:k+1];double x[3],y[3];for(int j=0;j<3;j++){x[j]=prev[j]-v[j];y[j]=next[j]-v[j];}normalize(x);normalize(y);double d=x[0]*y[0]+x[1]*y[1]+x[2]*y[2];double angle=acos(fmax(-1.,fmin(1.,d)));for(int j=0;j<3;j++)work[3*vi[k]+j]+=angle*n[j];}
  }
 }
 for(size_t i=0;i<nv;i++)if(normalize(work+3*i)==0){work[3*i]=work[3*i+1]=0;work[3*i+2]=1;}
 for(size_t f=0;f<np;f++)if(smooth[f])for(size_t k=offset[f];k<offset[f+1];k++)for(int j=0;j<3;j++)out[3*k+j]=(float)work[3*vi[k]+j];
 return 1;
}
