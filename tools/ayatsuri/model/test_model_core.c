#include "ayatsuri/ayatsuri_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n){for(unsigned k=0;k<4;++k)p[k]=(uint8_t)(n>>(8*k));}
static uint8_t *read_file(const char *path,size_t *bytes){
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    *bytes=(size_t)n;uint8_t *p=malloc(*bytes+1);assert(p);assert(fread(p+1,1,*bytes,f)==*bytes);fclose(f);return p;
}
static unsigned packet_count(ayatsuri_model_instance *i,const void *owner){
    uint32_t cursor=0;unsigned count=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
    while((status=ayatsuri_model_instance_next(i,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){
        assert(packet.kind==1||packet.kind==2);assert(packet.storage&&packet.lighting);++count;
    }
    assert(status==AYATSURI_MODEL_DONE);return count;
}
static void dump_packets(const char *path,ayatsuri_model_instance *i,const void *owner){
    FILE *f=fopen(path,"wb");assert(f);uint8_t header[8]={'M','C','P','1',0,0,0,0};
    put32(header+4,packet_count(i,owner));assert(fwrite(header,1,8,f)==8);
    uint32_t cursor=0;ayatsuri_model_packet p;
    while(ayatsuri_model_instance_next(i,owner,1,&cursor,&p)==AYATSURI_MODEL_OK){
        uint8_t row[8]={(uint8_t)p.node_id,(uint8_t)(p.node_id>>8),p.kind,p.lobe_count,0,0,0,0};
        put32(row+4,36+(p.kind==1?148:58+84*p.lobe_count));assert(fwrite(row,1,8,f)==8);
        assert(fwrite(p.lighting,1,36,f)==36);
        if(p.kind==1)assert(fwrite(p.storage+96,1,148,f)==148);
        else{assert(fwrite(p.storage+252,1,6,f)==6);assert(fwrite(p.storage+264,1,44,f)==44);
            assert(fwrite(p.storage+308,1,8,f)==8);assert(fwrite(p.storage+316+24*p.lobe_count,84,p.lobe_count,f)==p.lobe_count);}
    }
    assert(!fclose(f));
}
int main(int argc,char **argv){
    assert(argc==2||argc==3);size_t bytes;uint8_t *allocation=read_file(argv[1],&bytes),*blob=allocation+1;
    uint8_t *mut=malloc(bytes+64),*original=malloc(bytes);assert(mut&&original);memcpy(original,blob,bytes);
    ayatsuri_model model,sentinel;memset(&sentinel,0xa5,sizeof sentinel);
    assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_open(blob,bytes,(ayatsuri_model *)(void *)(blob+7))==AYATSURI_MODEL_INVALID);
    assert(model.node_count==19&&model.packet_count==16&&model.slot_bytes==6144);
    uint16_t id=999;assert(ayatsuri_model_find_node(&model,"actor",5,&id)==AYATSURI_MODEL_OK&&id==0);
    assert(ayatsuri_model_find_node(&model,"missing",7,&id)==AYATSURI_MODEL_DONE&&id==0);
    assert(ayatsuri_model_find_node(&model,"actor",5,(uint16_t *)(void *)&model)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_find_node(&model,"actor",5,(uint16_t *)(void *)(blob+1))==AYATSURI_MODEL_INVALID);
    assert(!memcmp(original,blob,bytes));
    for(size_t n=0;n<bytes;++n){ayatsuri_model out=sentinel;assert(ayatsuri_model_open(blob,n,&out)!=AYATSURI_MODEL_OK);assert(!memcmp(&out,&sentinel,sizeof out));}
    const unsigned offsets[]={0,4,6,8,12,16,20,24,28,32,34,36,40,44,48};
    for(unsigned k=0;k<sizeof offsets/sizeof offsets[0];++k){memcpy(mut,blob,bytes);mut[offsets[k]]^=255;
        ayatsuri_model out=sentinel;assert(ayatsuri_model_open(mut,bytes,&out)!=AYATSURI_MODEL_OK);assert(!memcmp(&out,&sentinel,sizeof out));}
    memcpy(mut,blob,bytes);mut[model.section[0].offset]=0;mut[model.section[0].offset+1]=0;
    assert(ayatsuri_model_open(mut,bytes,&sentinel)==AYATSURI_MODEL_INVALID);
    memcpy(mut,blob,bytes);put32(mut+model.section[1].offset+24,0);
    assert(ayatsuri_model_open(mut,bytes,&sentinel)==AYATSURI_MODEL_INVALID);
    memcpy(mut,blob,bytes);put32(mut+model.section[2].offset,0x7fc00000);
    assert(ayatsuri_model_open(mut,bytes,&sentinel)==AYATSURI_MODEL_INVALID);
    /* A bounded unknown optional section is skipped; changing only its flag
     * to required must reject the very same bytes. */
    const uint32_t first=get32(blob+36),extra=(uint32_t)((bytes+16+7)&~(size_t)7),extended=extra+8;
    memset(mut,0,bytes+64);memcpy(mut,blob,32+7*20);memcpy(mut+first+16,blob+first,bytes-first);
    put32(mut+8,extended);put32(mut+12,8);
    for(unsigned k=0;k<7;++k)put32(mut+36+20*k,get32(blob+36+20*k)+16);
    uint8_t *d=mut+32+7*20;d[0]=99;put32(d+4,extra);put32(d+8,8);put32(d+12,8);put32(d+16,1);
    assert(ayatsuri_model_open(mut,extended,&sentinel)==AYATSURI_MODEL_OK);d[2]=1;
    assert(ayatsuri_model_open(mut,extended,&sentinel)==AYATSURI_MODEL_INVALID);
    uint32_t random=0x12345678;
    for(unsigned k=0;k<5000;++k){memcpy(mut,blob,bytes);random=random*1664525u+1013904223u;unsigned at=random%bytes;
        random=random*1664525u+1013904223u;mut[at]^=(uint8_t)(1+random%255);ayatsuri_model_open(mut,bytes,&sentinel);}

    const size_t arena_bytes=ayatsuri_model_instance_bytes(&model);void *arena=malloc(arena_bytes),*second_arena=malloc(arena_bytes);
    uint8_t *saved_arena=malloc(arena_bytes);assert(arena&&second_arena&&saved_arena);
    ayatsuri_model_instance i,second;int owner,other_owner;
    assert(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&i)==AYATSURI_MODEL_OK);
    memcpy(second_arena,&model,sizeof model);
    assert(ayatsuri_model_instance_init(second_arena,second_arena,arena_bytes,&other_owner,1,&second)==AYATSURI_MODEL_INVALID);
    assert(!memcmp(second_arena,&model,sizeof model));
    assert(ayatsuri_model_instance_init(&model,second_arena,arena_bytes,&other_owner,1,
        (ayatsuri_model_instance *)(void *)&model)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_init(&model,second_arena,arena_bytes,&other_owner,1,&second)==AYATSURI_MODEL_OK);
    float camera[9]={37,-21,400,0,0,0,.83f,120,75},light[6];
    const double norm=hypot(hypot(-.5,-.4),.8);light[0]=(float)(-.5/norm);light[1]=(float)(-.4/norm);light[2]=(float)(.8/norm);
    light[3]=light[4]=light[5]=1;
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);assert(packet_count(&i,&owner)==16);
    assert(ayatsuri_model_instance_evaluate(&second,&other_owner,1,camera,light)==AYATSURI_MODEL_OK);
    /* Output copies must not mutate a live cache or the shared model metadata.
     * In particular, writing a pose over camera would bypass invalidation. */
    uint8_t saved_instance[sizeof i],saved_model[sizeof model];
    memcpy(saved_instance,&i,sizeof i);memcpy(saved_model,&model,sizeof model);
    assert(ayatsuri_model_instance_read_pose(&i,&owner,1,0,i.camera)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_read_pose(&i,&owner,1,0,(float *)(void *)&model)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_stats(&i,&owner,1,(ayatsuri_model_stats *)(void *)&model)==AYATSURI_MODEL_INVALID);
    uint32_t alias_cursor=0;ayatsuri_model_packet alias_packet;
    memcpy(saved_arena,arena,arena_bytes);
    assert(ayatsuri_model_instance_next(&i,&owner,1,&i.mutation_epoch,&alias_packet)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_next(&i,&owner,1,&alias_cursor,(ayatsuri_model_packet *)(void *)&model)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_next(&i,&owner,1,&alias_cursor,arena)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_next(&i,&owner,1,(uint32_t *)(void *)(blob+3),&alias_packet)==AYATSURI_MODEL_INVALID);
    union { uint32_t cursor; ayatsuri_model_packet packet; } mutual;
    memset(&mutual,0,sizeof mutual);
    assert(ayatsuri_model_instance_next(&i,&owner,1,&mutual.cursor,&mutual.packet)==AYATSURI_MODEL_INVALID);
    assert(alias_cursor==0&&mutual.cursor==0&&!memcmp(arena,saved_arena,arena_bytes)&&!memcmp(original,blob,bytes));
    assert(!memcmp(&i,saved_instance,sizeof i)&&!memcmp(&model,saved_model,sizeof model));
    if(argc==3)dump_packets(argv[2],&i,&owner);
    uint8_t dirty[19]={0};float pose[19*9];uint32_t color[19]={0};double opacity[19]={0};
    for(unsigned j=0;j<19;++j)assert(ayatsuri_model_instance_read_pose(&i,&owner,1,(uint16_t)j,pose+9*j)==AYATSURI_MODEL_OK);
    ayatsuri_model_targets t={dirty,pose,color,opacity,19};ayatsuri_model_stats before,after;
    assert(ayatsuri_model_instance_stats(&i,&owner,1,&before)==AYATSURI_MODEL_OK);
    memset(dirty,7,sizeof dirty);assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);
    assert(i.mutation_epoch==0&&i.evaluated);assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_stats(&i,&owner,1,&after)==AYATSURI_MODEL_OK);
    assert(after.geometry_updates==before.geometry_updates&&after.projection_updates==before.projection_updates&&after.target_changed==0);
    memcpy(saved_arena,arena,arena_bytes);pose[0]=3;pose[18*9+6]=0;
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_INVALID);
    assert(!memcmp(arena,saved_arena,arena_bytes)&&i.mutation_epoch==0&&i.evaluated);pose[18*9+6]=1;
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);assert(i.mutation_epoch==1&&!i.evaluated);
    float second_pose[9];assert(ayatsuri_model_instance_read_pose(&second,&other_owner,1,0,second_pose)==AYATSURI_MODEL_OK&&second_pose[0]==0);
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    memset(dirty,0,sizeof dirty);dirty[0]=AYATSURI_TARGET_HIDDEN;
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);assert(packet_count(&i,&owner)==5);
    dirty[0]=AYATSURI_TARGET_VISIBLE;dirty[1]=AYATSURI_TARGET_COLOR|AYATSURI_TARGET_OPACITY;color[1]=0x123456;opacity[1]=.7;
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    uint32_t cursor=0;ayatsuri_model_packet packet;assert(ayatsuri_model_instance_next(&i,&owner,1,&cursor,&packet)==AYATSURI_MODEL_OK);
    uint16_t alpha;memcpy(&alpha,packet.storage+254,2);assert(alpha==179);
    /* A device ArrayBuffer may be four-byte aligned even for Float64 data.
     * Decode byte-addressed values without a double load or batch scratch. */
    _Alignas(8) uint8_t opacity_storage[sizeof opacity+8];uint8_t *unaligned_opacity=opacity_storage+4;
    assert(((uintptr_t)unaligned_opacity&7)==4);memcpy(unaligned_opacity,opacity,sizeof opacity);
    t.opacity=unaligned_opacity;memset(dirty,0,sizeof dirty);dirty[1]=dirty[18]=AYATSURI_TARGET_OPACITY;
    const double half=.5,bad=NAN;memcpy(unaligned_opacity+8,&half,8);memcpy(unaligned_opacity+18*8,&bad,8);
    memcpy(saved_instance,&i,sizeof i);memcpy(saved_arena,arena,arena_bytes);
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_INVALID);
    assert(!memcmp(saved_instance,&i,sizeof i)&&!memcmp(saved_arena,arena,arena_bytes));
    dirty[18]=0;assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    cursor=0;assert(ayatsuri_model_instance_next(&i,&owner,1,&cursor,&packet)==AYATSURI_MODEL_OK);
    memcpy(&alpha,packet.storage+254,2);assert(alpha==128);
    const uint32_t epoch=i.mutation_epoch;assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_OK);
    assert(i.mutation_epoch==epoch&&i.evaluated);
    assert(ayatsuri_model_instance_read_pose(&i,&other_owner,1,0,second_pose)==AYATSURI_MODEL_STATE);
    assert(ayatsuri_model_instance_read_pose(&i,&owner,2,0,second_pose)==AYATSURI_MODEL_STATE);
    i.busy=true;assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_BUSY);i.busy=false;
    camera[6]=0;assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_INVALID);
    cursor=0;assert(ayatsuri_model_instance_next(&i,&owner,1,&cursor,&packet)==AYATSURI_MODEL_STATE);
    camera[6]=.83f;assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_targets(&i,&owner,1,&t)==AYATSURI_MODEL_STATE);
    assert(ayatsuri_model_instance_dispose(&second,&other_owner,1)==AYATSURI_MODEL_OK);
    assert(!memcmp(original,blob,bytes));
    printf("AYNM_CORE_PASS nodes=19 packets=16 cold=%zu slots=6144 arena=%zu truncation=%zu fuzz=5000 transaction isolation visibility lifetime no-op\n",bytes,arena_bytes,bytes);
    free(saved_arena);free(second_arena);free(arena);free(original);free(mut);free(allocation);return 0;
}
