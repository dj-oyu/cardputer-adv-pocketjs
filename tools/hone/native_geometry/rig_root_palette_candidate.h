#ifndef HONE_RIG_ROOT_PALETTE_CANDIDATE_H
#define HONE_RIG_ROOT_PALETTE_CANDIDATE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "root palette requires finite checks"
#endif
/* Caller admits root bones only, no constraints or bone scaling, and immutable
 * same-frame rest/pose values in armature space. Inputs and scratch/ready are
 * complete, disjoint spans. wxyz quaternions; Euler is XYZ, radians. Success
 * computes rest * pose * inverse(rest), represented as unit quaternion plus
 * translation. Failure may change scratch but never changes ready. */
enum { HONE_BONE_EULER_XYZ=0, HONE_BONE_QUATERNION=1 };
typedef struct {double rest_q[4],rest_t[3],location[3],rotation[4];unsigned mode;} hone_root_bone;
static inline void hone_quat_mul(const double a[4],const double b[4],double r[4]){
 r[0]=a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3];
 r[1]=a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2];
 r[2]=a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1];
 r[3]=a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0];
}
static inline int hone_quat_unit(const double input[4],double output[4]){
 double norm=0;for(unsigned i=0;i<4;i++){if(!isfinite(input[i]))return 0;norm+=input[i]*input[i];}
 if(!isfinite(norm)||fabs(norm-1)>1e-5||norm<=0)return 0;
 norm=sqrt(norm);for(unsigned i=0;i<4;i++)output[i]=input[i]/norm;return 1;
}
static inline void hone_quat_rotate(const double q[4],const double p[3],double out[3]){
 double v[4]={0,p[0],p[1],p[2]},conj[4]={q[0],-q[1],-q[2],-q[3]},a[4],b[4];
 hone_quat_mul(q,v,a);hone_quat_mul(a,conj,b);for(unsigned i=0;i<3;i++)out[i]=b[i+1];
}
static inline int hone_root_bone_palette(const hone_root_bone *bone,double output[7]){
 if(!bone||!output||bone->mode>HONE_BONE_QUATERNION)return 0;
 for(unsigned i=0;i<3;i++)if(!isfinite(bone->rest_t[i])||!isfinite(bone->location[i]))return 0;
 double rest[4],pose[4],p[4],result[7],conj[4],loc[3],center[3];
 if(!hone_quat_unit(bone->rest_q,rest))return 0;
 if(bone->mode==HONE_BONE_QUATERNION){if(!hone_quat_unit(bone->rotation,pose))return 0;}
 else{
  for(unsigned i=0;i<3;i++)if(!isfinite(bone->rotation[i]))return 0;
  const double x=bone->rotation[0]/2,y=bone->rotation[1]/2,z=bone->rotation[2]/2,
   cx=cos(x),sx=sin(x),cy=cos(y),sy=sin(y),cz=cos(z),sz=sin(z);
  pose[0]=cz*cy*cx+sz*sy*sx;pose[1]=cz*cy*sx-sz*sy*cx;
  pose[2]=cz*sy*cx+sz*cy*sx;pose[3]=sz*cy*cx-cz*sy*sx;
 }
 for(unsigned i=0;i<4;i++)conj[i]=i?-rest[i]:rest[i];
 hone_quat_mul(rest,pose,p);hone_quat_mul(p,conj,result);
 hone_quat_rotate(rest,bone->location,loc);hone_quat_rotate(result,bone->rest_t,center);
 for(unsigned i=0;i<3;i++)result[4+i]=bone->rest_t[i]+loc[i]-center[i];
 for(unsigned i=0;i<7;i++)if(!isfinite(result[i]))return 0;
 memcpy(output,result,sizeof result);return 1;
}
static inline int hone_root_palette(const hone_root_bone *bones,size_t count,double *scratch,double *ready){
 if(!bones||!scratch||!ready||!count||count>65536)return 0;
 for(size_t i=0;i<count;i++)if(!hone_root_bone_palette(&bones[i],scratch+7*i))return 0;
 memcpy(ready,scratch,count*7*sizeof(*ready));return 1;
}
typedef struct {uint16_t program,bone;uint8_t axis;} hone_root_driver_route;
typedef struct {uint32_t source_mesh;const uint32_t *groups;size_t group_count;double to_rig[12],from_rig[12];} hone_root_mesh_binding;
/* Routes apply only to admitted XYZ root rotations. Bone and palette scratch
 * are private, disjoint spans; ready changes only after every bone succeeds. */
static inline int hone_root_driver_palette(const hone_root_bone *base,size_t bone_count,
 const hone_root_driver_route *routes,size_t route_count,const double *values,size_t value_count,
 hone_root_bone *bone_scratch,double *palette_scratch,double *ready){
 if(!base||!bone_scratch||!palette_scratch||!ready||!bone_count||bone_count>65536||route_count>4096||
    (route_count&&(!routes||!values)))return 0;
 for(size_t i=0;i<route_count;i++){
  hone_root_driver_route r=routes[i];
  if(r.bone>=bone_count||r.program>=value_count||r.axis>=3||base[r.bone].mode!=HONE_BONE_EULER_XYZ||!isfinite(values[r.program]))return 0;
  for(size_t j=0;j<i;j++)if(routes[j].bone==r.bone&&routes[j].axis==r.axis)return 0;
 }
 memcpy(bone_scratch,base,bone_count*sizeof(*base));
 for(size_t i=0;i<route_count;i++)bone_scratch[routes[i].bone].rotation[routes[i].axis]=values[routes[i].program];
 return hone_root_palette(bone_scratch,bone_count,palette_scratch,ready);
}
#endif
