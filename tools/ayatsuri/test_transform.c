#include "ayatsuri/ayatsuri_transform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef AYATSURI_REQUIRE_BINARY64
_Static_assert(FLT_EVAL_METHOD==0,"strict profile requires no excess precision");
#endif

static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static ayatsuri_pose identity(void){
    const ayatsuri_pose p={{0,0,0},{0,0,0},{1,1,1}};return p;
}
static ayatsuri_transform matrix(void){
    const ayatsuri_transform m={{1,0,0,0,0,1,0,0,0,0,1,0}};return m;
}
static void failure(const ayatsuri_pose *p,const ayatsuri_transform *parent,ayatsuri_status status){
    struct{uint32_t left;ayatsuri_transform value;uint32_t right;} guard,saved;
    memset(&guard,0xa5,sizeof guard);memcpy(&saved,&guard,sizeof saved);
    CHECK(ayatsuri_transform_evaluate(p,parent,&guard.value)==status);
    CHECK(memcmp(&guard,&saved,sizeof guard)==0);
}
static uint32_t random_state=0x61796174;
static uint32_t random_u32(void){
    random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;
    return random_state;
}
static ayatsuri_pose random_pose(void){
    ayatsuri_pose p;
    for(unsigned k=0;k<3;++k){
        p.position[k]=(float)((int)(random_u32()%20001)-10000)/100.0f;
        p.rotation[k]=(float)((int)(random_u32()%16001)-8000)/1000.0f;
        p.scale[k]=(float)(250+random_u32()%4000)/1000.0f;
        if(random_u32()&1)p.scale[k]=-p.scale[k];
    }
    return p;
}
static void values(const float *p,unsigned n){
    putchar('[');
    for(unsigned k=0;k<n;++k){if(k)putchar(',');printf("%.9g",(double)p[k]);}
    putchar(']');
}
static void vector(const ayatsuri_pose *p,const ayatsuri_transform *parent,int *first){
    ayatsuri_transform out;
    memset(&out,0xa5,sizeof out);
    const ayatsuri_status status=ayatsuri_transform_evaluate(p,parent,&out);
    if(!*first)putchar(',');
    *first=0;
    printf("{\"pose\":");
    /* Separate struct arrays, rather than indexing across a C subobject. */
    putchar('[');
    for(unsigned k=0;k<9;++k){
        if(k)putchar(',');
        const float v=k<3?p->position[k]:k<6?p->rotation[k-3]:p->scale[k-6];
        if(isnan(v))printf("null");else printf("%.9g",(double)v);
    }
    printf("],\"parent\":");
    if(parent)values(parent->m,12);else printf("null");
    printf(",\"status\":\"%s\",\"output\":",status==AYATSURI_OK?"OK":status==AYATSURI_INVALID?"INVALID":"RANGE");
    if(status==AYATSURI_OK)values(out.m,12);else printf("null");
    putchar('}');
}
int main(int argc,char **argv){
    ayatsuri_pose p=identity(),saved_pose;
    ayatsuri_transform out,parent=matrix(),saved_parent;
    CHECK(ayatsuri_transform_evaluate(&p,NULL,&out)==AYATSURI_OK);
    CHECK(out.m[0]==1&&out.m[5]==1&&out.m[10]==1&&out.m[3]==0);
    CHECK(signbit(out.m[8]));
    p.position[0]=5;p.position[1]=-6;p.position[2]=7;
    parent.m[0]=2;parent.m[5]=-3;parent.m[10]=0.5f;
    parent.m[3]=10;parent.m[7]=20;parent.m[11]=30;
    memcpy(&saved_pose,&p,sizeof p);memcpy(&saved_parent,&parent,sizeof parent);
    CHECK(ayatsuri_transform_evaluate(&p,&parent,&out)==AYATSURI_OK);
    CHECK(out.m[3]==20&&out.m[7]==38&&out.m[11]==33.5f);
    CHECK(memcmp(&p,&saved_pose,sizeof p)==0&&memcmp(&parent,&saved_parent,sizeof parent)==0);
    CHECK(ayatsuri_transform_evaluate(&p,&parent,&parent)==AYATSURI_OK);
    CHECK(memcmp(&out,&parent,sizeof out)==0);
    p=identity();p.rotation[2]=1.57079632679489661923f;p.scale[0]=-2;p.scale[1]=3;
    CHECK(ayatsuri_transform_evaluate(&p,NULL,&out)==AYATSURI_OK);
    CHECK(fabsf(out.m[1]+3)<1e-6f&&fabsf(out.m[4]+2)<1e-6f);
    failure(NULL,NULL,AYATSURI_INVALID);
    CHECK(ayatsuri_transform_evaluate(&p,NULL,NULL)==AYATSURI_INVALID);
    for(unsigned k=0;k<3;++k){
        p=identity();p.position[k]=NAN;failure(&p,NULL,AYATSURI_INVALID);
        p=identity();p.rotation[k]=INFINITY;failure(&p,NULL,AYATSURI_INVALID);
        p=identity();p.scale[k]=NAN;failure(&p,NULL,AYATSURI_INVALID);
        p=identity();p.scale[k]=-0.0f;failure(&p,NULL,AYATSURI_INVALID);
    }
    p=identity();parent=matrix();
    for(unsigned k=0;k<12;++k){
        parent=matrix();parent.m[k]=INFINITY;failure(&p,&parent,AYATSURI_INVALID);
    }
    parent=matrix();parent.m[3]=FLT_MAX;p.position[0]=0x1p102f;
    CHECK(ayatsuri_transform_evaluate(&p,&parent,&out)==AYATSURI_OK&&out.m[3]==FLT_MAX);
    p.position[0]=0x1p103f;failure(&p,&parent,AYATSURI_RANGE);
    parent.m[3]=-FLT_MAX;p.position[0]=-0x1p102f;
    CHECK(ayatsuri_transform_evaluate(&p,&parent,&out)==AYATSURI_OK&&out.m[3]==-FLT_MAX);
    p.position[0]=-0x1p103f;failure(&p,&parent,AYATSURI_RANGE);
    p=identity();p.scale[0]=2;parent=matrix();parent.m[0]=FLT_MAX;failure(&p,&parent,AYATSURI_RANGE);
    /* In-place failure also leaves the parent byte-for-byte intact. */
    saved_parent=parent;
    CHECK(ayatsuri_transform_evaluate(&p,&parent,&parent)==AYATSURI_RANGE);
    CHECK(memcmp(&parent,&saved_parent,sizeof parent)==0);
    for(unsigned i=0;i<400;++i){
        p=random_pose();ayatsuri_pose pp=random_pose();parent=matrix();
        CHECK(ayatsuri_transform_evaluate(&pp,NULL,&parent)==AYATSURI_OK);
        saved_pose=p;saved_parent=parent;
        CHECK(ayatsuri_transform_evaluate(&p,i&1?&parent:NULL,&out)==AYATSURI_OK);
        CHECK(memcmp(&p,&saved_pose,sizeof p)==0&&memcmp(&parent,&saved_parent,sizeof parent)==0);
        if(i&1){CHECK(ayatsuri_transform_evaluate(&p,&parent,&parent)==AYATSURI_OK);CHECK(memcmp(&parent,&out,sizeof out)==0);}
    }
    if(argc>1&&!strcmp(argv[1],"--json")){
        int first=1;putchar('[');p=identity();vector(&p,NULL,&first);
        p.rotation[0]=-0.0f;p.rotation[1]=-0.0f;vector(&p,NULL,&first);
        p=identity();p.scale[0]=-2;p.scale[1]=0.5f;p.scale[2]=3;vector(&p,NULL,&first);
        p=identity();parent=matrix();parent.m[3]=FLT_MAX;p.position[0]=0x1p102f;vector(&p,&parent,&first);
        p.position[0]=0x1p103f;vector(&p,&parent,&first);
        parent.m[3]=-FLT_MAX;p.position[0]=-0x1p102f;vector(&p,&parent,&first);
        p.position[0]=-0x1p103f;vector(&p,&parent,&first);
        p=identity();p.scale[1]=0;vector(&p,NULL,&first);
        random_state=0x61796174;
        for(unsigned i=0;i<400;++i){
            p=random_pose();const ayatsuri_pose pp=random_pose();
            CHECK(ayatsuri_transform_evaluate(&pp,NULL,&parent)==AYATSURI_OK);
            vector(&p,i&1?&parent:NULL,&first);
        }
        puts("]");
    }
    fprintf(stderr,"AYATSURI_TRANSFORM_PASS %u checks pose=%zu transform=%zu pointer=%zu eval_method=%d\n",checks,sizeof p,sizeof out,sizeof(void*),FLT_EVAL_METHOD);
    return 0;
}
