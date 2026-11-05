#include "ayatsuri_skin.h"
#include <assert.h>
#include <stdio.h>

static const ayatsuri_transform identity={{1,0,0,0,0,1,0,0,0,0,1,0}};
static void close_to(double a,double b){assert(fabs(a-b)<=2e-5*fmax(1,fabs(b)));}
static ayatsuri_skin_vertex vertex(float x,float y,float z){
    ayatsuri_skin_vertex v={{x,y,z},{0,0,1},{0,0,0,0},{65535,0,0,0}};return v;
}
static void unchanged(const void *a,const void *b,size_t n){assert(memcmp(a,b,n)==0);}

static void bind_and_hierarchy(void){
    /* Translated root and rotated child: inverse bind includes translation. */
    ayatsuri_transform global[2]={{{1,0,0,3,0,1,0,-2,0,0,1,1}},
                                 {{0,-1,0,5,1,0,0,-2,0,0,1,1}}};
    ayatsuri_transform bind[2]={{{1,0,0,-3,0,1,0,2,0,0,1,-1}},
                               {{0,1,0,2,-1,0,0,5,0,0,1,-1}}},palette[2];
    assert(ayatsuri_skin_palette(global,bind,2,palette)==AYATSURI_OK);
    for(unsigned j=0;j<2;++j)for(unsigned k=0;k<12;++k)close_to(palette[j].m[k],identity.m[k]);
    ayatsuri_skin_vertex input[3]={vertex(0,0,0),vertex(2,3,4),vertex(-2,1,7)};
    input[1].joints[0]=1;input[2].joints[1]=1;
    input[2].weights[0]=21845;input[2].weights[1]=43690;
    ayatsuri_skin_result out[3];
    assert(ayatsuri_skin_evaluate(palette,2,input,3,out)==AYATSURI_OK);
    for(unsigned v=0;v<3;++v)for(unsigned k=0;k<3;++k)close_to(out[v].position[k],input[v].position[k]);
    /* Move only the child away from bind pose; mixed vertex moves 2/3 as far. */
    global[1].m[3]+=6;
    assert(ayatsuri_skin_palette(global,bind,2,palette)==AYATSURI_OK);
    assert(ayatsuri_skin_evaluate(palette,2,input,3,out)==AYATSURI_OK);
    close_to(out[0].position[0],0);close_to(out[1].position[0],8);close_to(out[2].position[0],2);
    /* Feed globals from the existing hierarchy evaluator, not a skin clock. */
    ayatsuri_pose root={{3,-2,1},{0,0,0},{1,1,1}},child={{2,0,0},{0,0,0},{1,1,1}};
    assert(ayatsuri_transform_evaluate(&root,NULL,global)==AYATSURI_OK);
    assert(ayatsuri_transform_evaluate(&child,global,global+1)==AYATSURI_OK);
    close_to(global[1].m[3],5);close_to(global[1].m[7],-2);
}
static void normals_and_four_weights(void){
    ayatsuri_transform p[4]={identity,identity,identity,identity};
    ayatsuri_skin_vertex v=vertex(1,2,3);v.normal[0]=1;v.normal[1]=1;v.normal[2]=0;
    p[0].m[0]=2;p[0].m[5]=3;p[0].m[10]=4;
    ayatsuri_skin_result out;
    assert(ayatsuri_skin_evaluate(p,4,&v,1,&out)==AYATSURI_OK);
    close_to(out.position[0],2);close_to(out.position[1],6);close_to(out.position[2],12);
    close_to(out.normal[0],3/sqrt(13));close_to(out.normal[1],2/sqrt(13));
    p[0].m[0]=-2;
    assert(ayatsuri_skin_evaluate(p,4,&v,1,&out)==AYATSURI_OK);
    close_to(out.normal[0],-3/sqrt(13));close_to(out.normal[1],2/sqrt(13));
    /* A shear checks the off-diagonal inverse-transpose convention. */
    p[0]=identity;p[0].m[1]=2;v.normal[0]=1;v.normal[1]=0;
    assert(ayatsuri_skin_evaluate(p,4,&v,1,&out)==AYATSURI_OK);
    close_to(out.normal[0],1/sqrt(5));close_to(out.normal[1],-2/sqrt(5));
    for(unsigned j=0;j<4;++j){p[j]=identity;p[j].m[3]=(float)j;v.joints[j]=(uint16_t)j;v.weights[j]=16384;}
    v.weights[3]=16383;
    assert(ayatsuri_skin_evaluate(p,4,&v,1,&out)==AYATSURI_OK);
    close_to(out.position[0],1+(16384.0+2*16384+3*16383)/65535);
}
static void numerical_boundaries_and_overlap(void){
    ayatsuri_transform p[2]={identity,identity};
    ayatsuri_skin_vertex v=vertex(1,2,3);
    ayatsuri_skin_result out;
    /* Subnormal inputs are nonzero normals, not a reason for a fallback. */
    const float tiny=0x1p-149f;
    assert(tiny>0);v.normal[0]=tiny;v.normal[1]=-tiny;v.normal[2]=tiny;
    assert(ayatsuri_skin_evaluate(p,2,&v,1,&out)==AYATSURI_OK);
    close_to(out.normal[0],1/sqrt(3));close_to(out.normal[1],-1/sqrt(3));
    close_to(out.normal[2],1/sqrt(3));
    /* No arbitrary determinant epsilon: this inverse is representable. */
    p[0].m[0]=0x1p-100f;v.normal[0]=1;v.normal[1]=0;v.normal[2]=0;
    assert(ayatsuri_skin_evaluate(p,2,&v,1,&out)==AYATSURI_OK);
    assert(out.position[0]==0x1p-100f);close_to(out.normal[0],1);
    close_to(out.normal[1],0);close_to(out.normal[2],0);p[0]=identity;

    /* Every overlapping span is aligned and fully inside valid backing
     * storage. Rejection must precede dereferencing any aliased output. */
    union storage {
        ayatsuri_transform transforms[3];
        ayatsuri_skin_vertex vertices[3];
        ayatsuri_skin_result results[6];
    } backing;
    unsigned char saved[sizeof backing];
    memset(&backing,0,sizeof backing);
    for(unsigned j=0;j<3;++j)backing.transforms[j]=identity;
    memcpy(saved,&backing,sizeof backing);
    assert(ayatsuri_skin_palette(p,backing.transforms,2,backing.transforms)==AYATSURI_INVALID);
    unchanged(saved,&backing,sizeof backing);
    assert(ayatsuri_skin_palette(p,backing.transforms,2,backing.transforms+1)==AYATSURI_INVALID);
    unchanged(saved,&backing,sizeof backing);
    assert(ayatsuri_skin_palette(backing.transforms,p,2,backing.transforms+1)==AYATSURI_INVALID);
    unchanged(saved,&backing,sizeof backing);
    memset(&backing,0,sizeof backing);
    for(unsigned j=0;j<3;++j)backing.vertices[j]=vertex(1,2,3);
    memcpy(saved,&backing,sizeof backing);
    assert(ayatsuri_skin_evaluate(p,2,backing.vertices,2,backing.results+1)==AYATSURI_INVALID);
    unchanged(saved,&backing,sizeof backing);
    assert(ayatsuri_skin_evaluate(p,2,backing.vertices+1,2,backing.results)==AYATSURI_INVALID);
    unchanged(saved,&backing,sizeof backing);
}
static void failures(void){
    ayatsuri_transform p[2]={identity,identity},bind[2]={identity,identity},palette[2],saved_palette[2];
    memset(palette,0xa5,sizeof palette);memcpy(saved_palette,palette,sizeof palette);
    p[1].m[0]=NAN;
    assert(ayatsuri_skin_palette(p,bind,2,palette)==AYATSURI_INVALID);
    unchanged(palette,saved_palette,sizeof palette);
    p[1]=identity;p[1].m[0]=FLT_MAX;bind[1].m[0]=2;
    assert(ayatsuri_skin_palette(p,bind,2,palette)==AYATSURI_RANGE);
    unchanged(palette,saved_palette,sizeof palette);
    assert(ayatsuri_skin_palette(p,bind,2,p)==AYATSURI_INVALID);
    assert(ayatsuri_skin_palette(p,bind,0,palette)==AYATSURI_INVALID);
    assert(ayatsuri_skin_palette(NULL,bind,2,palette)==AYATSURI_INVALID);
    assert(ayatsuri_skin_palette(p,bind,65537,palette)==AYATSURI_INVALID);
    p[1]=identity;
    ayatsuri_skin_vertex v[2]={vertex(1,2,3),vertex(4,5,6)};
    ayatsuri_skin_result out[2],saved[2];memset(out,0xa5,sizeof out);memcpy(saved,out,sizeof out);
#define REJECT(expected) do { assert(ayatsuri_skin_evaluate(p,2,v,2,out)==(expected)); unchanged(out,saved,sizeof out); } while(0)
    v[1].weights[0]=0;REJECT(AYATSURI_INVALID);
    v[1].weights[0]=65534;REJECT(AYATSURI_INVALID);
    v[1].weights[0]=65535;v[1].weights[1]=1;REJECT(AYATSURI_INVALID);
    v[1].weights[1]=0;v[1].joints[3]=2;REJECT(AYATSURI_INVALID);
    v[1].joints[3]=0;v[1].normal[2]=0;REJECT(AYATSURI_INVALID);
    v[1].normal[2]=INFINITY;REJECT(AYATSURI_INVALID);
    v[1].normal[2]=1;v[1].position[0]=NAN;REJECT(AYATSURI_INVALID);
    v[1].position[0]=FLT_MAX;p[0].m[0]=2;REJECT(AYATSURI_RANGE);
    v[1]=vertex(4,5,6);p[0]=identity;p[1].m[0]=NAN;REJECT(AYATSURI_INVALID);
    p[1]=identity;p[1].m[0]=0;v[1].joints[0]=1;REJECT(AYATSURI_RANGE);
    /* Both bones are invertible, but their 1:2 weighted blend is singular. */
    p[1].m[0]=-.5f;v[1].joints[0]=0;v[1].joints[1]=1;
    v[1].weights[0]=21845;v[1].weights[1]=43690;REJECT(AYATSURI_RANGE);
#undef REJECT
    assert(ayatsuri_skin_evaluate(p,2,v,0,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,0,v,2,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(NULL,2,v,2,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,NULL,2,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,v,2,NULL)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,65537,v,2,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,v,SIZE_MAX,out)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,v,2,(ayatsuri_skin_result *)(void *)v)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,v,2,(ayatsuri_skin_result *)(void *)p)==AYATSURI_INVALID);
    assert(ayatsuri_skin_evaluate(p,2,v,1,(ayatsuri_skin_result *)(void *)(v+1))==AYATSURI_OK);
    /* Recovery after all failures has no hidden dirty or poison state. */
    v[0]=vertex(1,2,3);v[1]=vertex(4,5,6);p[1]=identity;
    assert(ayatsuri_skin_evaluate(p,2,v,2,out)==AYATSURI_OK);
}
static void seek_and_independent_geometry(void){
    ayatsuri_transform p[2]={identity,identity};
    ayatsuri_skin_vertex v=vertex(1,2,3);v.normal[0]=0;v.normal[1]=0;v.normal[2]=1;
    v.joints[1]=1;v.weights[0]=21845;v.weights[1]=43690;
    ayatsuri_skin_result first[120],out;
    for(unsigned visit=0;visit<240;++visit){
        unsigned frame=visit<120?visit:239-visit;
        double t=frame*.07;
        p[1].m[0]=(float)(1+.2*sin(t));p[1].m[1]=(float)(.3*cos(t));
        p[1].m[4]=(float)(.1*sin(t));p[1].m[5]=(float)(1+.1*cos(t));
        p[1].m[8]=(float)(.2*cos(t));p[1].m[9]=(float)(.15*sin(t));
        p[1].m[3]=(float)(4*sin(t));p[1].m[7]=(float)(2*cos(t));
        assert(ayatsuri_skin_evaluate(p,2,&v,1,&out)==AYATSURI_OK);
        /* Independent oracle: blend already-transformed points, not matrices. */
        for(unsigned r=0;r<3;++r){
            double transformed=p[1].m[4*r]*(double)v.position[0]+p[1].m[4*r+1]*(double)v.position[1]+
                               p[1].m[4*r+2]*(double)v.position[2]+p[1].m[4*r+3];
            close_to(out.position[r],(v.position[r]+2*transformed)/3);
        }
        /* Transform plane tangents; their cross product independently gives
         * the normal for this positive-determinant, constant-weight surface. */
        double u[3],w[3],n[3];
        for(unsigned r=0;r<3;++r){u[r]=((r==0)+2*(double)p[1].m[4*r])/3;w[r]=((r==1)+2*(double)p[1].m[4*r+1])/3;}
        n[0]=u[1]*w[2]-u[2]*w[1];n[1]=u[2]*w[0]-u[0]*w[2];n[2]=u[0]*w[1]-u[1]*w[0];
        double length=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for(unsigned r=0;r<3;++r)close_to(out.normal[r],n[r]/length);
        if(visit<120)first[frame]=out;else unchanged(&first[frame],&out,sizeof out);
        ayatsuri_skin_result paused;
        assert(ayatsuri_skin_evaluate(p,2,&v,1,&paused)==AYATSURI_OK);
        unchanged(&paused,&out,sizeof out);
    }
}
int main(void){
    bind_and_hierarchy();normals_and_four_weights();numerical_boundaries_and_overlap();
    failures();seek_and_independent_geometry();
    printf("SKIN_CORE_OK: bind/hierarchy, four weights, inverse-transpose, atomic failures, 240 seek + pause evaluations; vertex=%zu result=%zu palette/joint=%zu bytes\n",
           sizeof(ayatsuri_skin_vertex),sizeof(ayatsuri_skin_result),sizeof(ayatsuri_transform));
    return 0;
}
