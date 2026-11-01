#include "ayatsuri/ayatsuri_follow.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
_Static_assert(FLT_EVAL_METHOD==0,"follow test requires binary64 intermediates without excess precision");

static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x);}}while(0)
static bool watching;
static unsigned allocations;
void *__real_malloc(size_t n);void *__real_calloc(size_t n,size_t s);void *__real_realloc(void *p,size_t n);void __real_free(void *p);
void *__wrap_malloc(size_t n){if(watching)++allocations;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s){if(watching)++allocations;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n){if(watching)++allocations;return __real_realloc(p,n);}
void __wrap_free(void *p){if(watching)++allocations;__real_free(p);}

static void contracts(void){
    ayatsuri_follow_config raw={{2,3},{0,0},{0,0},0,0},saved_raw=raw;
    ayatsuri_follow_coefficients c;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    CHECK(!memcmp(&raw,&saved_raw,sizeof raw));
    ayatsuri_follow_goal goal={{0,0}},saved_goal=goal;
    ayatsuri_follow_state s={{1,2},{3,4}};
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&s)==AYATSURI_OK);
    CHECK(s.q[0]==(float)(1+AYATSURI_FOLLOW_DT*3)&&s.q[1]==(float)(2+AYATSURI_FOLLOW_DT*4));
    CHECK(s.v[0]==3&&s.v[1]==4&&!memcmp(&goal,&saved_goal,sizeof goal));
    const float impulse[2]={2,-3};
    CHECK(ayatsuri_follow_impulse(&c,impulse,&s)==AYATSURI_OK);
    CHECK(s.v[0]==4&&s.v[1]==3);
    const ayatsuri_follow_state saved=s;
    const float target[2]={.2f,-.3f};
    CHECK(ayatsuri_follow_retarget(target,&goal)==AYATSURI_OK);
    CHECK(!memcmp(&s,&saved,sizeof s)&&goal.q[0]==target[0]&&goal.q[1]==target[1]);
    saved_goal=goal;const float bad_target[2]={0,NAN};
    CHECK(ayatsuri_follow_retarget(bad_target,&goal)==AYATSURI_INVALID&&!memcmp(&goal,&saved_goal,sizeof goal));
    ayatsuri_follow_coefficients before=c;
    raw.stiffness[0]=-1;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_INVALID&&!memcmp(&c,&before,sizeof c));
    raw.stiffness[0]=NAN;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_INVALID&&!memcmp(&c,&before,sizeof c));
    raw=saved_raw;raw.coupling_stiffness=FLT_MAX;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_RANGE&&!memcmp(&c,&before,sizeof c));
    raw=saved_raw;raw.stiffness[0]=8;raw.stiffness[1]=4;raw.damping[0]=2;raw.damping[1]=1;
    raw.coupling_stiffness=3;raw.coupling_damping=.5f;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    s.q[0]=goal.q[0];s.q[1]=goal.q[1];s.v[0]=s.v[1]=0;
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&s)==AYATSURI_OK);
    CHECK(s.q[0]==goal.q[0]&&s.q[1]==goal.q[1]&&s.v[0]==0&&s.v[1]==0);
    s=saved;before=c;c.velocity[0]=NAN;
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&s)==AYATSURI_INVALID&&!memcmp(&s,&saved,sizeof s));
    c=before;c.velocity[0]=DBL_MAX;
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&s)==AYATSURI_RANGE&&!memcmp(&s,&saved,sizeof s));
    c=before;const float bad_force[2]={INFINITY,0};
    CHECK(ayatsuri_follow_step(&c,&goal,bad_force,&s)==AYATSURI_INVALID&&!memcmp(&s,&saved,sizeof s));
    c.inertia[0]=0x1p-149;
    const float huge_j[2]={FLT_MAX,0};
    CHECK(ayatsuri_follow_impulse(&c,huge_j,&s)==AYATSURI_RANGE&&!memcmp(&s,&saved,sizeof s));
    CHECK(ayatsuri_follow_step(NULL,&goal,NULL,&s)==AYATSURI_INVALID);
    CHECK(ayatsuri_follow_step(&before,NULL,NULL,&s)==AYATSURI_INVALID);
    CHECK(ayatsuri_follow_step(&before,&goal,NULL,NULL)==AYATSURI_INVALID);
    CHECK(ayatsuri_follow_prepare(NULL,&c)==AYATSURI_INVALID);
    CHECK(ayatsuri_follow_retarget(NULL,&goal)==AYATSURI_INVALID);
    raw=(ayatsuri_follow_config){{1,2},{0,0},{0,0},20,1};
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    goal=(ayatsuri_follow_goal){{0,0}};s=(ayatsuri_follow_state){{1,0},{0,0}};
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&s)==AYATSURI_OK);
    CHECK(s.v[0]<0&&s.v[1]>0&&fabs(s.v[0]+2*(double)s.v[1])<1e-7);
    uint32_t phase[3]={0,UINT32_C(0x40000000),UINT32_C(0xc0000000)},inc[3]={1,2,3};float sample=7;
    CHECK(ayatsuri_follow_drive(phase,inc,&sample)==AYATSURI_OK&&sample==(float).15);
    CHECK(phase[0]==1&&phase[1]==UINT32_C(0x40000002)&&phase[2]==UINT32_C(0xc0000003));
    phase[0]=phase[1]=phase[2]=UINT32_MAX;inc[0]=inc[1]=inc[2]=1;
    CHECK(ayatsuri_follow_drive(phase,inc,&sample)==AYATSURI_OK&&phase[0]==0&&phase[1]==0&&phase[2]==0);
    phase[0]=1;phase[1]=2;phase[2]=3;
    CHECK(ayatsuri_follow_drive(phase,phase,&sample)==AYATSURI_OK&&phase[0]==2&&phase[1]==4&&phase[2]==6);
    CHECK(ayatsuri_follow_drive(phase,inc,NULL)==AYATSURI_INVALID&&phase[0]==2);
    /* No wrapped allocator is permitted during a long, warmed kernel loop. */
    raw=(ayatsuri_follow_config){{1,2},{30,20},{3,4},8,.6f};
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    s=(ayatsuri_follow_state){{.3f,-.2f},{0,0}};goal=(ayatsuri_follow_goal){{0,0}};
    bool ok=true;watching=true;
    for(unsigned i=0;i<20000;++i){
        float f[2];
        if(ayatsuri_follow_drive(phase,inc,&f[0]))ok=false;
        f[1]=0;
        if(ayatsuri_follow_step(&c,&goal,f,&s))ok=false;
    }
    watching=false;CHECK(ok&&allocations==0);
}


