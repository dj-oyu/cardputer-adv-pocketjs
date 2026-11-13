/* Rebuilt public synthetic contract, not a byte restoration of lost harness. */
#include <assert.h>
#include <stdio.h>
#include "ayatsuri_skin_dq.h"
static const ayatsuri_transform identity={{1,0,0,0,0,1,0,0,0,0,1,0}};
static void rotate(double angle,const double v[3],double out[3]){
 const double a=0.7071067811865475244,c=cos(angle),s=sin(angle),d=a*(v[0]+v[2]);
 const double cross[3]={-a*v[1],a*(v[0]-v[2]),a*v[1]};
 for(int k=0;k<3;k++)out[k]=v[k]*c+cross[k]*s+(k==1?0:a*d*(1-c));
}
/* Signed-permutation expectations do not use quaternion or matrix helpers.
 * Covers trace-positive and all three dominant-diagonal conversion branches.
 * A single full-weight 180-degree rotation is valid; only ambiguous blends
 * between different positive influences are rejected by the rigid contract. */
static void known_axis_contract(void){
 const float source[3]={2,-3,5}, normal[3]={2,-3,6}, translation[3]={.25f,-.5f,1.25f};
 unsigned checked=0;
 for(unsigned axis=0;axis<3;axis++)for(unsigned turns=0;turns<4;turns++){
  unsigned a=(axis+1)%3,b=(axis+2)%3;
  ayatsuri_transform p=identity;
  for(unsigned row=0;row<3;row++)p.m[row*4+3]=translation[row];
  float ep[3],en[3];memcpy(ep,source,sizeof ep);memcpy(en,normal,sizeof en);
  for(unsigned step=0;step<turns;step++){
   float old=ep[a];ep[a]=-ep[b];ep[b]=old;
   old=en[a];en[a]=-en[b];en[b]=old;
  }
  static const int cos_quarter[4]={1,0,-1,0},sin_quarter[4]={0,1,0,-1};
  p.m[a*4+a]=p.m[b*4+b]=(float)cos_quarter[turns];
  p.m[a*4+b]=(float)-sin_quarter[turns];p.m[b*4+a]=(float)sin_quarter[turns];
  for(unsigned slot=0;slot<4;slot++){
   ayatsuri_skin_vertex v={0};memcpy(v.position,source,sizeof source);memcpy(v.normal,normal,sizeof normal);v.weights[slot]=65535;
   ayatsuri_skin_result result;
   assert(ayatsuri_skin_evaluate_rigid_dq(&p,1,&v,1,&result)==AYATSURI_OK);
   for(unsigned k=0;k<3;k++){
    assert(fabs(result.position[k]-(ep[k]+translation[k]))<1e-6);
    assert(fabs(result.normal[k]-en[k]/7.0)<1e-7);
   }
   checked++;
  }
 }
 printf("DQ_AXIS_BRANCH_PASS cases=%u axes=xyz quarter_turns=0,1,2,3 influence_slots=4 translated_points unit_normals\n",checked);
}
int main(void){
 known_axis_contract();
 const double theta=1.047197551196597746;ayatsuri_transform p[2]={identity,identity};
 for(int col=0;col<3;col++){double e[3]={0},o[3];e[col]=1;rotate(theta,e,o);for(int row=0;row<3;row++)p[1].m[row*4+col]=(float)o[row];}
 for(int row=0;row<3;row++)p[1].m[row*4+3]=(row==0?1:0)-p[1].m[row*4];
 ayatsuri_skin_vertex v[6]={0};ayatsuri_skin_result out[6],lbs[6],old[6];double err=0,gap=0,quant=0;
 for(int i=0;i<6;i++){v[i].position[0]=(float)(i%3*2-2);v[i].position[1]=i<3?-1:1;v[i].normal[2]=1;v[i].joints[1]=1;v[i].weights[1]=i%3==0?0:i%3==1?32768:65535;v[i].weights[0]=65535-v[i].weights[1];}
 assert(ayatsuri_skin_evaluate_rigid_dq(p,2,v,6,out)==AYATSURI_OK);assert(ayatsuri_skin_evaluate(p,2,v,6,lbs)==AYATSURI_OK);
 for(int i=0;i<6;i++){
  double w=(double)v[i].weights[1]/65535,phi=2*atan2(w*sin(theta/2),1-w+w*cos(theta/2));double src[3]={v[i].position[0]-1,v[i].position[1],0},pred[3],normal[3],n[3]={0,0,1},exact[3];rotate(phi,src,pred);pred[0]+=1;rotate(phi,n,normal);
  double wf=i%3==0?0:i%3==1?.5:1;rotate(2*atan2(wf*sin(theta/2),1-wf+wf*cos(theta/2)),src,exact);exact[0]+=1;
  for(int k=0;k<3;k++){err=fmax(err,fabs(out[i].position[k]-pred[k]));err=fmax(err,fabs(out[i].normal[k]-normal[k]));gap=fmax(gap,fabs(out[i].position[k]-lbs[i].position[k]));quant=fmax(quant,fabs(exact[k]-pred[k]));}
 }
 assert(err<2e-6&&gap>.01&&quant>0);unsigned negatives=0;
 memset(out,0x5a,sizeof out);memcpy(old,out,sizeof out);
 for(int kind=0;kind<7;kind++){
  ayatsuri_transform bad[2];memcpy(bad,p,sizeof bad);ayatsuri_skin_vertex b[6];memcpy(b,v,sizeof b);
  if(kind==0)bad[1].m[0]=NAN;
  if(kind==1)bad[1].m[0]=2;
  if(kind==2)b[5].weights[0]=0;
  if(kind==3)b[5].joints[3]=2;
  if(kind==4)b[5].normal[2]=0;
  if(kind==5)b[5].position[0]=INFINITY;
  if(kind==6)bad[1].m[1]+=.1f;
  /* kind2 must actually break the full-weight sum. */
  if(kind==2)b[5].weights[1]=1;
  assert(ayatsuri_skin_evaluate_rigid_dq(bad,2,b,6,out)!=AYATSURI_OK);assert(memcmp(old,out,sizeof out)==0);negatives++;
 }
 ayatsuri_transform extra[3]={p[0],p[1],identity};extra[2].m[0]=NAN;
 assert(ayatsuri_skin_evaluate_rigid_dq(extra,3,v,6,out)!=AYATSURI_OK&&memcmp(old,out,sizeof out)==0);negatives++;
 extra[2]=identity;extra[2].m[0]=-1;
 assert(ayatsuri_skin_evaluate_rigid_dq(extra,3,v,6,out)!=AYATSURI_OK&&memcmp(old,out,sizeof out)==0);negatives++;
 ayatsuri_transform halfturn[2]={identity,identity};halfturn[1].m[5]=-1;halfturn[1].m[10]=-1;
 assert(ayatsuri_skin_evaluate_rigid_dq(halfturn,2,v,6,out)==AYATSURI_RANGE&&memcmp(old,out,sizeof out)==0);negatives++;
 assert(ayatsuri_skin_evaluate_method((ayatsuri_skin_method)99,p,2,v,6,out)==AYATSURI_INVALID&&memcmp(old,out,sizeof out)==0);
 assert(ayatsuri_skin_evaluate_rigid_dq(p,2,v,6,out)==AYATSURI_OK);memcpy(old,out,sizeof out);memset(v,0xff,sizeof v);memset(p,0xff,sizeof p);assert(memcmp(old,out,sizeof out)==0);
 printf("DQ_REBUILT_PASS max_error=%.12g max_component_LBS_gap=%.12g quantization_delta=%.12g transactional_negatives=%u\n",err,gap,quant,negatives);
 return 0;
}
