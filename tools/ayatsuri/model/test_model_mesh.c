#include "ayatsuri/ayatsuri_model.h"
#include "ayatsuri/ayatsuri_shade.h"
#include "ui/kasane/ksn_procedural.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned j=0;j<4;j++)p[j]=(uint8_t)(v>>(8*j));}
static uint32_t section(const uint8_t *blob,unsigned type){
    for(unsigned j=0;j<u32(blob+12);j++)if(u16(blob+32+j*20)==type)return u32(blob+36+j*20);
    assert(0);return 0;
}
static ayatsuri_model_packet packet(ayatsuri_model_instance *i,const void *owner){
    uint32_t cursor=0;ayatsuri_model_packet result;
    assert(ayatsuri_model_instance_next(i,owner,1,&cursor,&result)==AYATSURI_MODEL_OK);
    assert(result.kind==4&&result.node_id==1&&result.lobe_count==0&&result.bytes==1176);
    ayatsuri_model_packet ignored;assert(ayatsuri_model_instance_next(i,owner,1,&cursor,&ignored)==AYATSURI_MODEL_DONE);
    return result;
}
static ksn_proc_gouraud_patch copy_patch(ayatsuri_model_instance *i,const void *owner){
    ayatsuri_model_packet p=packet(i,owner);ksn_proc_gouraud_patch result;memcpy(&result,p.storage+84,sizeof result);return result;
}
static void rejected(const uint8_t *blob,size_t bytes){
    ayatsuri_model sentinel,out;memset(&sentinel,0xa5,sizeof sentinel);out=sentinel;
    assert(ayatsuri_model_open(blob,bytes,&out)==AYATSURI_MODEL_INVALID);assert(!memcmp(&out,&sentinel,sizeof out));
}
int main(int argc,char **argv){
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));const long size=ftell(file);assert(size>0);rewind(file);
    const size_t bytes=(size_t)size;uint8_t *allocation=malloc(bytes+1),*mut=malloc(bytes),*original=malloc(bytes);assert(allocation&&mut&&original);
    uint8_t *blob=allocation+1;assert(fread(blob,1,bytes,file)==bytes);assert(!fclose(file));memcpy(original,blob,bytes);
    ayatsuri_model model;assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
    assert(model.node_count==2&&model.packet_count==1&&model.slot_bytes==1260);
    const uint32_t descriptor=section(blob,9),vertices=section(blob,10),triangles=section(blob,11),holes=section(blob,12);
    for(size_t cut=0;cut<bytes;cut++)rejected(blob,cut);
    const unsigned bad_offsets[]={0,2,4,6,8,10,11,12,13,19,20,31};
    for(unsigned k=0;k<sizeof bad_offsets/sizeof bad_offsets[0];k++){
        memcpy(mut,blob,bytes);mut[descriptor+bad_offsets[k]]=255;rejected(mut,bytes);
    }
    for(unsigned field=0;field<7;field++){
        memcpy(mut,blob,bytes);put32(mut+vertices+4*field,0x7fc00000);rejected(mut,bytes);
    }
    memcpy(mut,blob,bytes);memset(mut+vertices+12,0,12);rejected(mut,bytes);
    memcpy(mut,blob,bytes);put32(mut+vertices,0x45800800);rejected(mut,bytes); /* 4097 */
    memcpy(mut,blob,bytes);put16(mut+triangles,4);rejected(mut,bytes);
    memcpy(mut,blob,bytes);put16(mut+triangles,1);rejected(mut,bytes);
    memcpy(mut,blob,bytes);put32(mut+holes,0x7f800000);rejected(mut,bytes);
    memcpy(mut,blob,bytes);put16(mut+section(blob,1)+16+4,1);rejected(mut,bytes);
    const uint32_t common=section(blob,5);
    for(unsigned field=0;field<3;field++)for(unsigned value=0;value<2;value++){
        memcpy(mut,blob,bytes);const double invalid=field==2?(value?1:.5):(value?.5:0);
        uint64_t bits;memcpy(&bits,&invalid,8);put32(mut+common+48+8*field,(uint32_t)bits);
        put32(mut+common+52+8*field,(uint32_t)(bits>>32));rejected(mut,bytes);
    }
    for(unsigned type=9;type<=12;type++){
        memcpy(mut,blob,bytes);for(unsigned j=0;j<u32(mut+12);j++)if(u16(mut+32+j*20)==type){put16(mut+32+j*20,99);put16(mut+34+j*20,0);}
        rejected(mut,bytes);
    }
    unsigned owner=0,second_owner=0;const size_t arena_bytes=ayatsuri_model_instance_bytes(&model);
    void *arena=malloc(arena_bytes),*second_arena=malloc(arena_bytes),*saved=malloc(arena_bytes);assert(arena&&second_arena&&saved);
    ayatsuri_model_instance first,second;assert(ayatsuri_model_instance_init(&model,arena,arena_bytes,&owner,1,&first)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&model,second_arena,arena_bytes,&second_owner,1,&second)==AYATSURI_MODEL_OK);
    uint8_t kind=255;
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,0,&kind)==AYATSURI_MODEL_OK&&kind==0);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,&kind)==AYATSURI_MODEL_OK&&kind==3);
    kind=255;const ayatsuri_model_instance before_kind=first;memcpy(saved,arena,arena_bytes);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,2,&kind)==AYATSURI_MODEL_INVALID&&kind==255);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,NULL)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_node_kind(&first,&second_owner,1,1,&kind)==AYATSURI_MODEL_STATE&&kind==255);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,2,1,&kind)==AYATSURI_MODEL_STATE&&kind==255);
    assert(ayatsuri_model_instance_node_kind(NULL,&owner,1,1,&kind)==AYATSURI_MODEL_STATE&&kind==255);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,(uint8_t *)&first.camera)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,(uint8_t *)&model)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,arena)==AYATSURI_MODEL_INVALID);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,blob)==AYATSURI_MODEL_INVALID);
    first.busy=true;assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,&kind)==AYATSURI_MODEL_BUSY&&kind==255);first.busy=false;
    assert(!memcmp(&first,&before_kind,sizeof first)&&!memcmp(saved,arena,arena_bytes)&&!memcmp(blob,original,bytes));
    float camera[9]={0,0,400,0,0,0,1,120,78},light[6]={-.5f,-.4f,.8f,1,1,1};
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&second,&second_owner,1,camera,light)==AYATSURI_MODEL_OK);
    const ksn_proc_gouraud_patch stable=copy_patch(&first,&owner);ksn_proc_gouraud_patch actual=copy_patch(&second,&second_owner);
    assert(!memcmp(&stable,&actual,sizeof stable));assert(stable.header.vertices==4&&stable.header.triangles==2&&stable.header.holes==1);
    assert(stable.vertices[0].x==110&&stable.vertices[0].y==68&&stable.vertices[3].x==130&&stable.vertices[3].y==88);
    assert(stable.vertices[0].mask==-10&&stable.vertices[2].mask==10);
    assert(stable.clips[0].count==3&&stable.clips[0].xy[0]==118&&stable.clips[0].xy[1]==76);
    const uint16_t expected_indices[]={0,1,2,1,3,2};assert(!memcmp(stable.indices,expected_indices,sizeof expected_indices));
    const float material[8]={.4f,.6f,.025f,4,.08f,1,.73f,.44f};const int16_t front[3]={0,0,32767};ayatsuri_shade_basis basis;
    assert(ayatsuri_shade_prepare(first.lighting,&basis));
    assert(stable.vertices[0].color_a==ayatsuri_shade_sample(front,0xe3c6,175,first.lighting,material,&basis));
    assert(stable.vertices[0].color_b==ayatsuri_shade_sample(front,0xff57,175,first.lighting,material,&basis));
    uint8_t dirty[2]={0};float pose[18];uint32_t color[2]={0};double opacity[2]={1,1};
    for(unsigned j=0;j<2;j++)assert(ayatsuri_model_instance_read_pose(&first,&owner,1,(uint16_t)j,pose+9*j)==AYATSURI_MODEL_OK);
    ayatsuri_model_targets targets={dirty,pose,color,opacity,2};memcpy(saved,arena,arena_bytes);
    for(unsigned flag=32;flag<=64;flag*=2){
        dirty[0]=1;pose[0]=5;dirty[1]=(uint8_t)flag;
        assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_INVALID);
        assert(!memcmp(saved,arena,arena_bytes)&&first.evaluated&&first.mutation_epoch==0);
    }
    dirty[0]=dirty[1]=7;pose[0]=0;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    const uint32_t updates=first.stats.geometry_updates;
    for(unsigned j=0;j<1000;j++)assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(first.stats.geometry_updates==updates&&first.mutation_epoch==0);
    /* Nonuniform parent scale and child rotation exercise inverse-transpose. */
    pose[6]=2;pose[8]=.5f;pose[9+4]=.4f;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    ayatsuri_model_packet transformed=packet(&first,&owner);float matrix[12];memcpy(matrix,transformed.storage,48);
    double nx=(double)matrix[4]*matrix[9]-(double)matrix[8]*matrix[5],
        ny=(double)matrix[8]*matrix[1]-(double)matrix[0]*matrix[9],
        nz=(double)matrix[0]*matrix[5]-(double)matrix[4]*matrix[1],norm=hypot(hypot(nx,ny),nz);
    const int16_t expected_normal[3]={(int16_t)floor(nx*32767/norm+.5),(int16_t)floor(ny*32767/norm+.5),(int16_t)floor(nz*32767/norm+.5)};
    actual=copy_patch(&first,&owner);
    assert(actual.vertices[0].color_a==ayatsuri_shade_sample(expected_normal,0xe3c6,175,first.lighting,material,&basis));
    ksn_proc_gouraud_patch second_stable=copy_patch(&second,&second_owner);assert(!memcmp(&stable,&second_stable,sizeof stable));
    /* Reflection preserves source winding: an X reflection is wholly culled. */
    pose[6]=-1;pose[8]=1;pose[13]=0;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    uint32_t cursor=0;ayatsuri_model_packet none;
    assert(ayatsuri_model_instance_next(&first,&owner,1,&cursor,&none)==AYATSURI_MODEL_DONE);
    /* A Z reflection keeps projected winding but reverses the outward normal. */
    pose[6]=1;pose[8]=-1;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    const int16_t back[3]={0,0,-32767};actual=copy_patch(&first,&owner);
    assert(actual.vertices[0].color_a==ayatsuri_shade_sample(back,0xe3c6,175,first.lighting,material,&basis));
    dirty[0]=AYATSURI_TARGET_HIDDEN;dirty[1]=0;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);cursor=0;
    assert(ayatsuri_model_instance_next(&first,&owner,1,&cursor,&none)==AYATSURI_MODEL_DONE);
    dirty[0]=AYATSURI_TARGET_VISIBLE;
    assert(ayatsuri_model_instance_targets(&first,&owner,1,&targets)==AYATSURI_MODEL_OK);
    camera[7]=5000;
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_RANGE);cursor=0;
    assert(ayatsuri_model_instance_next(&first,&owner,1,&cursor,&none)==AYATSURI_MODEL_STATE);
    second_stable=copy_patch(&second,&second_owner);assert(!memcmp(&stable,&second_stable,sizeof stable));
    camera[7]=120;assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    /* Invalid projected hole: cold XYZ is finite, but a repeated rim point is not convex. */
    memcpy(mut,blob,bytes);memcpy(mut+holes+12,mut+holes,12);ayatsuri_model bad_hole;
    assert(ayatsuri_model_open(mut,bytes,&bad_hole)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&first,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&bad_hole,arena,arena_bytes,&owner,1,&first)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&first,&owner,1,camera,light)==AYATSURI_MODEL_RANGE);
    assert(ayatsuri_model_instance_dispose(&first,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&first,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_dispose(&second,&second_owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_node_kind(&first,&owner,1,1,&kind)==AYATSURI_MODEL_STATE&&kind==255);
    assert(!memcmp(blob,original,bytes));free(saved);free(second_arena);free(arena);free(original);free(mut);free(allocation);
    printf("AYNM_MESH_CORE_PASS nodes=2 triangles=2 arena=%zu malformed=true unaligned=true shared=true atomic=true reflected=true empty=true\n",arena_bytes);
    return 0;
}
