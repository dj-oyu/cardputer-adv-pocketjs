#include "derby_runner_affine.h"

#include <math.h>
#include <stdlib.h>

/* Nine unique body/jockey vertices, in sixteenths of the old u=2.5*s
 * coordinate unit. The first eight form one PIE block; point 8 is a scalar
 * tail. Different horses never share a matrix. Legs remain screen inputs. */
static const _Alignas(16) int16_t local_x[16]={-16,0,48,64,80,64,32,32,48};
static const _Alignas(16) int16_t local_y[16]={16,0,0,-32,-16,-16,0,-16,-32};

static bool screen(float v){return isfinite(v)&&v>=-480.0f&&v<=720.0f;}

bool derby_runner_affine(const float in[8],const float colors[3],
                         const KsnProcPointsPolicy *policy,
                         ksn_proc_frame *out,KsnProcPointsDecision *decision)
{
    if(!in||!colors||!out)return false;
    for(unsigned i=0;i<8;i++)if(!isfinite(in[i]))return false;
    for(unsigned i=0;i<3;i++)
        if(!isfinite(colors[i])||colors[i]<0||colors[i]>65535||
           colors[i]!=(float)(uint16_t)colors[i])return false;
    if(!screen(in[0])||!screen(in[1])||in[2]<0||in[2]>8)return false;
    for(unsigned i=3;i<8;i++)if(!screen(in[i]))return false;
    const float u=in[2]*2.5f;
    /* Conservative pre-round geometry guard, with room for float operation
     * rounding and the <.003 px Q14 coefficient/translation error. This
     * prevents saturation and preserves the VM's pre-round range errors by
     * falling back before changing any output. */
    if(in[0]-u < -479.99f||in[0]+5*u > 719.99f||
       in[1]-2*u < -479.99f||in[1]+u > 719.99f)return false;
    const int16_t scale=(int16_t)lroundf(in[2]*2560.0f);
    const KsnProcAffineQ14 c={scale,0,0,scale,
        (int32_t)lroundf(in[0]*16384.0f)+8192,
        (int32_t)lroundf(in[1]*16384.0f)+8192};
    _Alignas(16) int16_t x[16],y[16];
    /* Kernel: wide sum -> floor(Q14) -> sat16. The +8192 translation bias
     * explicitly chooses half-up screen rounding, unlike VM lroundf's
     * negative half ties. The visual contract permits <=1 px endpoint error.
     * Max |sum| < 2^24 here (signed 25 bits); scalar uses int64, PIE QACC40. */
    KsnProcPointsDecision chosen=ksn_proc_points_affine_dispatch(
        (KsnProcPointDst){x,y},(KsnProcPointSrc){local_x,local_y},9,&c,policy);
    for(unsigned i=0;i<9;i++)
        if(x[i]<-480||x[i]>720||y[i]<-480||y[i]>720)return false;
    int16_t feet[4];
    for(unsigned i=0;i<4;i++)feet[i]=(int16_t)lroundf(in[4+i]);
    const int16_t hoof=(int16_t)lroundf(in[3]);
    const uint16_t coat=(uint16_t)colors[0],silk=(uint16_t)colors[1],cap=(uint16_t)colors[2];
    const unsigned a[9]={0,1,2,3,4,6,7,8,8},b[9]={1,2,3,4,5,7,8,5,8};
    ksn_proc_segment seg[13];
    for(unsigned i=0;i<5;i++)seg[i]=(ksn_proc_segment){x[a[i]],y[a[i]],x[b[i]],y[b[i]],coat};
    for(unsigned i=0;i<4;i++){
        const unsigned hip=i<2?1:2;
        seg[5+i]=(ksn_proc_segment){x[hip],y[hip],feet[i],hoof,coat};
    }
    for(unsigned i=5;i<9;i++)seg[4+i]=(ksn_proc_segment){x[a[i]],y[a[i]],x[b[i]],y[b[i]],i==8?cap:silk};
    unsigned raster=0;
    for(unsigned i=0;i<13;i++){
        const unsigned dx=(unsigned)abs(seg[i].x1-seg[i].x0),dy=(unsigned)abs(seg[i].y1-seg[i].y0);
        raster+=(dx>dy?dx:dy)+1;
    }
    if(raster>KSN_PROC_RASTER_STEPS)return false;
    out->count=13;out->raster_steps=(uint16_t)raster;out->ready=true;out->ext=0;
    for(unsigned i=0;i<13;i++)out->segments[i]=seg[i];
    if(decision)*decision=chosen;
    return true;
}
