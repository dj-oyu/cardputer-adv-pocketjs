#include "ayatsuri/ayatsuri_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned k=0;k<4;k++)p[k]=(uint8_t)(v>>(k*8));}
static void putf(uint8_t *p,float v){uint32_t bits;memcpy(&bits,&v,4);put32(p,bits);}
static uint8_t *load(const char *name,size_t *bytes){FILE *f=fopen(name,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *p=malloc((size_t)n+1);assert(p);*bytes=(size_t)n;assert(fread(p+1,1,*bytes,f)==*bytes);assert(!fclose(f));return p;}
static void reject(const uint8_t *p,size_t bytes){ayatsuri_model s,out;memset(&s,0xa5,sizeof s);out=s;
    assert(ayatsuri_model_open(p,bytes,&out)==AYATSURI_MODEL_INVALID);assert(!memcmp(&out,&s,sizeof s));}
static ayatsuri_model_packet packet(ayatsuri_model_instance *i,unsigned id){uint32_t cursor=0;ayatsuri_model_packet p;ayatsuri_model_status s;
    while((s=ayatsuri_model_instance_next(i,i,1,&cursor,&p))==AYATSURI_MODEL_OK)if(p.node_id==id)return p;
    assert(s==AYATSURI_MODEL_OK);return p;}
static float number(const uint8_t *p,unsigned at){float v;memcpy(&v,p+at,4);return v;}
static unsigned word(const uint8_t *p,unsigned at){uint16_t v;memcpy(&v,p+at,2);return v;}
static void plane(ayatsuri_model_instance *i,unsigned id,const float values[5],unsigned color){
    ayatsuri_model_packet p=packet(i,id);assert(p.bytes==264&&p.kind==1);
    assert(word(p.storage,240)==4&&word(p.storage,234)==color);
    for(unsigned k=0;k<5;k++)assert(number(p.storage,196+4*k)==values[k]);
    assert(number(p.storage,216)==0&&number(p.storage,224)==0&&number(p.storage,228)==1);
}
int main(int argc,char **argv){
    assert(argc==3);size_t bytes,plain_bytes;uint8_t *a=load(argv[1],&bytes),*p=a+1,*b=load(argv[2],&plain_bytes);
    ayatsuri_model model,plain;assert(ayatsuri_model_open(p,bytes,&model)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_open(b+1,plain_bytes,&plain)==AYATSURI_MODEL_OK);
    assert(model.section[7].count==2&&plain.section[7].count==0&&model.node_count==4);
    assert(model.slot_bytes==plain.slot_bytes&&ayatsuri_model_instance_bytes(&model)==ayatsuri_model_instance_bytes(&plain));
    assert(sizeof(ayatsuri_model_section)==8);
    typedef struct {const uint8_t *data;uint32_t bytes;uint16_t node_count,packet_count;uint32_t slot_bytes;ayatsuri_model_section section[7];} old_model_layout;
    assert(sizeof(ayatsuri_model)==sizeof(old_model_layout)+8);
    uint8_t *mut=malloc(bytes),*snapshot=malloc(bytes);assert(mut&&snapshot);memcpy(snapshot,p,bytes);
    const uint32_t start=model.section[7].offset,dir=32+7*20;
    for(size_t n=0;n<bytes;n++)reject(p,n);
    const unsigned bad32[]={dir+4,dir+8,dir+12,dir+16};
    for(unsigned k=0;k<4;k++){memcpy(mut,p,bytes);put32(mut+bad32[k],UINT32_MAX);reject(mut,bytes);}
    memcpy(mut,p,bytes);mut[dir+2]=0;reject(mut,bytes);
    memcpy(mut,p,bytes);put32(mut+dir+12,0);put32(mut+dir+8,0);reject(mut,bytes);
    for(unsigned id=0;id<5;id++)if(id!=1&&id!=3){memcpy(mut,p,bytes);mut[start]=(uint8_t)id;reject(mut,bytes);}
    memcpy(mut,p,bytes);mut[start+32]=1;reject(mut,bytes);
    for(unsigned at=2;at<=3;at++){memcpy(mut,p,bytes);mut[start+at]=5;reject(mut,bytes);}
    memcpy(mut,p,bytes);put32(mut+start+28,1);reject(mut,bytes);
    memcpy(mut,p,bytes);put32(mut+start+24,0x1000000);reject(mut,bytes);
    const float bad_normal[]={0,1.001f,NAN,INFINITY};
    for(unsigned k=0;k<4;k++){memcpy(mut,p,bytes);putf(mut+start+8,bad_normal[k]);reject(mut,bytes);}
    const float bad_offset[]={-2.001f,2.001f,NAN,INFINITY};
    const float bad_feather[]={-.1f,9e-5f,2.001f,NAN,INFINITY};
    for(unsigned k=0;k<4;k++){memcpy(mut,p,bytes);putf(mut+start+16,bad_offset[k]);reject(mut,bytes);}
    for(unsigned k=0;k<5;k++){memcpy(mut,p,bytes);putf(mut+start+20,bad_feather[k]);reject(mut,bytes);}
    const float feathers[]={0,1e-4f,2};
    for(unsigned k=0;k<3;k++){memcpy(mut,p,bytes);putf(mut+start+20,feathers[k]);ayatsuri_model out;assert(ayatsuri_model_open(mut,bytes,&out)==AYATSURI_MODEL_OK);}
    memcpy(mut,p,bytes);putf(mut+start+8,1.00004f);ayatsuri_model near_unit;
    assert(ayatsuri_model_open(mut,bytes,&near_unit)==AYATSURI_MODEL_OK);
    putf(mut+start+8,1.00006f);reject(mut,bytes);
    /* Wire rows need not be node ordered. Duplicates, not order, are rejected. */
    memcpy(mut,p,bytes);memcpy(mut+start,p+start+32,32);memcpy(mut+start+32,p+start,32);
    ayatsuri_model reversed;assert(ayatsuri_model_open(mut,bytes,&reversed)==AYATSURI_MODEL_OK);
    const size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes),*old_arena=malloc(arena_bytes),*other_arena=malloc(arena_bytes);
    assert(arena&&old_arena&&other_arena);ayatsuri_model_instance i,old,other;
    assert(ayatsuri_model_instance_init(&model,arena,arena_bytes,&i,1,&i)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&plain,old_arena,arena_bytes,&old,1,&old)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&model,other_arena,arena_bytes,&other,1,&other)==AYATSURI_MODEL_OK);
    float camera[9]={0,0,400,0,0,0,1.3f,120,78},light[6]={0,0,1,1,1,1};
    assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&old,&old,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&other,&other,1,camera,light)==AYATSURI_MODEL_OK);
    const float first[5]={0,1,0,.2f,.1f},second[5]={1,0,0,-.25f,1e-4f};plane(&i,1,first,0x07e0);plane(&i,3,second,0xf800);
    ayatsuri_model_packet np=packet(&i,1),op=packet(&old,1);assert(!memcmp(np.storage+96,op.storage+96,100));
    assert(word(np.storage,232)==word(op.storage,232)&&word(np.storage,236)==word(op.storage,236)&&word(np.storage,238)==word(op.storage,238));
    float poses[36];uint8_t dirty[4]={0};uint32_t colors[4]={0};double opacity[4]={0};
    for(unsigned k=0;k<4;k++)assert(ayatsuri_model_instance_read_pose(&i,&i,1,(uint16_t)k,poses+k*9)==AYATSURI_MODEL_OK);
    dirty[1]=AYATSURI_TARGET_ROTATION|AYATSURI_TARGET_SCALE|AYATSURI_TARGET_COLOR;poses[12]=.2f;poses[15]=1.2f;colors[1]=0xff0000;
    ayatsuri_model_targets targets={dirty,poses,colors,opacity,4};
    assert(ayatsuri_model_instance_targets(&i,&i,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);plane(&i,1,first,0x07e0);
    np=packet(&i,1);assert(word(np.storage,232)==0xf800);plane(&other,1,first,0x07e0);
    light[0]=1;light[2]=0;assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);plane(&i,1,first,0x07e0);
    assert(!memcmp(p,snapshot,bytes));
    assert(ayatsuri_model_instance_dispose(&i,&i,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&old,&old,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&other,&other,1)==AYATSURI_MODEL_OK);
    printf("AYNM_REGION_PASS rows=2 bytes=%zu oldBytes=%zu arena=%zu immutable atomic malformed truncations=%zu packet geometry material isolation\n",bytes,plain_bytes,arena_bytes,bytes);
    free(other_arena);free(old_arena);free(arena);free(snapshot);free(mut);free(b);free(a);return 0;
}
