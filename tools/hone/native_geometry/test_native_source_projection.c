#include "native_source_projection.h"
#include <assert.h>

typedef struct { unsigned count,original[4];float xyz[9];bool inside; } observation;
static bool observe(void *ctx,unsigned i,const uint8_t face[4],const float *local,const float xyz[9],bool inside) {
    observation *o=ctx;
    assert(face[3]==4 && local[0]==0 && o->count<4);
    o->original[o->count++]=i;memcpy(o->xyz,xyz,sizeof o->xyz);o->inside=inside;
    return true;
}
static uint32_t seed=17;
static float random_coord(void) {
    seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
    return (float)(seed>>8)*0x1p-13f;
}
int main(void) {
    float camera[9]={0,-4,2,0,0,2,30,120,84};
    hone_source_camera basis;
    assert(hone_source_camera_z_up(camera,&basis));
    ayatsuri_transform world={{1,0,0,0,0,1,0,0,0,0,1,0}};
    float p[12];
    assert(hone_source_projection(&basis,&world,p));
    float local[]={0,0,2,2,0,2,0,0,3};
    float result[3][3];
    const float expected[3][3]={{120,84,0},{180,84,0},{120,54,0}};
    assert(hone_source_project_vertices(p,local,3,result) && !memcmp(result,expected,sizeof result));
    const uint8_t faces[][4]={{0,1,2,4},{0,1,2,4},{0,0,1,4},{0,2,1,4}};
    assert(hone_source_faces_valid(faces,4,3,5));
    assert(!hone_source_faces_valid(faces,4,2,5));
    assert(!hone_source_faces_valid(faces,4,3,4));
    observation o={0};
    assert(hone_source_emit_faces(faces,4,AYATSURI_VISIBILITY_NEGATIVE,result,local,240,135,observe,&o));
    assert(o.count==2 && o.original[0]==0 && o.original[1]==1 && o.inside);
    memset(&o,0,sizeof o);
    assert(hone_source_emit_faces(faces,4,AYATSURI_VISIBILITY_POSITIVE,result,local,100,100,observe,&o));
    assert(o.count==1 && o.original[0]==3 && !o.inside);
    local[0]=NAN;
    assert(!hone_source_project_vertices(p,local,3,result));
    camera[1]=0;
    assert(!hone_source_camera_z_up(camera,&basis));
    camera[2]=5;
    assert(hone_source_camera_z_up(camera,&basis) && basis.axis[0][0]==1);
    world.m[0]=0;
    assert(!hone_source_projection(&basis,&world,p));
    /* Compare the shared renderer boundary, including nearly collinear and
     * exactly collinear points and both stable winding orientations. */
    for(unsigned i=0;i<100000;i++){
        float xy[6];for(unsigned k=0;k<6;k++)xy[k]=random_coord();
        if(i%2==0){xy[4]=xy[0]+(xy[2]-xy[0])*.5f;xy[5]=xy[1]+(xy[3]-xy[1])*.5f;}
        if(i%3==0)xy[5]=nextafterf(xy[5],INFINITY);
        if(i%5==0){xy[4]=xy[2];xy[5]=xy[3];}
        float depth[3]={0};
        uint16_t indices[]={0,1,2},selected[3];
        for(unsigned faces=1;faces<=3;faces++){
            uint32_t count[3];
            assert(ayatsuri_visibility_select_faces(xy,depth,3,indices,1,AYATSURI_VISIBILITY_AUTHORED,
                (ayatsuri_visibility_faces)faces,selected,3,count)==AYATSURI_VISIBILITY_OK);
            assert(count[0]==!!(faces&hone_source_winding(xy,xy+2,xy+4)));
        }
    }
    return 0;
}
