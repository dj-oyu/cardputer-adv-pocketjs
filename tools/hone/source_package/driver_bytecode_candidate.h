#ifndef HONE_DRIVER_BYTECODE_CANDIDATE_H
#define HONE_DRIVER_BYTECODE_CANDIDATE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "driver bytecode requires finite checks"
#endif
/* Candidate interpreter for trusted compiler output and same-frame resolved
 * scalar inputs. Fixed stack, no allocation, no retained pointers. Rejects
 * nonfinite intermediate results; output is unchanged on all failures. This
 * does not resolve Blender paths, drivers, modifiers or frame ownership. */
typedef struct { uint8_t op; uint16_t arg; double value; } hone_driver_instruction;
typedef struct { const hone_driver_instruction *code; size_t length; uint16_t variables; uint32_t first; } hone_driver_program;
static inline int hone_driver_run(const hone_driver_instruction *code,size_t count,
                                  const double *variables,size_t variable_count,double *out){
 if(!code||!out||!count||count>256||variable_count>64||(variable_count&&!variables))return 0;
 double stack[32];size_t n=0,pc=0;
 while(pc<count){
  hone_driver_instruction ins=code[pc++];double a,b,r;
  switch(ins.op){
   case 1: if(n==32||!isfinite(ins.value))return 0;stack[n++]=ins.value;break;
   case 2: if(n==32||ins.arg>=variable_count||!isfinite(variables[ins.arg]))return 0;stack[n++]=variables[ins.arg];break;
   case 3: if(!n)return 0;stack[n-1]=-stack[n-1];break;
   case 4: case 5: case 6: case 8: case 9: case 10: case 11:
    if(n<2)return 0;
    b=stack[--n];a=stack[n-1];
    switch(ins.op){case 4:r=a+b;break;case 5:r=a-b;break;case 6:r=a*b;break;
     case 8:r=a<=b?a:b;break;case 9:r=a>=b?a:b;break;case 10:r=a==b;break;default:r=a>=b;break;}
    if(!isfinite(r))return 0;
    stack[n-1]=r;break;
   case 7: if(!n||ins.arg>4)return 0;r=pow(stack[n-1],(double)ins.arg);if(!isfinite(r))return 0;stack[n-1]=r;break;
   case 12: if(!n||ins.arg<pc||ins.arg>=count)return 0;if(stack[--n]==0)pc=ins.arg;break;
   case 13: if(ins.arg<pc||ins.arg>=count)return 0;pc=ins.arg;break;
   case 14: if(n!=1||pc!=count)return 0;*out=stack[0];return 1;
   default:return 0;
  }
 }
 return 0;
}
/* programs and input-slot mapping are trusted complete immutable cold data;
 * inputs are resolved for the same frame. All arrays must be disjoint. The
 * caller owns scratch and ready; on failure ready is unchanged, scratch may
 * contain partial results. Publication and binding resolution remain external. */
static inline int hone_driver_frame(const hone_driver_program *programs,size_t count,
    const uint16_t *slots,size_t slot_count,const double *inputs,size_t input_count,
    double *scratch,double *ready){
 if(!programs||!slots||!inputs||!scratch||!ready||count>1024||input_count>65536)return 0;
 for(size_t i=0;i<count;i++){
  const hone_driver_program *p=&programs[i];double local[64];
  if(p->variables>64||p->first>slot_count||p->variables>slot_count-p->first)return 0;
  for(size_t k=0;k<p->variables;k++){
   uint16_t index=slots[p->first+k];if(index>=input_count)return 0;
   local[k]=inputs[index];
  }
  if(!hone_driver_run(p->code,p->length,local,p->variables,&scratch[i]))return 0;
 }
 for(size_t i=0;i<count;i++)ready[i]=scratch[i];
 return 1;
}
#endif
