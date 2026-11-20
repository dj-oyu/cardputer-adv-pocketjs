#include "eye_ledger.h"
#include <assert.h>

static bool masked_shader(void *context,unsigned material,const float xyz[3],float *alpha,float rgb[3]){
    (void)context;(void)material;
    *alpha=xyz[2]<.25f?0:1;
    rgb[0]=xyz[0]>0?.75f:.125f;rgb[1]=.5f;rgb[2]=.875f;
    return true;
}
static bool solid_shader(void *context,unsigned material,const float xyz[3],float *alpha,float rgb[3]){
    (void)context;(void)material;(void)xyz;
    *alpha=1;rgb[0]=1;rgb[1]=rgb[2]=0;return true;
}
int main(void){
    static const hone_alpha_instruction code[]={{.op=HONE_ALPHA_CONST,.value=1}};
    static const hone_alpha_program program={code,1,0};
    for(unsigned subgrid=0;subgrid<2;subgrid++){
        hone_graph_triangle t={.v={{3,4,1,{-.5f,-7,.125f}},{17,3,2,{.75f,9,.5f}},
                                  {5,19,3,{.25f,11,.25f}}},.alpha=&program,.color=&program};
        if(subgrid)t.v[0].x=nextafterf(3.f,4.f);
        hone_eye_ledger expected,got;
        for(unsigned solid=0;solid<2;solid++){
            hone_eye_shader shader=solid?solid_shader:masked_shader;
            hone_eye_begin(&expected,1);hone_eye_begin(&got,1);
            assert(hone_eye_emit_general(&expected,&t,0,1,shader,NULL));
            assert(hone_eye_emit_masked(&got,&t,0,1,shader,NULL,5,solid?0xf800:-1));
            assert(expected.count && !memcmp(&got,&expected,sizeof got));
        }
        hone_eye_begin(&got,1);
        assert(!hone_eye_emit_masked(&got,&t,0,1,masked_shader,NULL,8,-1)&&got.failed);
    }
    return 0;
}
