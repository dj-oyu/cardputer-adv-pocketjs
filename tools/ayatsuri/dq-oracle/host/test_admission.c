/* Test-only boundary proposal, not a production API or provenance verifier. */
#include <assert.h>
#include <stdio.h>
#include "ayatsuri_skin_dq.h"
static const ayatsuri_transform I={{1,0,0,0,0,1,0,0,0,0,1,0}};
/* Caller-established source contract is indispensable: matrices alone cannot
 * Non-NULL pointers are a precondition of this test-only helper.
 * Matrices alone cannot reveal authored scale that has already been cancelled upstream. */
static ayatsuri_status admit(int source_rigid,const ayatsuri_transform *g,const ayatsuri_transform *ib,ayatsuri_transform *p){
 double q[4],d[4];
 if(!source_rigid||!ayatsuri_dq_from_rigid(g,q,d)||!ayatsuri_dq_from_rigid(ib,q,d))return AYATSURI_INVALID;
 ayatsuri_transform candidate;ayatsuri_status s=ayatsuri_skin_palette(g,ib,1,&candidate);
 if(s!=AYATSURI_OK)return s;
 if(!ayatsuri_dq_from_rigid(&candidate,q,d))return AYATSURI_INVALID;
 *p=candidate;return AYATSURI_OK;
}
int main(void){
 unsigned rejects=0;ayatsuri_transform g=I,b=I,p,old;double q[4],d[4];
 for(int k=0;k<3;k++){
  g=b=I;
  if(k==0){g.m[0]=2;b.m[0]=.5;}
  if(k==1){g.m[1]=.25;b.m[1]=-.25;}
  if(k==2){g.m[0]=-1;b.m[0]=-1;}
  assert(ayatsuri_skin_palette(&g,&b,1,&p)==AYATSURI_OK);
  assert(ayatsuri_dq_from_rigid(&p,q,d)); /* product-only check falsely admits */
  memset(&p,0x5a,sizeof p);old=p;
  assert(admit(1,&g,&b,&p)==AYATSURI_INVALID&&!memcmp(&p,&old,sizeof p));rejects++;
 }
 g=b=I;g.m[3]=3;b.m[3]=-2;
 assert(admit(1,&g,&b,&p)==AYATSURI_OK&&p.m[3]==1);
 old=p;assert(admit(0,&g,&b,&p)==AYATSURI_INVALID&&!memcmp(&p,&old,sizeof p));rejects++;
 b.m[7]=NAN;assert(admit(1,&g,&b,&p)==AYATSURI_INVALID&&!memcmp(&p,&old,sizeof p));rejects++;
 b=I;g.m[3]=INFINITY;assert(admit(1,&g,&b,&p)==AYATSURI_INVALID&&!memcmp(&p,&old,sizeof p));rejects++;
 /* Near-identity storage tolerance does not imply authoring provenance. */
 g=I;g.m[0]=1.0000001f;assert(ayatsuri_dq_from_rigid(&g,q,d));
 assert(admit(0,&g,&b,&p)==AYATSURI_INVALID&&!memcmp(&p,&old,sizeof p));rejects++;
 printf("DQ_SOURCE_ADMISSION_PASS negatives=%u cancellation3 finite2 upstream_flag2 translation_recovery no_product_change\n",rejects);
}