static void reference_step(const ayatsuri_follow_config *c,const ayatsuri_follow_goal *goal,const float force[2],double state[4]){
    const double h=AYATSURI_FOLLOW_DT,kc=c->coupling_stiffness,cc=c->coupling_damping,
      k00=(double)c->stiffness[0]+kc,k11=(double)c->stiffness[1]+kc,
      a00=c->inertia[0]+h*((double)c->damping[0]+cc)+h*h*k00,
      a01=-h*cc-h*h*kc,a11=c->inertia[1]+h*((double)c->damping[1]+cc)+h*h*k11,
      determinant=a00*a11-a01*a01,e0=state[0]-goal->q[0],e1=state[1]-goal->q[1],
      r0=c->inertia[0]*state[2]+h*(force[0]-k00*e0+kc*e1),
      r1=c->inertia[1]*state[3]+h*(force[1]+kc*e0-k11*e1),
      v0=(a11*r0-a01*r1)/determinant,v1=(a00*r1-a01*r0)/determinant;
    state[0]+=h*v0;state[1]+=h*v1;state[2]=v0;state[3]=v1;
}
static double energy(const ayatsuri_follow_config *c,const ayatsuri_follow_state *s){
    const double q0=s->q[0],q1=s->q[1],v0=s->v[0],v1=s->v[1],delta=q0-q1;
    return .5*(c->inertia[0]*v0*v0+c->inertia[1]*v1*v1+
        c->stiffness[0]*q0*q0+c->stiffness[1]*q1*q1+c->coupling_stiffness*delta*delta);
}

/* Independent binary64 reference with non-cancelling determinant and expanded
 * A^-1 K terms; used only to probe the newly declared conditioning boundary. */
