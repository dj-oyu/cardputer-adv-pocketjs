#ifndef AYATSURI_FOLLOW_H
#define AYATSURI_FOLLOW_H

#include "ayatsuri_transform.h"
#include <stdint.h>

/* Two generalized DOFs, fixed h=1/60 second. Each pair uses one common
 * coordinate unit U: q/goal in U and v in U/s. Rotational rigs use radians;
 * visual translation rigs may use model distance units. I is positive,
 * k has I/s^2 and c has I/s; force has I*U/s^2 and impulse J has I*U/s.
 * Coupling acts on the difference of goal-relative deviations, not absolute
 * coordinates. M q'' = -K(q-goal)-C q' + force, with symmetric mutual K/C.
 * Prepare once: B=inv(M+hC+h*hK)M, F=inv(A)hK, G=inv(A)h.
 * Warm step: v'=Bv-F(q-goal)+Gforce, q'=q+h*v' (unrounded v').
 * Raw config/state/goal/force/impulse are binary32; prepared matrices and all
 * intermediates are binary64. Same IEEE/RNE/no-fast-math/no-contraction and
 * no excess-precision requirements as ayatsuri_transform.h. No clock,
 * allocation, hidden state, callback or retained pointer. All inputs are
 * synchronously borrowed; caller prevents concurrent writes. Prepared
 * coefficients are caller-owned and immutable between prepare and step.
 * Every failure leaves output/state unchanged; in-place state is supported.
 * Preparation supports only kappa-infinity(A) <= 1e6. This conservative
 * numerical domain leaves kappa*epsilon64 <= 2.23e-10, well below binary32
 * unit roundoff (about 5.96e-8), with headroom for the coefficient arithmetic.
 * It is not a proof of energy monotonicity for every finite state or forcing.
 * Outside that domain preparation returns RANGE without publishing output.
 * Retarget changes only goal: neither q nor v resets. J is generalized impulse,
 * applied as v += J/I. There is no automatic settling or clamping of state. */
#define AYATSURI_FOLLOW_DT (1.0 / 60.0)
#define AYATSURI_FOLLOW_MAX_CONDITION 1000000.0

typedef struct {
    float inertia[2], stiffness[2], damping[2];
    float coupling_stiffness, coupling_damping;
} ayatsuri_follow_config;
typedef struct {
    double velocity[4], restoring[4], force[4], inertia[2];
} ayatsuri_follow_coefficients;
typedef ayatsuri_follow_coefficients ayatsuri_follow_coeff;
typedef struct { float q[2], v[2]; } ayatsuri_follow_state;
typedef struct { float q[2]; } ayatsuri_follow_goal;

#if defined(__cplusplus)
static_assert(sizeof(ayatsuri_follow_config)==32 && sizeof(ayatsuri_follow_coefficients)==112 &&
              sizeof(ayatsuri_follow_state)==16 && sizeof(ayatsuri_follow_goal)==8,"follow storage layout");
#else
_Static_assert(sizeof(ayatsuri_follow_config)==32 && sizeof(ayatsuri_follow_coefficients)==112 &&
               sizeof(ayatsuri_follow_state)==16 && sizeof(ayatsuri_follow_goal)==8,"follow storage layout");
#endif

