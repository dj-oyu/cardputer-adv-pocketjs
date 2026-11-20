#include <assert.h>
#include <stdio.h>
#include "rigid_dq_candidate.c"
int main(void){
 double p[3]={1,2,3},j[14]={1,0,0,0,2,-1,3,1,0,0,0,-2,1,-3},w[2]={.25,.75},o[3];unsigned idx[2]={0,1};
 assert(rigid_dq_position(p,j,2,idx,w,2,o));assert(o[0]==0&&o[1]==2.5&&o[2]==1.5);
 idx[1]=2;o[0]=7;o[1]=8;o[2]=9;assert(!rigid_dq_position(p,j,2,idx,w,2,o));assert(o[0]==7&&o[1]==8&&o[2]==9);
 idx[1]=1;assert(rigid_dq_position(p,j,2,idx,w,2,o));
 w[1]=NAN;assert(!rigid_dq_position(p,j,2,idx,w,2,o));
 puts("PASS rigid DQ C bounds, late failure and retry");return 0;
}