static void stable_reference_step(const ayatsuri_follow_config *c,double s[4]){
    const double h=AYATSURI_FOLLOW_DT,i0=c->inertia[0],i1=c->inertia[1],
      k0=c->stiffness[0],k1=c->stiffness[1],c0=c->damping[0],c1=c->damping[1],
      kc=c->coupling_stiffness,cc=c->coupling_damping,
      d0=i0+h*c0+h*h*k0,d1=i1+h*c1+h*h*k1,u=h*cc+h*h*kc,
      determinant=d0*d1+u*(d0+d1),
      b00=(d1+u)*i0/determinant,b01=u*i1/determinant,
      b10=u*i0/determinant,b11=(d0+u)*i1/determinant,
      f00=h*((d1+u)*k0+d1*kc)/determinant,
      f01=h*(h*cc*k1-(i1+h*c1)*kc)/determinant,
      f10=h*(h*cc*k0-(i0+h*c0)*kc)/determinant,
      f11=h*((d0+u)*k1+d0*kc)/determinant,
      v0=b00*s[2]+b01*s[3]-f00*s[0]-f01*s[1],
      v1=b10*s[2]+b11*s[3]-f10*s[0]-f11*s[1];
    s[0]+=h*v0;s[1]+=h*v1;s[2]=v0;s[3]=v1;
}
static void conditioning_boundary(void){
    ayatsuri_follow_config raw={{1,1},{0,0},{0,0},0,0};
    ayatsuri_follow_coeff c;CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    const ayatsuri_follow_coeff unchanged=c;
    ayatsuri_follow_state state={{0,0},{1,1}},prior=state;
    const ayatsuri_follow_goal goal={{0,0}};
    const float rejected_couplings[3]={1e18f,1.8e9f,FLT_MAX};
    for(unsigned i=0;i<3;++i){
        raw.coupling_stiffness=rejected_couplings[i];
        CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_RANGE);
        CHECK(!memcmp(&c,&unchanged,sizeof c)&&!memcmp(&state,&prior,sizeof state));
    }
    raw.coupling_stiffness=0;raw.coupling_damping=1e18f;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_RANGE&&!memcmp(&c,&unchanged,sizeof c));
    raw.coupling_damping=0;raw.inertia[0]=1e-20f;raw.inertia[1]=1e20f;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_RANGE&&!memcmp(&c,&unchanged,sizeof c));
    raw=(ayatsuri_follow_config){{1,1},{0,0},{0,0},0,0};
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    CHECK(ayatsuri_follow_step(&c,&goal,NULL,&state)==AYATSURI_OK&&state.v[0]==1&&state.v[1]==1);
    /* Adjacent binary32 stiffness values straddle kappa=1e6 for I0=I1=1.
     * The rejected value's kappa is 1000000.0044..., not rounded to 1e6. */
    const float above=(float)((AYATSURI_FOLLOW_MAX_CONDITION-1)/(2*AYATSURI_FOLLOW_DT*AYATSURI_FOLLOW_DT)),
      below=nextafterf(above,0);
    raw.coupling_stiffness=above;c=unchanged;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_RANGE&&!memcmp(&c,&unchanged,sizeof c));
    raw.coupling_stiffness=below;
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    state=prior;
    for(unsigned tick=0;tick<1200;++tick){
        CHECK(ayatsuri_follow_step(&c,&goal,NULL,&state)==AYATSURI_OK);
        CHECK(state.v[0]==1&&state.v[1]==1&&state.q[0]==state.q[1]);
        CHECK(energy(&raw,&state)==1);
    }
    CHECK(fabs(state.q[0]-20)<1e-3);
    /* Unequal anchored stiffness/damping close to the same limit exercises
     * restoring coefficients, not merely the B common-velocity eigenmode. */
    raw=(ayatsuri_follow_config){{1,2},{3,7},{.2f,.5f},2.6e9f,.1f};
    CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
    double maximum_q_error=0,maximum_v_error=0,maximum_single_q=0,maximum_single_v=0;
    for(unsigned run=0;run<2;++run){
        state=(ayatsuri_follow_state){{run?1:.2f,run?1:.2f},{run?0:.1f,run?0:-.2f}};
        double reference[4]={state.q[0],state.q[1],state.v[0],state.v[1]},previous=energy(&raw,&state);
        for(unsigned tick=0;tick<1200;++tick){
            double single[4]={state.q[0],state.q[1],state.v[0],state.v[1]};
            stable_reference_step(&raw,single);stable_reference_step(&raw,reference);
            CHECK(ayatsuri_follow_step(&c,&goal,NULL,&state)==AYATSURI_OK);
            maximum_q_error=fmax(maximum_q_error,fmax(fabs(state.q[0]-reference[0]),fabs(state.q[1]-reference[1])));
            maximum_v_error=fmax(maximum_v_error,fmax(fabs(state.v[0]-reference[2]),fabs(state.v[1]-reference[3])));
            maximum_single_q=fmax(maximum_single_q,fmax(fabs(state.q[0]-single[0]),fabs(state.q[1]-single[1])));
            maximum_single_v=fmax(maximum_single_v,fmax(fabs(state.v[0]-single[2]),fabs(state.v[1]-single[3])));
            const double current=energy(&raw,&state);
            CHECK(current<=previous+1e-5);previous=current;
            CHECK(fabs(state.q[0])<=1.01&&fabs(state.q[1])<=1.01&&fabs(state.v[0])<3&&fabs(state.v[1])<3);
        }
    }
    CHECK(maximum_q_error<1e-5&&maximum_v_error<1e-5&&maximum_single_q<1e-6&&maximum_single_v<1e-6);
    fprintf(stderr,"FOLLOW_CONDITION_BOUNDARY max_kappa=1000000 adjacent_accept_kc=%.9g adjacent_reject_kc=%.9g common_v1200=1 energy1200=1 unequal_q_error=%.9g unequal_v_error=%.9g single_q_error=%.9g single_v_error=%.9g\n",
      below,above,maximum_q_error,maximum_v_error,maximum_single_q,maximum_single_v);
}

