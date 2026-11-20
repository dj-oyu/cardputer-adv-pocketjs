#include <assert.h>
#include "nearest_batch_candidate.c"
int main(void){double v[9]={0,0,0,1,0,0,0,1,0},p[6]={.2,.2,1,.3,.3,2},o[6];uint32_t t[72],f[2];nearest_box b[3];size_t n;
for(int i=0;i<72;i++)t[i]=i%3;
assert(nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));assert(f[0]==0&&f[1]==0);
v[2]=v[5]=v[8]=.5;assert(nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));assert(o[2]==.5);
assert(nearest_batch(p,2,v,3,t,24,b,0,o,f,&n)&&n==48);
t[71]=99;assert(!nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));t[71]=2;assert(nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));
p[3]=NAN;assert(!nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));p[3]=.3;assert(nearest_batch(p,2,v,3,t,24,b,3,o,f,&n));return 0;}