static inline ayatsuri_status ayatsuri_follow_narrow(double value,float *out){
    const double magnitude=fabs(value),midpoint=(double)FLT_MAX+0x1p103;
    if(!isfinite(value)||magnitude>=midpoint)return AYATSURI_RANGE;
    *out=magnitude>(double)FLT_MAX?(value<0?-FLT_MAX:FLT_MAX):(float)value;
    return AYATSURI_OK;
}
static inline int ayatsuri_follow_coefficients_valid(const ayatsuri_follow_coefficients *c){
    if(!c)return 0;
    for(unsigned i=0;i<4;++i)
        if(!isfinite(c->velocity[i])||!isfinite(c->restoring[i])||!isfinite(c->force[i]))return 0;
    return isfinite(c->inertia[0])&&isfinite(c->inertia[1])&&c->inertia[0]>0&&c->inertia[1]>0;
}
static inline int ayatsuri_follow_state_valid(const ayatsuri_follow_state *s){
    return s&&isfinite(s->q[0])&&isfinite(s->q[1])&&isfinite(s->v[0])&&isfinite(s->v[1]);
}
static inline ayatsuri_status ayatsuri_follow_prepare(
        const ayatsuri_follow_config *c,ayatsuri_follow_coefficients *out){
    if(!c||!out)return AYATSURI_INVALID;
    for(unsigned i=0;i<2;++i)
        if(!isfinite(c->inertia[i])||c->inertia[i]<=0||!isfinite(c->stiffness[i])||c->stiffness[i]<0||
           !isfinite(c->damping[i])||c->damping[i]<0)return AYATSURI_INVALID;
    if(!isfinite(c->coupling_stiffness)||c->coupling_stiffness<0||
       !isfinite(c->coupling_damping)||c->coupling_damping<0)return AYATSURI_INVALID;
    const double h=AYATSURI_FOLLOW_DT,i0=c->inertia[0],i1=c->inertia[1],
        kc=c->coupling_stiffness,cc=c->coupling_damping;
    /* A=[[d0+u,-u],[-u,d1+u]]. All determinant terms below are
     * nonnegative, so the condition check does not subtract nearly equal
     * products. ||A||inf=||adj(A)||inf=max(d0,d1)+2u. Keep the existing
     * coefficient expression order after this gate for accepted inputs. */
    const double d0=i0+h*c->damping[0]+h*h*c->stiffness[0],
        d1=i1+h*c->damping[1]+h*h*c->stiffness[1],u=h*cc+h*h*kc,
        stable_determinant=d0*d1+u*(d0+d1),norm=fmax(d0,d1)+2*u,
        condition=norm*norm/stable_determinant;
    if(!isfinite(stable_determinant)||stable_determinant<=0||!isfinite(condition)||
       condition>AYATSURI_FOLLOW_MAX_CONDITION)return AYATSURI_RANGE;
    const double k00=(double)c->stiffness[0]+kc,k01=-kc,k11=(double)c->stiffness[1]+kc,
        a00=i0+h*((double)c->damping[0]+cc)+h*h*k00,
        a01=-h*cc-h*h*kc,a11=i1+h*((double)c->damping[1]+cc)+h*h*k11,
        determinant=a00*a11-a01*a01;
    if(!isfinite(determinant)||determinant<=0)return AYATSURI_RANGE;
    const double inv00=a11/determinant,inv01=-a01/determinant,inv11=a00/determinant;
    ayatsuri_follow_coefficients result;
    result.velocity[0]=inv00*i0;result.velocity[1]=inv01*i1;
    result.velocity[2]=inv01*i0;result.velocity[3]=inv11*i1;
    result.restoring[0]=h*(inv00*k00+inv01*k01);result.restoring[1]=h*(inv00*k01+inv01*k11);
    result.restoring[2]=h*(inv01*k00+inv11*k01);result.restoring[3]=h*(inv01*k01+inv11*k11);
    result.force[0]=h*inv00;result.force[1]=h*inv01;result.force[2]=h*inv01;result.force[3]=h*inv11;
    result.inertia[0]=i0;result.inertia[1]=i1;
    if(!ayatsuri_follow_coefficients_valid(&result))return AYATSURI_RANGE;
    memcpy(out,&result,sizeof result);return AYATSURI_OK;
}
static inline ayatsuri_status ayatsuri_follow_step(
        const ayatsuri_follow_coefficients *c,const ayatsuri_follow_goal *goal,
        const float force[2],ayatsuri_follow_state *state){
    if(!ayatsuri_follow_coefficients_valid(c)||!ayatsuri_follow_state_valid(state)||!goal||
       !isfinite(goal->q[0])||!isfinite(goal->q[1])||
       (force&&(!isfinite(force[0])||!isfinite(force[1]))))return AYATSURI_INVALID;
    const double e0=(double)state->q[0]-goal->q[0],e1=(double)state->q[1]-goal->q[1],
        f0=force?force[0]:0,f1=force?force[1]:0,
        v0=c->velocity[0]*state->v[0]+c->velocity[1]*state->v[1]-c->restoring[0]*e0-c->restoring[1]*e1+c->force[0]*f0+c->force[1]*f1,
        v1=c->velocity[2]*state->v[0]+c->velocity[3]*state->v[1]-c->restoring[2]*e0-c->restoring[3]*e1+c->force[2]*f0+c->force[3]*f1,
        q0=state->q[0]+AYATSURI_FOLLOW_DT*v0,q1=state->q[1]+AYATSURI_FOLLOW_DT*v1;
    ayatsuri_follow_state next;
    if(ayatsuri_follow_narrow(q0,&next.q[0])||ayatsuri_follow_narrow(q1,&next.q[1])||
       ayatsuri_follow_narrow(v0,&next.v[0])||ayatsuri_follow_narrow(v1,&next.v[1]))return AYATSURI_RANGE;
    memcpy(state,&next,sizeof next);return AYATSURI_OK;
}
static inline ayatsuri_status ayatsuri_follow_retarget(const float target[2],ayatsuri_follow_goal *goal){
    if(!target||!goal||!isfinite(target[0])||!isfinite(target[1]))return AYATSURI_INVALID;
    const ayatsuri_follow_goal next={{target[0],target[1]}};
    memcpy(goal,&next,sizeof next);return AYATSURI_OK;
}
static inline ayatsuri_status ayatsuri_follow_impulse(
        const ayatsuri_follow_coefficients *c,const float impulse[2],ayatsuri_follow_state *state){
    if(!ayatsuri_follow_coefficients_valid(c)||!ayatsuri_follow_state_valid(state)||!impulse||
       !isfinite(impulse[0])||!isfinite(impulse[1]))return AYATSURI_INVALID;
    ayatsuri_follow_state next=*state;
    if(ayatsuri_follow_narrow((double)state->v[0]+(double)impulse[0]/c->inertia[0],&next.v[0])||
       ayatsuri_follow_narrow((double)state->v[1]+(double)impulse[1]/c->inertia[1],&next.v[1]))return AYATSURI_RANGE;
    memcpy(state,&next,sizeof next);return AYATSURI_OK;
}