static void numerical_probes(void){
    const double pi=3.141592653589793;
    for(unsigned kind=0;kind<2;++kind)for(unsigned heavy=0;heavy<2;++heavy){
        const double frequency=kind?.9:1.8,zeta=kind?.35:.7;
        ayatsuri_follow_config raw={{1,heavy?2:1},{(float)((2*pi*1.2)*(2*pi*1.2)),0},
            {(float)(2*.7*2*pi*1.2),0},(float)((2*pi*frequency)*(2*pi*frequency)),(float)(2*zeta*2*pi*frequency)};
        ayatsuri_follow_coefficients c;CHECK(ayatsuri_follow_prepare(&raw,&c)==AYATSURI_OK);
        ayatsuri_follow_state s={{.2f,-.1f},{.1f,-.2f}};
        ayatsuri_follow_goal goal={{0,0}};const float zero[2]={0,0};
        unsigned consecutive=0,first_hold=0;double max_angle=0,max_speed=0,previous_energy=energy(&raw,&s);
        for(unsigned tick=1;tick<=1200;++tick){
            CHECK(ayatsuri_follow_step(&c,&goal,zero,&s)==AYATSURI_OK);
            const double current_energy=energy(&raw,&s);
            CHECK(current_energy<=previous_energy+1e-12);previous_energy=current_energy;
            const double angle=fmax(fabs(s.q[0]),fabs(s.q[1])),speed=fmax(fabs(s.v[0]),fabs(s.v[1]));
            max_angle=fmax(max_angle,angle);max_speed=fmax(max_speed,speed);
            consecutive=angle<=1e-3&&speed<=1e-3?consecutive+1:0;
            if(!first_hold&&consecutive>=60)first_hold=tick;
        }
        const double angle=fmax(fabs(s.q[0]),fabs(s.q[1])),speed=fmax(fabs(s.v[0]),fabs(s.v[1]));
        CHECK(first_hold>0&&angle<=1e-3&&speed<=1e-3);
        fprintf(stderr,"FOLLOW_CONVERGENCE config=%s inertia1=%u threshold_q=0.001 threshold_v=0.001 hold_ticks=60 first_hold_completed_s=%.9g residual20_q=%.17g residual20_v=%.17g max_q=%.9g max_v=%.9g strict_angle_1e-8=%s\n",
          kind?"livelysoft":"reference",heavy?2:1,first_hold/60.0,angle,speed,max_angle,max_speed,angle<1e-8?"PASS":"FAIL");
        s=(ayatsuri_follow_state){{.2f,-.1f},{.1f,-.2f}};
        double reference[4]={s.q[0],s.q[1],s.v[0],s.v[1]},step_angle=0,step_speed=0,
          error_q=0,error_v=0,error_tip=0,error1200_q=0,error1200_v=0,error1200_tip=0;
        uint32_t phase[3]={42,UINT32_C(0x81234567),UINT32_C(0xf1234567)},increment[3]={14316558,25769803,38654705};
        for(unsigned tick=1;tick<=24000;++tick){
            float force[2]={0,0};CHECK(ayatsuri_follow_drive(phase,increment,&force[1])==AYATSURI_OK);
            if(tick==401){const float target[2]={.15f,-.1f};CHECK(ayatsuri_follow_retarget(target,&goal)==AYATSURI_OK);}
            if(tick==701){const float j[2]={.1f,-.2f};CHECK(ayatsuri_follow_impulse(&c,j,&s)==AYATSURI_OK);
              reference[2]+=(double)j[0]/raw.inertia[0];reference[3]+=(double)j[1]/raw.inertia[1];}
            double single[4]={s.q[0],s.q[1],s.v[0],s.v[1]};reference_step(&raw,&goal,force,single);
            reference_step(&raw,&goal,force,reference);
            CHECK(ayatsuri_follow_step(&c,&goal,force,&s)==AYATSURI_OK);
            step_angle=fmax(step_angle,fmax(fabs(s.q[0]-single[0]),fabs(s.q[1]-single[1])));
            step_speed=fmax(step_speed,fmax(fabs(s.v[0]-single[2]),fabs(s.v[1]-single[3])));
            error_q=fmax(error_q,fmax(fabs(s.q[0]-reference[0]),fabs(s.q[1]-reference[1])));
            error_v=fmax(error_v,fmax(fabs(s.v[0]-reference[2]),fabs(s.v[1]-reference[3])));
            const double dx=35*sin(s.q[0])+35*sin(s.q[1])-35*sin(reference[0])-35*sin(reference[1]),
              dy=35*cos(s.q[0])+35*cos(s.q[1])-35*cos(reference[0])-35*cos(reference[1]);
            error_tip=fmax(error_tip,hypot(dx,dy));
            if(tick==1200){error1200_q=error_q;error1200_v=error_v;error1200_tip=error_tip;}
        }
        CHECK(step_angle<1e-6&&step_speed<1e-6&&error_q<1e-4&&error_v<1e-4&&error_tip<.01);
        fprintf(stderr,"FOLLOW_PRECISION config=%s inertia1=%u samples=24000 single_q=%.9g single_v=%.9g max1200_q=%.9g max1200_v=%.9g max1200_tip35=%.9g max24000_q=%.9g max24000_v=%.9g max24000_tip35=%.9g\n",
          kind?"livelysoft":"reference",heavy?2:1,step_angle,step_speed,error1200_q,error1200_v,error1200_tip,error_q,error_v,error_tip);
    }
}

