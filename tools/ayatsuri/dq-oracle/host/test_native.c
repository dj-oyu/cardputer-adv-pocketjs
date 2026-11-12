/* Test-only: experimental DQ -> existing native mesh packet/snapshot. */
#define main preserved_lbs_test_main
#include "test_render.c"
#undef main
#include "ayatsuri/ayatsuri_skin_dq.h"
static void known_rotation(double t,const double v[3],double out[3]){
 double a=1/sqrt(2.),c=cos(t),s=sin(t),dot=a*(v[0]+v[2]);double cross[3]={-a*v[1],a*(v[0]-v[2]),a*v[1]};
 for(int k=0;k<3;k++)out[k]=c*v[k]+s*cross[k]+(k==1?0:a)*dot*(1-c);
}
int main(int argc,char **argv){
 assert(argc==2);size_t bytes;uint8_t *blob=load(argv[1],"plain",&bytes);ayatsuri_model model;assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
 size_t n=ayatsuri_model_instance_bytes(&model);void *arena=malloc(n),*refarena=malloc(n);unsigned owner=1,refowner=2;ayatsuri_model_instance actor,reference_actor;
 assert(ayatsuri_model_instance_init(&model,arena,n,&owner,1,&actor)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_init(&model,refarena,n,&refowner,1,&reference_actor)==AYATSURI_MODEL_OK);
 ayatsuri_transform p[2]={identity,identity};double theta=acos(-1.)/3;
 for(int col=0;col<3;col++){double v[3]={0},r[3];v[col]=1;known_rotation(theta,v,r);for(int row=0;row<3;row++)p[1].m[row*4+col]=(float)r[row];}
 for(int row=0;row<3;row++)p[1].m[row*4+3]=15*((row==0?1:0)-p[1].m[row*4]);
 ayatsuri_skin_vertex v[6];rest_inputs(v);ayatsuri_skin_result actual[6],expected[6];
 for(int i=0;i<6;i++){
  if(i%3==1){v[i].weights[0]=32767;v[i].weights[1]=32768;}
  double w=(double)v[i].weights[1]/65535,phi=2*atan2(w*sin(theta/2),1-w+w*cos(theta/2)),rel[3]={v[i].position[0]-15,v[i].position[1],0},r[3],normal[3]={0,0,1};
  known_rotation(phi,rel,r);r[0]+=15;for(int k=0;k<3;k++)expected[i].position[k]=(float)r[k];known_rotation(phi,normal,r);for(int k=0;k<3;k++)expected[i].normal[k]=(float)r[k];
 }
 assert(ayatsuri_skin_evaluate_rigid_dq(p,2,v,6,actual)==AYATSURI_OK);
 for(int i=0;i<6;i++)for(int k=0;k<3;k++){assert(fabsf(actual[i].position[k]-expected[i].position[k])<5e-6f);assert(fabsf(actual[i].normal[k]-expected[i].normal[k])<2e-6f);}
 const float cam[9]={200,0,400,0,0,0,1,120,67};ayatsuri_model_mesh_geometry ga={1,6,actual},ge={1,6,expected};
 assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,cam,light,&ga,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_evaluate_meshes(&reference_actor,&refowner,1,cam,light,&ge,1)==AYATSURI_MODEL_OK);
 uint8_t sa[2048]={0},se[2048]={0};float scratch[240*8];ksn_mesh_snapshot a=snapshot(sa,sizeof sa),e=snapshot(se,sizeof se);append_model(&actor,&owner,&a);append_model(&reference_actor,&refowner,&e);
 render(&a,pixels,actual_z,4,1,scratch);render(&e,reference,reference_z,8,8,scratch);assert(!memcmp(pixels,reference,sizeof pixels));
 /* Cold rendering must differ: background-only/no overridden mesh cannot pass. */
 assert(ayatsuri_model_instance_evaluate(&reference_actor,&refowner,1,cam,light)==AYATSURI_MODEL_OK);e=snapshot(se,sizeof se);append_model(&reference_actor,&refowner,&e);render(&e,rest,reference_z,8,8,scratch);assert(memcmp(rest,reference,sizeof rest));
 const size_t saved_size=a.packets.size;const unsigned saved_packets=a.packets.packets;
 /* Caller gates failed DQ before evaluate_meshes: previous packet remains. */
 v[5].normal[2]=0;assert(ayatsuri_skin_evaluate_rigid_dq(p,2,v,6,actual)!=AYATSURI_OK);uint8_t saved[2048];memcpy(saved,sa,sizeof sa);a=snapshot(sa,sizeof sa);append_model(&actor,&owner,&a);assert(a.packets.size==saved_size&&a.packets.packets==saved_packets);assert(!memcmp(sa,saved,saved_size));
 /* Poison borrowed arrays before copying packet; native evaluation owns data. */
 memset(actual,0xff,sizeof actual);memset(expected,0xff,sizeof expected);memset(p,0xff,sizeof p);memset(v,0xff,sizeof v);a=snapshot(sa,sizeof sa);append_model(&actor,&owner,&a);assert(a.packets.size==saved_size&&a.packets.packets==saved_packets);assert(!memcmp(sa,saved,saved_size));
 assert(ayatsuri_model_instance_dispose(&actor,&owner,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_dispose(&reference_actor,&refowner,1)==AYATSURI_MODEL_OK);free(arena);free(refarena);free(blob);memset(&model,0,sizeof model);
 render(&a,pixels,actual_z,8,8,scratch);assert(!memcmp(pixels,reference,sizeof pixels));assert(live==0);
 printf("DQ_NATIVE_CONTRACT_PASS analytic_raw_geometry shared_renderer_panel caller_gated_failure packet_ownership snapshot_after_free live0 peak=%zu\n",peak);
 return 0;
}
