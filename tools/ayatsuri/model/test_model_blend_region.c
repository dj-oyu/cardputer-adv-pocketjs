#include "ayatsuri/ayatsuri_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *load(const char *path,size_t *bytes){FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *p=malloc((size_t)n+1);assert(p);*bytes=(size_t)n;assert(fread(p+1,1,*bytes,f)==*bytes);assert(!fclose(f));return p;}
static void putf(uint8_t *p,float f){memcpy(p,&f,4);}
static float getf(const uint8_t *p){float f;memcpy(&f,p,4);return f;}
static unsigned get16(const uint8_t *p){uint16_t v;memcpy(&v,p,2);return v;}
static void reject(uint8_t *p,size_t n){ayatsuri_model out,s;memset(&s,0xa5,sizeof s);out=s;
    assert(ayatsuri_model_open(p,n,&out)==AYATSURI_MODEL_INVALID&&!memcmp(&out,&s,sizeof s));}
static ayatsuri_model_packet packet(ayatsuri_model_instance *i,unsigned id){uint32_t cursor=id;ayatsuri_model_packet p;
    assert(ayatsuri_model_instance_next(i,i,1,&cursor,&p)==AYATSURI_MODEL_OK&&p.node_id==id);return p;}
static void check_plane(ayatsuri_model_instance *i,unsigned id,const float camera[9],const double normal[3],double offset){
    ayatsuri_model_packet p=packet(i,id);assert(p.kind==3);const uint8_t *mask=p.storage+p.bytes-24;
    double fx=(double)camera[0]-camera[3],fy=(double)camera[1]-camera[4],fz=(double)camera[2]-camera[5],len=hypot(hypot(fx,fy),fz);
    fx/=len;fy/=len;fz/=len;double rx=fz,rz=-fx;len=hypot(rx,rz);if(len<1e-6){rx=1;rz=0;len=1;}rx/=len;rz/=len;
    const float basis[9]={(float)rx,0,(float)rz,(float)(fy*rz),(float)(fz*rx-fx*rz),(float)(-fy*rx),(float)fx,(float)fy,(float)fz};
    for(unsigned k=0;k<12;k++){
        const double q[3]={(int)k-6,(int)(k*3%11)-5,(int)(k*7%13)-6};double world[3],screen[3];
        for(unsigned r=0;r<3;r++)world[r]=getf(p.storage+4*(4*r))*q[0]+getf(p.storage+4*(4*r+1))*q[1]+getf(p.storage+4*(4*r+2))*q[2]+getf(p.storage+4*(4*r+3))-camera[3+r];
        for(unsigned r=0;r<3;r++)screen[r]=(basis[3*r]*world[0]+basis[3*r+1]*world[1]+basis[3*r+2]*world[2])*(r<2?camera[6]:1)+(r<2?camera[7+r]:0);
        const double actual=getf(mask)*screen[0]+getf(mask+4)*screen[1]+getf(mask+8)*screen[2]+getf(mask+12),expected=normal[0]*q[0]+normal[1]*q[1]+normal[2]*q[2]-offset;
        assert(fabs(actual-expected)<2e-4*(1+fabs(expected)));
    }
    assert(get16(mask+22)==0);
}
int main(int argc,char **argv){
    assert(argc==3);size_t size,plain_size;uint8_t *raw=load(argv[1],&size),*data=raw+1,*plain_raw=load(argv[2],&plain_size);
    ayatsuri_model model,plain;assert(ayatsuri_model_open(data,size,&model)==AYATSURI_MODEL_OK);assert(ayatsuri_model_open(plain_raw+1,plain_size,&plain)==AYATSURI_MODEL_OK);
    assert(model.node_count==4&&model.section[7].count==2&&model.slot_bytes==plain.slot_bytes+48);
    size_t bytes=ayatsuri_model_instance_bytes(&model),old_bytes=ayatsuri_model_instance_bytes(&plain);assert(bytes==old_bytes+48);
    uint8_t *mut=malloc(size),*source_copy=malloc(size);assert(mut&&source_copy);memcpy(source_copy,data,size);uint32_t at=model.section[7].offset;
    for(size_t n=0;n<size;n++)reject(data,n);
    const unsigned badbytes[]={0,2,3,28};for(unsigned k=0;k<4;k++){memcpy(mut,data,size);mut[at+badbytes[k]]=k==1?4:1;reject(mut,size);}
    memcpy(mut,data,size);mut[at+32]=2;reject(mut,size);
    for(unsigned axis=0;axis<3;axis++){memcpy(mut,data,size);putf(mut+at+4+axis*4,NAN);reject(mut,size);}
    const float offsets[]={-4097,4097,INFINITY},feathers[]={-1,9e-5f,4097,NAN};
    for(unsigned k=0;k<3;k++){memcpy(mut,data,size);putf(mut+at+16,offsets[k]);reject(mut,size);}
    for(unsigned k=0;k<4;k++){memcpy(mut,data,size);putf(mut+at+20,feathers[k]);reject(mut,size);}
    const float edges[]={-4096,4096};for(unsigned k=0;k<2;k++){memcpy(mut,data,size);putf(mut+at+16,edges[k]);ayatsuri_model out;assert(ayatsuri_model_open(mut,size,&out)==AYATSURI_MODEL_OK);}
    void *arena=malloc(bytes),*old_arena=malloc(old_bytes),*other_arena=malloc(bytes);assert(arena&&old_arena&&other_arena);
    ayatsuri_model_instance i,old,other;assert(ayatsuri_model_instance_init(&model,arena,bytes,&i,1,&i)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&plain,old_arena,old_bytes,&old,1,&old)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&model,other_arena,bytes,&other,1,&other)==AYATSURI_MODEL_OK);
    float camera[9]={37,-21,400,2,-3,1,1.3f,120,78},light[6]={0,0,1,1,1,1};
    const double n0[3]={.6f,.8f,0},n1[3]={0,0,-1};
    float poses[36];uint8_t dirty[4]={0};uint32_t colors[4]={0};double opacity[4]={0};
    for(unsigned k=0;k<4;k++)assert(ayatsuri_model_instance_read_pose(&i,&i,1,k,poses+k*9)==AYATSURI_MODEL_OK);
    dirty[0]=7;poses[0]=4;poses[1]=-5;poses[2]=7;poses[3]=.2f;poses[4]=-.35f;poses[5]=.1f;poses[6]=-1.2f;poses[7]=.8f;poses[8]=1.1f;
    ayatsuri_model_targets targets={dirty,poses,colors,opacity,4};
    assert(ayatsuri_model_instance_targets(&i,&i,1,&targets)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_targets(&old,&old,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_evaluate(&old,&old,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&other,&other,1,camera,light)==AYATSURI_MODEL_OK);
    for(unsigned id=1;id<4;id++){ayatsuri_model_packet p=packet(&i,id),o=packet(&old,id);assert(!memcmp(p.storage,o.storage,o.bytes));assert(p.bytes==o.bytes+(id>1?24:0));}
    check_plane(&i,2,camera,n0,3);check_plane(&i,3,camera,n1,-4);
    ayatsuri_model_packet p=packet(&i,2);uint8_t saved[24];memcpy(saved,p.storage+p.bytes-24,24);assert(get16(saved+20)==get16(p.storage+252));
    light[0]=1;light[2]=0;assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);p=packet(&i,2);assert(!memcmp(saved,p.storage+p.bytes-24,24));
    camera[0]=-53;camera[1]=29;camera[6]=.8f;assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);check_plane(&i,2,camera,n0,3);check_plane(&i,3,camera,n1,-4);
    uint32_t epoch=i.mutation_epoch;dirty[0]=0;dirty[1]=1;poses[9]=NAN;assert(ayatsuri_model_instance_targets(&i,&i,1,&targets)==AYATSURI_MODEL_INVALID&&i.evaluated&&i.mutation_epoch==epoch);
    camera[6]=1e-8f;assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_RANGE);uint32_t cursor=0;assert(ayatsuri_model_instance_next(&i,&i,1,&cursor,&p)==AYATSURI_MODEL_STATE);
    camera[6]=1.3f;assert(ayatsuri_model_instance_evaluate(&i,&i,1,camera,light)==AYATSURI_MODEL_OK);check_plane(&i,2,camera,n0,3);
    assert(!memcmp(data,source_copy,size));assert(ayatsuri_model_instance_dispose(&i,&i,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_dispose(&old,&old,1)==AYATSURI_MODEL_OK);assert(ayatsuri_model_instance_dispose(&other,&other,1)==AYATSURI_MODEL_OK);
    printf("AYNM_BLEND_REGION_PASS masks=2 extraArena=48 geometry=identical commonPlane inverseCamera negativeScale parentChild lightStable rangeRecovery atomic immutable truncations=%zu\n",size);
    free(arena);free(old_arena);free(other_arena);free(mut);free(source_copy);free(raw);free(plain_raw);return 0;
}
