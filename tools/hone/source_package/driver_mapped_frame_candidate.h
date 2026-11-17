#ifndef HONE_DRIVER_MAPPED_FRAME_CANDIDATE_H
#define HONE_DRIVER_MAPPED_FRAME_CANDIDATE_H
#include "driver_bytecode_candidate.h"
/* Independent host candidate, default interpreter is unchanged. This freezes
 * its opcode arithmetic and control flow; differential tests guard drift.
 * Reads shared same-frame inputs directly through per-program slot mappings,
 * removing frame's local[64] copy. Immutable complete code/slots/inputs and
 * disjoint scratch/ready are REQUIRED for the entire call. No concurrent
 * publication or callbacks. No allocation or persistent pointer retention.
 * This is not driver binding/dependency resolution or runtime admission. */
static inline int hone_driver_mapped_run(const hone_driver_instruction *code,size_t count,
                                  const double *inputs,size_t input_count,
                                  const uint16_t *mapping,size_t variable_count,double *out){
 if(!code||!out||!count||count>256||variable_count>64||input_count>65536||!inputs||(variable_count&&!mapping))return 0;
 double stack[32];size_t n=0,pc=0;
 while(pc<count){
  hone_driver_instruction ins=code[pc++];double a,b,r;
  switch(ins.op){
   case 1: if(n==32||!isfinite(ins.value))return 0;stack[n++]=ins.value;break;
   case 2: if(n==32||ins.arg>=variable_count||mapping[ins.arg]>=input_count||!isfinite(inputs[mapping[ins.arg]]))return 0;stack[n++]=inputs[mapping[ins.arg]];break;
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
/* Validate ALL local slot indices, including an untaken branch, before
 * evaluating each program, preserving the existing frame failure contract.
 * Only loaded values require finiteness, preserving short-circuit behavior.
 * Failed frame keeps ready unchanged; scratch may contain a partial frame. */
static inline int hone_driver_mapped_frame(const hone_driver_program *programs,size_t count,
    const uint16_t *slots,size_t slot_count,const double *inputs,size_t input_count,
    double *scratch,double *ready){
 if(!programs||!slots||!inputs||!scratch||!ready||count>1024||input_count>65536)return 0;
 for(size_t i=0;i<count;i++){
  const hone_driver_program *p=&programs[i];
  if(p->variables>64||p->first>slot_count||p->variables>slot_count-p->first)return 0;
  for(size_t k=0;k<p->variables;k++)if(slots[p->first+k]>=input_count)return 0;
  if(!hone_driver_mapped_run(p->code,p->length,inputs,input_count,slots+p->first,p->variables,&scratch[i]))return 0;
 }
 for(size_t i=0;i<count;i++)ready[i]=scratch[i];
 return 1;
}
#endif
