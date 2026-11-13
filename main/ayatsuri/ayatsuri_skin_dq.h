#ifndef AYATSURI_SKIN_DQ_H
#define AYATSURI_SKIN_DQ_H
#include "ayatsuri_skin.h"

/* Explicit experimental RIGID dual-quaternion path; existing LBS unchanged.
 * Caller must declare rigid source transforms. Matrix admission tolerates only
 * 2e-6 orthogonality/determinant residual for binary32 storage. This tolerance
 * is not authorization to discard authored scale/shear. Converter must reject
 * any authored nonunit scale, reflection or shear before this API.
 * Palette matrices are already mesh-local global * inverse_bind.
 * No Blender scale-correction matrices, B-bones, envelopes or morph evaluation.
 * Positive influences must share a pairwise-consistent quaternion hemisphere;
 * pairwise aligned dot <=1e-12 rejects as ambiguous. No arbitrary wide-angle blend.
 * Output is transactional, allocation-free, synchronous caller-owned storage.
 * Normals rotate by the blended quaternion, NOT deformed-surface recomputation.
 */
typedef enum { AYATSURI_SKIN_METHOD_LBS=0, AYATSURI_SKIN_METHOD_RIGID_DQ=1 } ayatsuri_skin_method;
static inline void ayatsuri_dq_mul(const double a[4],const double b[4],double r[4]){
    r[0]=a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3];
    r[1]=a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2];
    r[2]=a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1];
    r[3]=a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0];
}
static inline int ayatsuri_dq_from_rigid(const ayatsuri_transform *t,double q[4],double d[4]){
    const float *m=t->m;
    for(unsigned k=0;k<12;k++)if(!isfinite(m[k]))return 0;
    for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++){
        double dot=0;for(unsigned r=0;r<3;r++)dot+=(double)m[4*r+a]*m[4*r+b];
        if(fabs(dot-(a==b?1:0))>2e-6)return 0;
    }
    double det=(double)m[0]*(m[5]*(double)m[10]-m[6]*(double)m[9])-
        (double)m[1]*(m[4]*(double)m[10]-m[6]*(double)m[8])+
        (double)m[2]*(m[4]*(double)m[9]-m[5]*(double)m[8]);
    if(fabs(det-1)>2e-6)return 0;
    double tr=(double)m[0]+m[5]+m[10],s;
    if(tr>0){s=2*sqrt(tr+1);q[0]=s/4;q[1]=(m[9]-(double)m[6])/s;q[2]=(m[2]-(double)m[8])/s;q[3]=(m[4]-(double)m[1])/s;}
    else if(m[0]>m[5]&&m[0]>m[10]){s=2*sqrt(1+(double)m[0]-m[5]-m[10]);q[0]=(m[9]-(double)m[6])/s;q[1]=s/4;q[2]=(m[1]+(double)m[4])/s;q[3]=(m[2]+(double)m[8])/s;}
    else if(m[5]>m[10]){s=2*sqrt(1+(double)m[5]-m[0]-m[10]);q[0]=(m[2]-(double)m[8])/s;q[1]=(m[1]+(double)m[4])/s;q[2]=s/4;q[3]=(m[6]+(double)m[9])/s;}
    else{s=2*sqrt(1+(double)m[10]-m[0]-m[5]);q[0]=(m[4]-(double)m[1])/s;q[1]=(m[2]+(double)m[8])/s;q[2]=(m[6]+(double)m[9])/s;q[3]=s/4;}
    double n=hypot(hypot(q[0],q[1]),hypot(q[2],q[3]));
    if(!(n>0)||!isfinite(n))return 0;
    for(unsigned k=0;k<4;k++)q[k]/=n;
    double translation[4]={0,m[3],m[7],m[11]};ayatsuri_dq_mul(translation,q,d);
    for(unsigned k=0;k<4;k++)d[k]*=.5;
    return 1;
}
static inline void ayatsuri_dq_rotate(const double q[4],const float v[3],double out[3]){
    double x[4]={0,v[0],v[1],v[2]},qc[4]={q[0],-q[1],-q[2],-q[3]},a[4],b[4];
    ayatsuri_dq_mul(q,x,a);ayatsuri_dq_mul(a,qc,b);
    for(unsigned k=0;k<3;k++)out[k]=b[k+1];
}
static inline ayatsuri_status ayatsuri_skin_evaluate_rigid_dq(
        const ayatsuri_transform *palette,size_t joints,
        const ayatsuri_skin_vertex *vertices,size_t count,ayatsuri_skin_result *out){
    uintptr_t begin,end;
    if(joints>65536||!ayatsuri_skin_span(palette,joints,sizeof *palette,&begin,&end)||
       !ayatsuri_skin_span(vertices,count,sizeof *vertices,&begin,&end)||
       !ayatsuri_skin_disjoint(out,count,sizeof *out,palette,joints,sizeof *palette)||
       !ayatsuri_skin_disjoint(out,count,sizeof *out,vertices,count,sizeof *vertices))return AYATSURI_INVALID;
    for(size_t j=0;j<joints;j++){double q[4],d[4];if(!ayatsuri_dq_from_rigid(palette+j,q,d))return AYATSURI_INVALID;}
    for(unsigned pass=0;pass<2;pass++)for(size_t v=0;v<count;v++){
        const ayatsuri_skin_vertex *in=vertices+v;unsigned sum=0;
        for(unsigned k=0;k<4;k++){if(in->joints[k]>=joints)return AYATSURI_INVALID;sum+=in->weights[k];}
        if(sum!=65535)return AYATSURI_INVALID;
        for(unsigned k=0;k<3;k++)if(!isfinite(in->position[k])||!isfinite(in->normal[k]))return AYATSURI_INVALID;
        double q[4]={0},d[4]={0},ref[4]={0},aligned[4][4];unsigned used=0;int have=0;
        for(unsigned k=0;k<4;k++)if(in->weights[k]){
            double a[4],b[4],dot=0;ayatsuri_dq_from_rigid(palette+in->joints[k],a,b);
            if(!have){memcpy(ref,a,sizeof ref);have=1;}
            for(unsigned c=0;c<4;c++)dot+=ref[c]*a[c];
            double sign=dot<0?-1:1;
            for(unsigned previous=0;previous<used;previous++){
                double pairdot=0;for(unsigned c=0;c<4;c++)pairdot+=aligned[previous][c]*sign*a[c];
                /* Reject ambiguous wide-rotation blends, not a hidden sign policy. */
                if(pairdot<=1e-12)return AYATSURI_RANGE;
            }
            for(unsigned c=0;c<4;c++)aligned[used][c]=sign*a[c];
            used++;
            double w=sign*(double)in->weights[k]/65535.0;
            for(unsigned c=0;c<4;c++){q[c]+=w*a[c];d[c]+=w*b[c];}
        }
        double n=hypot(hypot(q[0],q[1]),hypot(q[2],q[3]));
        if(!(n>0)||!isfinite(n))return AYATSURI_RANGE;
        for(unsigned k=0;k<4;k++){q[k]/=n;d[k]/=n;}
        double qc[4]={q[0],-q[1],-q[2],-q[3]},translation[4],position[3],normal[3];
        ayatsuri_dq_mul(d,qc,translation);ayatsuri_dq_rotate(q,in->position,position);ayatsuri_dq_rotate(q,in->normal,normal);
        double length=hypot(hypot(normal[0],normal[1]),normal[2]);if(!(length>0)||!isfinite(length))return AYATSURI_INVALID;
        ayatsuri_skin_result candidate;
        for(unsigned k=0;k<3;k++)if(!ayatsuri_skin_narrow(position[k]+2*translation[k+1],&candidate.position[k])||
            !ayatsuri_skin_narrow(normal[k]/length,&candidate.normal[k]))return AYATSURI_RANGE;
        if(pass)memcpy(out+v,&candidate,sizeof candidate);
    }
    return AYATSURI_OK;
}
static inline ayatsuri_status ayatsuri_skin_evaluate_method(ayatsuri_skin_method method,
        const ayatsuri_transform *palette,size_t joints,const ayatsuri_skin_vertex *vertices,
        size_t count,ayatsuri_skin_result *out){
    if(method==AYATSURI_SKIN_METHOD_LBS)return ayatsuri_skin_evaluate(palette,joints,vertices,count,out);
    if(method==AYATSURI_SKIN_METHOD_RIGID_DQ)return ayatsuri_skin_evaluate_rigid_dq(palette,joints,vertices,count,out);
    return AYATSURI_INVALID;
}
#endif
