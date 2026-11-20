#ifndef AYATSURI_SMOOTH_NORMALS_H
#define AYATSURI_SMOOTH_NORMALS_H
#include "ayatsuri_skin.h"
/* Explicit smooth-triangle-only stage. Not an importer or runtime default.
 * Caller must preserve original smoothing connectivity (split sharp seams).
 * No custom normals supported. All spans valid/aligned/immutable during call.
 * Workspace may change on failure; published normals never do. All workspace
 * and output spans must be pairwise disjoint from inputs and each other.
 * No heap, callbacks or variable stack. Unreferenced vertices reject. */
static inline ayatsuri_status ayatsuri_smooth_normals(
 const float (*positions)[3],size_t vertices,const uint32_t *indices,size_t corners,
 double (*workspace)[3],float (*normals)[3]){
 if(vertices>1000000||corners>3000000||corners%3||
 !ayatsuri_skin_disjoint(workspace,vertices,sizeof *workspace,positions,vertices,sizeof *positions)||
 !ayatsuri_skin_disjoint(workspace,vertices,sizeof *workspace,indices,corners,sizeof *indices)||
 !ayatsuri_skin_disjoint(normals,vertices,sizeof *normals,positions,vertices,sizeof *positions)||
 !ayatsuri_skin_disjoint(normals,vertices,sizeof *normals,indices,corners,sizeof *indices)||
 !ayatsuri_skin_disjoint(normals,vertices,sizeof *normals,workspace,vertices,sizeof *workspace))return AYATSURI_INVALID;
 for(size_t i=0;i<vertices;i++)for(unsigned k=0;k<3;k++)if(!isfinite(positions[i][k]))return AYATSURI_INVALID;
 for(size_t i=0;i<corners;i++)if(indices[i]>=vertices)return AYATSURI_INVALID;
 memset(workspace,0,vertices*sizeof *workspace);
 for(size_t t=0;t<corners;t+=3)for(unsigned j=0;j<3;j++){
  const float *p=positions[indices[t+j]],*q=positions[indices[t+(j+1)%3]],*r=positions[indices[t+(j+2)%3]];
  double a[3],b[3],n[3];for(unsigned k=0;k<3;k++){a[k]=(double)q[k]-p[k];b[k]=(double)r[k]-p[k];}
  n[0]=a[1]*b[2]-a[2]*b[1];n[1]=a[2]*b[0]-a[0]*b[2];n[2]=a[0]*b[1]-a[1]*b[0];
  double len=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(!(len>0)||!isfinite(len))return AYATSURI_RANGE;
  double angle=atan2(len,a[0]*b[0]+a[1]*b[1]+a[2]*b[2]);
  for(unsigned k=0;k<3;k++)workspace[indices[t+j]][k]+=n[k]/len*angle;
 }
 for(size_t i=0;i<vertices;i++){
  double *a=workspace[i],len=sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);if(!(len>0)||!isfinite(len))return AYATSURI_RANGE;
  for(unsigned k=0;k<3;k++)a[k]/=len;
 }
 for(size_t i=0;i<vertices;i++)for(unsigned k=0;k<3;k++)normals[i][k]=(float)workspace[i][k];
 return AYATSURI_OK;
}
#endif