/* Q15 sine on 64 equally spaced phases; linear interpolation uses lower26
 * bits. One full turn is 2^32. Sample current phases, then advance all three
 * with defined uint32 wrap. No random source, wall clock or global phase.
 * output is f32(.55*s0+.30*s1+.15*s2), not three rounded intermediates.
 * phase==increment is supported. out must not overlap either integer array. */
static const int16_t ayatsuri_follow_sine[64]={
    0,3212,6393,9512,12539,15446,18204,20787,23170,25329,27245,28898,30273,31356,32137,32609,
    32767,32609,32137,31356,30273,28898,27245,25329,23170,20787,18204,15446,12539,9512,6393,3212,
    0,-3212,-6393,-9512,-12539,-15446,-18204,-20787,-23170,-25329,-27245,-28898,-30273,-31356,-32137,-32609,
    -32767,-32609,-32137,-31356,-30273,-28898,-27245,-25329,-23170,-20787,-18204,-15446,-12539,-9512,-6393,-3212};
static inline double ayatsuri_follow_wave(uint32_t phase){
    const unsigned i=phase>>26;
    const double fraction=(phase&UINT32_C(0x03ffffff))/67108864.0;
    return (ayatsuri_follow_sine[i]+(ayatsuri_follow_sine[(i+1)&63]-ayatsuri_follow_sine[i])*fraction)/32767.0;
}
static inline ayatsuri_status ayatsuri_follow_drive(uint32_t phase[3],const uint32_t increment[3],float *out){
    if(!phase||!increment||!out)return AYATSURI_INVALID;
    const uint32_t next[3]={phase[0]+increment[0],phase[1]+increment[1],phase[2]+increment[2]};
    const double value=.55*ayatsuri_follow_wave(phase[0])+.30*ayatsuri_follow_wave(phase[1])+.15*ayatsuri_follow_wave(phase[2]);
    float sample;
    if(ayatsuri_follow_narrow(value,&sample))return AYATSURI_RANGE;
    *out=sample;memcpy(phase,next,sizeof next);return AYATSURI_OK;
}
#endif
