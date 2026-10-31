#include "ayatsuri_visibility.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

#ifdef AYATSURI_VISIBILITY_ALLOC_TEST
static bool measure_allocations;
static unsigned allocations;
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);void __real_free(void *);
void *__wrap_malloc(size_t n){if(measure_allocations)++allocations;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s){if(measure_allocations)++allocations;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n){if(measure_allocations)++allocations;return __real_realloc(p,n);}
void __wrap_free(void *p){if(measure_allocations)++allocations;__real_free(p);}
#endif

typedef struct {
    float xy[64],depth[32];uint16_t indices[192],out[200];uint32_t counts[3];
} fixture;
static fixture f;
static void reset(void){
    memset(&f,0,sizeof f);
    for(unsigned i=0;i<200;i++)f.out[i]=0xabcd;
    f.counts[0]=0x12345678;f.counts[1]=0xabcdef01;f.counts[2]=0x87654321;
    for(unsigned i=0;i<3;i++){
        const unsigned v=3*i;f.xy[2*v]=0;f.xy[2*v+1]=0;
        f.xy[2*(v+1)]=1;f.xy[2*(v+1)+1]=0;
        f.xy[2*(v+2)]=0;f.xy[2*(v+2)+1]=1;
        for(unsigned k=0;k<3;k++)f.indices[3*i+k]=(uint16_t)(v+k);
    }
}
static void failed(ayatsuri_visibility_status expected,const float *xy,const float *depth,
                   size_t vertices,const uint16_t *indices,size_t triangles,
                   ayatsuri_visibility_order order,uint16_t *out,size_t capacity,uint32_t *counts){
    const fixture before=f;
    CHECK(ayatsuri_visibility_select(xy,depth,vertices,indices,triangles,order,out,capacity,counts)==expected);
    CHECK(memcmp(&f,&before,sizeof f)==0);
}
static void ordinary_failure(ayatsuri_visibility_status expected){
    failed(expected,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
}
static void success_counts(unsigned front,unsigned back,unsigned degenerate){
    CHECK(f.counts[0]==front&&f.counts[1]==back&&f.counts[2]==degenerate);
    for(unsigned i=front*3;i<200;i++)CHECK(f.out[i]==0xabcd);
}
static void classification(void){
    reset();f.indices[3]=3;f.indices[4]=5;f.indices[5]=4;f.indices[8]=7;
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(1,1,1);CHECK(memcmp(f.out,f.indices,6)==0);
    reset();f.indices[1]=2;f.indices[2]=1;f.indices[4]=5;f.indices[5]=4;f.indices[8]=7;
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_CENTROID_DEPTH,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(0,2,1);
    reset();f.xy[5]=1e-5f;
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,3,f.indices,1,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(0,0,1);
    f.xy[5]=nextafterf(1e-5f,INFINITY);
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,3,f.indices,1,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(1,0,0);
    reset();f.xy[5]=-nextafterf(1e-5f,INFINITY);
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,3,f.indices,1,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(0,1,0);
    reset();f.indices[0]=f.indices[1]=f.indices[2]=0;
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,1,f.indices,1,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    success_counts(0,0,1);
}
static void ordering(void){
    reset();for(unsigned k=0;k<3;k++){f.depth[k]=-1;f.depth[k+3]=2;f.depth[k+6]=-1;}
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_CENTROID_DEPTH,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    CHECK(f.out[0]==0&&f.out[3]==6&&f.out[6]==3);success_counts(3,0,0);
    /* Equal keys follow source triangle ID, even when authored order reverses. */
    for(unsigned k=0;k<3;k++){f.indices[k]=(uint16_t)(6+k);f.indices[k+6]=(uint16_t)k;}
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_CENTROID_DEPTH,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    CHECK(f.out[0]==6&&f.out[3]==0&&f.out[6]==3);
    for(unsigned k=0;k<3;k++){f.depth[k]=-FLT_MAX;f.depth[k+3]=0;f.depth[k+6]=FLT_MAX;}
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_CENTROID_DEPTH,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    CHECK(f.out[0]==0&&f.out[3]==3&&f.out[6]==6);
    CHECK(ayatsuri_visibility_select(f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts)==AYATSURI_VISIBILITY_OK);
    CHECK(memcmp(f.out,f.indices,18)==0);
}
static void failures(void){
    reset();f.indices[8]=9;ordinary_failure(AYATSURI_VISIBILITY_RANGE);
    reset();f.xy[17]=NAN;ordinary_failure(AYATSURI_VISIBILITY_INVALID);
    reset();f.depth[8]=INFINITY;ordinary_failure(AYATSURI_VISIBILITY_INVALID);
    reset();f.xy[17]=4097;ordinary_failure(AYATSURI_VISIBILITY_INVALID);
    reset();f.xy[17]=-INFINITY;ordinary_failure(AYATSURI_VISIBILITY_INVALID);
    reset();
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,8,f.counts);
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,33,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,9,f.indices,65,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,9,f.indices,0,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,SIZE_MAX,f.counts);
    failed(AYATSURI_VISIBILITY_RANGE,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,SIZE_MAX/2,f.counts);
    failed(AYATSURI_VISIBILITY_INVALID,f.xy,f.depth,9,f.indices,3,(ayatsuri_visibility_order)2,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_INVALID,NULL,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_INVALID,(const float *)((const char *)f.xy+1),f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,f.counts);
    failed(AYATSURI_VISIBILITY_INVALID,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,(uint16_t *)((char *)f.out+1),199,f.counts);
    failed(AYATSURI_VISIBILITY_INVALID,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,(uint32_t *)((char *)f.counts+1));
    void *inputs[3]={f.xy,f.depth,f.indices};
    for(unsigned i=0;i<3;i++){
        failed(AYATSURI_VISIBILITY_OVERLAP,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,inputs[i],9,f.counts);
        failed(AYATSURI_VISIBILITY_OVERLAP,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,inputs[i]);
    }
    failed(AYATSURI_VISIBILITY_OVERLAP,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,200,(uint32_t *)f.out);
    /* Capacity, not only the visible prefix, defines the writable span. */
    failed(AYATSURI_VISIBILITY_OVERLAP,f.xy,f.depth,9,f.indices,3,AYATSURI_VISIBILITY_AUTHORED,f.out,202,f.counts);
}
static void independent_replay(void){
    reset();fixture first=f,second=f;
    for(unsigned i=0;i<64;i++)for(unsigned k=0;k<3;k++)first.indices[3*i+k]=(uint16_t)k;
    first.xy[0]=-4096;first.xy[1]=-4096;first.xy[2]=4096;first.xy[3]=-4096;first.xy[4]=0;first.xy[5]=4096;
#ifdef AYATSURI_VISIBILITY_ALLOC_TEST
    allocations=0;measure_allocations=true;
#endif
    for(unsigned replay=0;replay<100;replay++){
        CHECK(ayatsuri_visibility_select(first.xy,first.depth,32,first.indices,64,AYATSURI_VISIBILITY_CENTROID_DEPTH,first.out,200,first.counts)==AYATSURI_VISIBILITY_OK);
        CHECK(first.counts[0]==64&&first.counts[1]==0&&first.counts[2]==0);
        CHECK(memcmp(first.out,first.indices,384)==0);
        CHECK(ayatsuri_visibility_select(second.xy,second.depth,9,second.indices,3,AYATSURI_VISIBILITY_AUTHORED,second.out,200,second.counts)==AYATSURI_VISIBILITY_OK);
        CHECK(memcmp(second.out,second.indices,18)==0&&second.counts[0]==3);
    }
#ifdef AYATSURI_VISIBILITY_ALLOC_TEST
    measure_allocations=false;CHECK(allocations==0);
#endif
    for(unsigned i=192;i<200;i++)CHECK(first.out[i]==0xabcd);
    CHECK(memcmp(&f.xy,&second.xy,sizeof f.xy)==0);
}
int main(void){
    classification();ordering();failures();independent_replay();
    puts("PASS visibility classification/order/atomicity/alignment/overlap/capacity/replay");
#ifdef AYATSURI_VISIBILITY_ALLOC_TEST
    puts("PASS visibility allocator calls = 0 (200 warm selections)");
#endif
    return 0;
}
