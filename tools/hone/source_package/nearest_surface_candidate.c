/* Independent closest-point host candidate. Brute force, no BVH, no runtime
 * admission. Degenerate triangles retain their closest nonzero edge/vertex. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
static double dot3(const double*a,const double*b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static void score(const double*p,const double*a,const double*b,const double*c,const double*w,double *best,double *bestw){
 double d[3];for(unsigned k=0;k<3;k++)d[k]=w[0]*a[k]+w[1]*b[k]+w[2]*c[k]-p[k];double s=dot3(d,d);
 if(s<*best){*best=s;for(unsigned k=0;k<3;k++)bestw[k]=w[k];}
}
static double nearest_triangle(const double*p,const double*a,const double*b,const double*c,double*w){
 double ab[3],ac[3],ap[3];for(unsigned k=0;k<3;k++){ab[k]=b[k]-a[k];ac[k]=c[k]-a[k];ap[k]=p[k]-a[k];}
 double aa=dot3(ab,ab),bb=dot3(ab,ac),cc=dot3(ac,ac),u=dot3(ap,ab),v=dot3(ap,ac),det=aa*cc-bb*bb,best=INFINITY;
 if(det>0){double y=(u*cc-v*bb)/det,z=(v*aa-u*bb)/det;double q[3]={1-y-z,y,z};if(q[0]>=0&&y>=0&&z>=0)score(p,a,b,c,q,&best,w);}
 const double*vs[3]={a,b,c};
 for(unsigned i=0;i<3;i++){
  unsigned j=(i+1)%3;double d[3],r[3];for(unsigned k=0;k<3;k++){d[k]=vs[j][k]-vs[i][k];r[k]=p[k]-vs[i][k];}
  double l=dot3(d,d),t=l>0?dot3(d,r)/l:0;if(t<0)t=0;if(t>1)t=1;
  double q[3]={0};q[i]=1-t;q[j]=t;score(p,a,b,c,q,&best,w);
 }
 return best;
}
int nearest_surface(const double*p,const double*vertices,size_t nv,const uint32_t*triangles,size_t nt,double*out,double*bary,uint32_t*face){
 if(!p||!vertices||!triangles||!out||!bary||!face||!nv||!nt||nt>UINT32_MAX)return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(p[k]))return 0;
 double best=INFINITY,winner[3]={0};uint32_t which=0;
 for(size_t f=0;f<nt;f++){
  const uint32_t*t=triangles+3*f;for(unsigned k=0;k<3;k++){if(t[k]>=nv)return 0;for(unsigned j=0;j<3;j++)if(!isfinite(vertices[3*t[k]+j]))return 0;}
  double w[3];double d=nearest_triangle(p,vertices+3*t[0],vertices+3*t[1],vertices+3*t[2],w);
  if(d<best){best=d;which=(uint32_t)f;for(unsigned k=0;k<3;k++)winner[k]=w[k];}
 }
 if(!isfinite(best))return 0;
 double q[3]={0};for(unsigned k=0;k<3;k++)for(unsigned j=0;j<3;j++)q[k]+=winner[j]*vertices[3*triangles[3*which+j]+k];
 for(unsigned k=0;k<3;k++){if(!isfinite(q[k]))return 0;}
 for(unsigned k=0;k<3;k++){out[k]=q[k];bary[k]=winner[k];}*face=which;return 1;
}
/* ON_SURFACE subset: offset toward the original query side, not target normal.
 * Coincident points remain coincident. Other wrap modes are not admitted. */
int nearest_surface_offset(const double*p,const double*vertices,size_t nv,const uint32_t*triangles,size_t nt,double offset,double*out){
 if(!out||!isfinite(offset))return 0;
 double q[3],b[3];uint32_t face;
 if(!nearest_surface(p,vertices,nv,triangles,nt,q,b,&face))return 0;
 double d[3];for(unsigned k=0;k<3;k++)d[k]=p[k]-q[k];double length=sqrt(dot3(d,d));
 if(!isfinite(length))return 0;
 if(length>0)for(unsigned k=0;k<3;k++)q[k]+=offset*d[k]/length;
 for(unsigned k=0;k<3;k++)if(!isfinite(q[k]))return 0;
 for(unsigned k=0;k<3;k++)out[k]=q[k];
 return 1;
}
