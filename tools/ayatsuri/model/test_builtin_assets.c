/* Whitelist spans remain immutable/process-owned; neither Model nor Instance frees them. */
#include "ayatsuri/ayatsuri_model.h"
#include "pocket/pocket_ayatsuri_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void file_equal(const char *path,const uint8_t *data,size_t bytes){
    FILE *f=fopen(path,"rb");CHECK(f);uint8_t *copy=malloc(bytes);CHECK(copy);
    CHECK(fread(copy,1,bytes,f)==bytes&&fgetc(f)==EOF&&!memcmp(copy,data,bytes));
    CHECK(fclose(f)==0);free(copy);
}
int main(void){
    const uint8_t *cloud,*fox,*again;size_t cloud_bytes,fox_bytes,again_bytes;
    CHECK(pocket_ayatsuri_builtin_asset("cloud",&cloud,&cloud_bytes)&&cloud_bytes==2766);
    CHECK(pocket_ayatsuri_builtin_asset("fox-face",&fox,&fox_bytes)&&fox_bytes==3620);
    CHECK(pocket_ayatsuri_builtin_asset("fox-face",&again,&again_bytes)&&again==fox&&again_bytes==fox_bytes);
    const char *unknown[]={"fox-face-base","foxbase","FOX-FACE","fox-face/../cloud","",NULL};
    for(unsigned j=0;j<sizeof unknown/sizeof *unknown;j++){
        again=cloud;again_bytes=17;CHECK(!pocket_ayatsuri_builtin_asset(unknown[j],&again,&again_bytes));
        CHECK(again==cloud&&again_bytes==17);
    }
    CHECK(!pocket_ayatsuri_builtin_asset("cloud",NULL,&again_bytes));
    CHECK(!pocket_ayatsuri_builtin_asset("fox-face",&again,NULL));
    file_equal("apps/cloud/assets/cloud.aynm",cloud,cloud_bytes);
    file_equal("apps/fox3d/assets/fox_face.aynm",fox,fox_bytes);
    ayatsuri_model model;CHECK(ayatsuri_model_open(fox,fox_bytes,&model)==AYATSURI_MODEL_OK);
    CHECK(model.node_count==29&&model.packet_count==25&&model.slot_bytes==8496);
    const char *names[]={"fox-head","skull","ear-outer-1","ear-outer1","jaw-hinge","lower-jaw","cream-face","oral-volume","nose-bridge",
        "eye-socket-1","sclera-1","iris-1","pupil-1","lid-arc-left-1","lid-arc-right-1","brow-1",
        "eye-socket1","sclera1","iris1","pupil1","lid-arc-left1","lid-arc-right1","brow1","lip","nose","ear-inner-1","ear-tuft-1","ear-inner1","ear-tuft1"};
    for(unsigned j=0;j<29;j++){uint16_t id=99;CHECK(ayatsuri_model_find_node(&model,names[j],strlen(names[j]),&id)==AYATSURI_MODEL_OK&&id==j);}
    const size_t bytes=ayatsuri_model_instance_bytes(&model);CHECK(bytes==9192);
    void *allocation=malloc(bytes+7);CHECK(allocation);void *arena=(void *)(((uintptr_t)allocation+7u)&~(uintptr_t)7u);
    ayatsuri_model_instance instance;
    CHECK(ayatsuri_model_instance_init(&model,arena,bytes,&instance,1,&instance)==AYATSURI_MODEL_OK);
    float pose[9];CHECK(ayatsuri_model_instance_read_pose(&instance,&instance,1,7,pose)==AYATSURI_MODEL_OK);
    CHECK(pose[0]==0&&pose[1]==14&&pose[2]==22&&pose[6]==1&&pose[7]==.15f&&pose[8]==1);
    float camera[9]={0,0,400,0,0,0,1.3f,120,78},light[6]={0,0,1,1,1,1};
    CHECK(ayatsuri_model_instance_evaluate(&instance,&instance,1,camera,light)==AYATSURI_MODEL_OK);
    uint32_t cursor=0,count=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
    while((status=ayatsuri_model_instance_next(&instance,&instance,1,&cursor,&packet))==AYATSURI_MODEL_OK){CHECK(packet.node_id!=7);count++;}
    CHECK(status==AYATSURI_MODEL_DONE&&count==24);
    CHECK(ayatsuri_model_instance_dispose(&instance,&instance,1)==AYATSURI_MODEL_OK);free(allocation);
    file_equal("apps/fox3d/assets/fox_face.aynm",fox,fox_bytes);
    puts("BUILTIN_ASSETS_PASS cloud=2766 fox=3620 nodes=29 slots=8496 arena=9192 visible=24 whitelist immutable lifetime");return 0;
}
