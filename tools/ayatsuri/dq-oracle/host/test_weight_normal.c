/* Independent common-pivot X-rotation oracle. No product API additions. */
#include <assert.h>
#include <stdio.h>
#include "ayatsuri_skin_dq.h"
static const ayatsuri_transform I={{1,0,0,0,0,1,0,0,0,0,1,0}};
static double phi(double a,unsigned w){double t=w/65535.;return 2*atan2(t*sin(a/2),1-t+t*cos(a/2));}
int main(void){
 const unsigned weights[]={0,1,16384,32767,32768,49151,65534,65535};
 double error=0,quant=0,surface_gap=0;unsigned cases=0;
 for(unsigned degree=30;degree<=120;degree*=2){
  const double a=degree*acos(-1.)/180,c=cos(a),s=sin(a);
  ayatsuri_transform g[2]={I,I},ib[2]={I,I},p[2];
  g[1].m[5]=c;g[1].m[6]=-s;g[1].m[9]=s;g[1].m[10]=c;
  g[1].m[7]=1;ib[1].m[7]=-1; /* G * IB, pivot y=1 */
  assert(ayatsuri_skin_palette(g,ib,2,p)==AYATSURI_OK);
  for(unsigned j=0;j<sizeof weights/sizeof *weights;j++){
   ayatsuri_skin_vertex v={.position={2,3,0},.normal={0,0,7},.joints={0,1},.weights={65535-weights[j],weights[j]}};
   ayatsuri_skin_result out;assert(ayatsuri_skin_evaluate_rigid_dq(p,2,&v,1,&out)==AYATSURI_OK);
   double b=phi(a,weights[j]),pos[3]={2,1+2*cos(b),2*sin(b)},norm[3]={0,-sin(b),cos(b)};
   for(int k=0;k<3;k++){assert(isfinite(out.position[k])&&isfinite(out.normal[k]));error=fmax(error,fabs(pos[k]-out.position[k]));error=fmax(error,fabs(norm[k]-out.normal[k]));}
   assert(fabs(hypot(out.normal[1],out.normal[2])-1)<2e-7);cases++;
   if(weights[j]==32768)quant=fmax(quant,fabs(b-a/2));
  }
  /* Nonuniform weights deform a triangle. Its geometric face normal is NOT
   * the fixed-weight transported normal at its middle-weight vertex. */
  ayatsuri_skin_vertex v[3]={0};ayatsuri_skin_result out[3];
  const float xyz[3][3]={{-1,0,0},{1,0,0},{0,2,0}};const unsigned w[3]={0,32768,65535};
  for(int j=0;j<3;j++){memcpy(v[j].position,xyz[j],12);v[j].normal[2]=1;v[j].joints[1]=1;v[j].weights[1]=w[j];v[j].weights[0]=65535-w[j];}
  assert(ayatsuri_skin_evaluate_rigid_dq(p,2,v,3,out)==AYATSURI_OK);
  double e[3],f[3],n[3];for(int k=0;k<3;k++){e[k]=out[1].position[k]-out[0].position[k];f[k]=out[2].position[k]-out[0].position[k];}
  for(int k=0;k<3;k++)n[k]=e[(k+1)%3]*f[(k+2)%3]-e[(k+2)%3]*f[(k+1)%3];
  double len=hypot(hypot(n[0],n[1]),n[2]);assert(len>0);
  double gap=0;for(int k=0;k<3;k++)gap+=pow(n[k]/len-out[1].normal[k],2);
  surface_gap=fmax(surface_gap,sqrt(gap));assert(sqrt(gap)>.05);
 }
 assert(error<1e-6&&quant>1e-6&&surface_gap>.05);
 printf("DQ_WEIGHT_NORMAL_PASS cases=%u max_component_error=%.12g quantized_half_angle_delta=%.12g surface_normal_gap=%.12g\n",cases,error,quant,surface_gap);
}