static const char *status(ayatsuri_status s){return s==AYATSURI_OK?"OK":s==AYATSURI_INVALID?"INVALID":"RANGE";}
static void floats(const float *p,unsigned n){putchar('[');for(unsigned i=0;i<n;++i)printf("%s%.9g",i?",":"",p[i]);putchar(']');}
static void doubles(const double *p,unsigned n){putchar('[');for(unsigned i=0;i<n;++i)printf("%s%.17g",i?",":"",p[i]);putchar(']');}
static uint32_t random_state=UINT32_C(0x61796174);
static uint32_t random_u32(void){uint32_t x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x;}
static float random_float(float range){return (float)(((double)random_u32()/4294967295.0*2-1)*range);}
static void state_array(const ayatsuri_follow_state *s,float out[4]){out[0]=s->q[0];out[1]=s->q[1];out[2]=s->v[0];out[3]=s->v[1];}
static unsigned json_records;
static void separator(void){if(json_records++)puts(",");}
static void vectors(void){
    putchar('[');
    for(unsigned scenario=0;scenario<40;++scenario){
        ayatsuri_follow_config raw={{1+fabsf(random_float(2)),1+fabsf(random_float(3))},
            {5+fabsf(random_float(70)),5+fabsf(random_float(50))},
            {fabsf(random_float(8)),fabsf(random_float(8))},fabsf(random_float(20)),fabsf(random_float(3))};
        float raw_array[8];memcpy(raw_array,&raw,sizeof raw);
        ayatsuri_follow_coefficients c;ayatsuri_status code=ayatsuri_follow_prepare(&raw,&c);
        CHECK(code==AYATSURI_OK);
        double prepared[14];memcpy(prepared,&c,sizeof c);
        separator();printf("{\"op\":\"prepare\",\"config\":");floats(raw_array,8);
        printf(",\"expected\":");doubles(prepared,14);printf(",\"status\":\"%s\"}",status(code));
        ayatsuri_follow_state s={{random_float(1),random_float(1)},{random_float(2),random_float(2)}};
        ayatsuri_follow_goal goal={{random_float(.4f),random_float(.4f)}};
        for(unsigned tick=0;tick<120;++tick){
            if(tick%17==0){const float target[2]={random_float(.4f),random_float(.4f)};CHECK(ayatsuri_follow_retarget(target,&goal)==AYATSURI_OK);}
            float prior[4],after[4];state_array(&s,prior);
            float force[2]={random_float(2),random_float(2)};
            const bool impulsive=tick%23==0;
            code=impulsive?ayatsuri_follow_impulse(&c,force,&s):ayatsuri_follow_step(&c,&goal,force,&s);
            CHECK(code==AYATSURI_OK);state_array(&s,after);
            separator();printf("{\"op\":\"%s\",\"config\":",impulsive?"impulse":"step");floats(raw_array,8);
            printf(",\"state\":");floats(prior,4);printf(",\"goal\":");floats(goal.q,2);
            printf(",\"%s\":",impulsive?"impulse":"force");floats(force,2);
            printf(",\"expected\":");floats(after,4);printf(",\"status\":\"%s\"}",status(code));
        }
    }
    uint32_t phase[3]={random_u32(),random_u32(),random_u32()},inc[3]={UINT32_C(14316558),UINT32_C(25769803),UINT32_C(38654705)};
    for(unsigned tick=0;tick<128;++tick){
        uint32_t prior[3];memcpy(prior,phase,sizeof prior);float sample=0;
        CHECK(ayatsuri_follow_drive(phase,inc,&sample)==AYATSURI_OK);
        separator();printf("{\"op\":\"drive\",\"phase\":[%u,%u,%u],\"increment\":[%u,%u,%u],\"expectedPhase\":[%u,%u,%u],\"expected\":[%.9g],\"status\":\"OK\"}",
            prior[0],prior[1],prior[2],inc[0],inc[1],inc[2],phase[0],phase[1],phase[2],sample);
    }

    const ayatsuri_follow_config boundary_configs[]={
      {{1,1},{0,0},{0,0},1e18f,0},{{1,1},{0,0},{0,0},1.8e9f,0},
      {{1e-20f,1e20f},{0,0},{0,0},0,0},{{1,1},{0,0},{0,0},0,1e18f},
      {{1,1},{0,0},{0,0},1.7e9f,0},{{1,2},{3,7},{.2f,.5f},2.6e9f,.1f}};
    for(unsigned i=0;i<sizeof boundary_configs/sizeof boundary_configs[0];++i){
        float raw[8];memcpy(raw,&boundary_configs[i],sizeof raw);
        ayatsuri_follow_coeff c;const ayatsuri_status code=ayatsuri_follow_prepare(&boundary_configs[i],&c);
        CHECK(code==(i<4?AYATSURI_RANGE:AYATSURI_OK));
        separator();printf("{\"op\":\"prepare\",\"config\":");floats(raw,8);
        if(code==AYATSURI_OK){double prepared[14];memcpy(prepared,&c,sizeof c);printf(",\"expected\":");doubles(prepared,14);}
        printf(",\"status\":\"%s\"}",status(code));
        if(code==AYATSURI_OK){
            ayatsuri_follow_state s={{1,1},{1,1}};const ayatsuri_follow_goal goal={{0,0}};const float force[2]={0,0};
            CHECK(ayatsuri_follow_step(&c,&goal,force,&s)==AYATSURI_OK);float after[4];state_array(&s,after);
            separator();printf("{\"op\":\"step\",\"config\":");floats(raw,8);
            printf(",\"state\":[1,1,1,1],\"goal\":[0,0],\"force\":[0,0],\"expected\":");floats(after,4);
            printf(",\"status\":\"OK\"}");
        }
    }
    puts("]");
}

int main(int argc,char **argv){
    contracts();conditioning_boundary();numerical_probes();
    if(argc>1&&!strcmp(argv[1],"--json"))vectors();
    fprintf(stderr,"AYATSURI_FOLLOW_%s checks=%u allocations=%u vectors=%u config=%zu prepared=%zu state=%zu\n",
        failures?"FAIL":"PASS",checks,allocations,json_records,sizeof(ayatsuri_follow_config),sizeof(ayatsuri_follow_coefficients),sizeof(ayatsuri_follow_state));
    return failures?1:0;
}
