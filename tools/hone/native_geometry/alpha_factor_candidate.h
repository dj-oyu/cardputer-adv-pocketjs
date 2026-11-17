#ifndef HONE_ALPHA_FACTOR_CANDIDATE_H
#define HONE_ALPHA_FACTOR_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "alpha factor candidate requires IEEE finite checks"
#endif
/* Limited scalar alpha-factor graph. Source materials must first pass the
 * compiler's explicit transparent/opaque socket and coordinate admission.
 * Inputs/code/scratch/output are complete, immutable or exclusively owned,
 * and pairwise disjoint. Scratch may change on failure; output never does. */
enum {HONE_ALPHA_CONST,HONE_ALPHA_X,HONE_ALPHA_Y,HONE_ALPHA_Z,HONE_ALPHA_DRIVER,
      HONE_ALPHA_ADD,HONE_ALPHA_SUB,HONE_ALPHA_MUL,HONE_ALPHA_DIV,HONE_ALPHA_MIN,
      HONE_ALPHA_MAX,HONE_ALPHA_LT,HONE_ALPHA_GT,HONE_ALPHA_SQRT,HONE_ALPHA_CLAMP};
typedef struct {uint8_t op;uint16_t a,b;float value;} hone_alpha_instruction;
typedef struct {const hone_alpha_instruction *code;size_t count;uint16_t result;} hone_alpha_program;
static inline int hone_alpha_factor(const hone_alpha_program *program,const float xyz[3],
 const double *driver,size_t driver_count,float *scratch,size_t capacity,float *out){
 if(!program||!program->code||!xyz||!scratch||!out||!program->count||program->count>1024||
    capacity<program->count||program->result>=program->count)return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(xyz[k]))return 0;
 for(size_t i=0;i<program->count;i++){
  hone_alpha_instruction in=program->code[i];float v=0;
  if(in.op==HONE_ALPHA_CONST){v=in.value;}
  else if(in.op>=HONE_ALPHA_X&&in.op<=HONE_ALPHA_Z){v=xyz[in.op-HONE_ALPHA_X];}
  else if(in.op==HONE_ALPHA_DRIVER){if(!driver||in.a>=driver_count||!isfinite(driver[in.a])||fabs(driver[in.a])>FLT_MAX)return 0;v=(float)driver[in.a];}
  else if(in.op==HONE_ALPHA_SQRT||in.op==HONE_ALPHA_CLAMP){
   if(in.a>=i)return 0;float a=scratch[in.a];
   if(in.op==HONE_ALPHA_SQRT){if(a<0)return 0;v=sqrtf(a);}else v=fmaxf(0,fminf(1,a));
  }else{
   if(in.a>=i||in.b>=i)return 0;float a=scratch[in.a],b=scratch[in.b];
   switch(in.op){case HONE_ALPHA_ADD:v=a+b;break;case HONE_ALPHA_SUB:v=a-b;break;
    case HONE_ALPHA_MUL:v=a*b;break;case HONE_ALPHA_DIV:if(b==0)return 0;v=a/b;break;
    case HONE_ALPHA_MIN:v=fminf(a,b);break;case HONE_ALPHA_MAX:v=fmaxf(a,b);break;
    case HONE_ALPHA_LT:v=a<b?1.f:0.f;break;case HONE_ALPHA_GT:v=a>b?1.f:0.f;break;
    default:return 0;}
  }
  if(!isfinite(v))return 0;scratch[i]=v;
 }
 float value=scratch[program->result];if(value<0||value>1)return 0;*out=value;return 1;
}
#endif
