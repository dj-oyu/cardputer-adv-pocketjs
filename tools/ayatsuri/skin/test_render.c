#include "ayatsuri/ayatsuri_model_skin.h"
#include "ayatsuri/ayatsuri_shade.h"
#include "ui/kasane/ksn_mesh_depth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Count only linked allocation requests, not libc/ASan internals or stack. */
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);void __real_free(void *);
static struct {void *p;size_t n;} ledger[64];
static size_t live,peak,calls;
static void record(void *p,size_t n){
    if(!p)return;
    for(unsigned j=0;j<64;++j)if(!ledger[j].p){ledger[j].p=p;ledger[j].n=n;live+=n;if(live>peak)peak=live;return;}
    assert(0);
}
static size_t forget(void *p){
    if(!p)return 0;
    for(unsigned j=0;j<64;++j)if(ledger[j].p==p){size_t n=ledger[j].n;live-=n;ledger[j].p=NULL;return n;}
    assert(0);return 0;
}
void *__wrap_malloc(size_t n){++calls;void *p=__real_malloc(n);record(p,n);return p;}
void *__wrap_calloc(size_t n,size_t s){++calls;void *p=__real_calloc(n,s);record(p,n*s);return p;}
void *__wrap_realloc(void *p,size_t n){++calls;size_t old=forget(p);void *q=__real_realloc(p,n);if(q)record(q,n);else if(n)record(p,old);return q;}
void __wrap_free(void *p){forget(p);__real_free(p);}

static const ayatsuri_transform identity={{1,0,0,0,0,1,0,0,0,0,1,0}};
static const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
static const uint16_t indices[12]={0,1,4,0,4,3,1,2,5,1,5,4};
static uint16_t pixels[240*135],reference[240*135],rest[240*135];
static float actual_z[240*135],reference_z[240*135];
static uint16_t replay[240*8];
static uint8_t *load(const char *dir,const char *variant,size_t *size){
    char path[1024];assert(snprintf(path,sizeof path,"%s/skin-%s.aynm",dir,variant)>0);
    FILE *f=fopen(path,"rb");assert(f&&fseek(f,0,SEEK_END)==0);long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *p=malloc((size_t)n);assert(p&&fread(p,1,(size_t)n,f)==(size_t)n&&fclose(f)==0);*size=(size_t)n;return p;
}
static ksn_mesh_snapshot snapshot(uint8_t *data,size_t bytes){
    return (ksn_mesh_snapshot){.packets={data,bytes,0,0},.work_limit=200000,.enabled=true};
}
static void append_model(ayatsuri_model_instance *i,const void *owner,ksn_mesh_snapshot *s){
    uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
    while((status=ayatsuri_model_instance_next(i,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){
        assert(packet.kind==4);ayatsuri_model_mesh_view view;
        assert(ayatsuri_model_instance_mesh_view(i,owner,1,packet.node_id,&view)==AYATSURI_MODEL_OK);
        assert(ksn_mesh_append(s,view.patch,view.depth,view.node_id));
    }
    assert(status==AYATSURI_MODEL_DONE);
}
static void render(const ksn_mesh_snapshot *s,uint16_t *image,float *depth,unsigned band,unsigned rows,float *scratch){
    for(unsigned j=0;j<240*135;++j)image[j]=17;
    for(unsigned y=0;y<135;y+=band){
        unsigned h=135-y<band?135-y:band;
        assert(ksn_mesh_render(s,image+y*240,y,h,scratch,rows));
        /* Preserve the original band RGB. Separate chunk replays collect all
         * depth rows and must independently equal that untouched band image. */
        for(unsigned offset=0;offset<h;offset+=rows){
            unsigned n=h-offset<rows?h-offset:rows;
            for(unsigned j=0;j<n*240;++j)replay[j]=17;
            assert(ksn_mesh_render(s,replay,y+offset,n,scratch,rows));
            assert(!memcmp(replay,image+(y+offset)*240,n*240*sizeof *replay));
            memcpy(depth+(y+offset)*240,scratch,n*240*sizeof *scratch);
        }
    }
}
static void rest_inputs(ayatsuri_skin_vertex *v){
    for(unsigned j=0;j<6;++j){
        memset(v+j,0,sizeof *v);v[j].position[0]=(float)((int)(j%3)*30-30);v[j].position[1]=j<3?-15:15;
        v[j].normal[2]=1;v[j].joints[1]=1;v[j].weights[1]=(uint16_t)((j%3)==0?0:(j%3)==1?21845:65535);
        v[j].weights[0]=(uint16_t)(65535-v[j].weights[1]);
    }
}
static void deform(unsigned frame,ayatsuri_transform *global,const ayatsuri_transform *bind,
                   ayatsuri_transform *palette,const ayatsuri_skin_vertex *v,ayatsuri_skin_result *out){
    global[0]=identity;
    ayatsuri_pose pose={{15,0,0},{0,(float)(.7*sin(frame*.11)),0},{1,1,1}};
    pose.position[1]=(float)(8*sin(frame*.07));pose.position[2]=(float)(12*sin(frame*.13));
    assert(ayatsuri_transform_evaluate(&pose,NULL,global+1)==AYATSURI_OK);
    assert(ayatsuri_skin_palette(global,bind,2,palette)==AYATSURI_OK);
    assert(ayatsuri_skin_evaluate(palette,2,v,6,out)==AYATSURI_OK);
}
static ksn_proc_gouraud_patch oracle(const ayatsuri_skin_result *v,float depth[6]){
    /* Independent node-local projection and SNORM preparation. The existing
     * shader/raster remain shared; this is not an independent raster oracle. */
    ksn_proc_gouraud_patch p={0};p.header.vertices=6;p.header.triangles=4;memcpy(p.indices,indices,sizeof indices);
    const float lighting[9]={0,0,1,0,0,1,1,1,1},material[8]={.25f,.75f,0,4,0,0,0,0};
    ayatsuri_shade_basis basis;assert(ayatsuri_shade_prepare(lighting,&basis));
    for(unsigned j=0;j<6;++j){
        p.vertices[j].x=v[j].position[0]+131;p.vertices[j].y=v[j].position[1]+66;p.vertices[j].mask=-1;
        depth[j]=v[j].position[2];
        double n=hypot(hypot(v[j].normal[0],v[j].normal[1]),v[j].normal[2]);int16_t sn[3];
        for(unsigned r=0;r<3;++r)sn[r]=(int16_t)fmax(-32767,fmin(32767,floor(v[j].normal[r]*32767.0/n+.5)));
        p.vertices[j].color_a=p.vertices[j].color_b=ayatsuri_shade_sample(sn,0xf800,255,lighting,material,&basis);
    }
    return p;
}
static void near(double actual,double expected){assert(fabs(actual-expected)<=2e-5*fmax(1,fabs(expected)));}
static void transform_oracle(const float poses[27],const ayatsuri_skin_result *source,
                             double point[3],double normal[3]){
    /* Deliberately sequential S/R/T operations, not the core matrix builder:
     * child Ry and root Rz have unequal scales on both sides of a rotation. */
    double cy=cos(poses[13]),sy=sin(poses[13]),cz=cos(poses[5]),sz=sin(poses[5]);
    double x=source->position[0]*poses[15],y=source->position[1]*poses[16],z=source->position[2]*poses[17];
    double a=(cy*x+sy*z+poses[9])*poses[6],b=(y+poses[10])*poses[7],c=(-sy*x+cy*z+poses[11])*poses[8];
    point[0]=cz*a-sz*b+poses[0];point[1]=sz*a+cz*b+poses[1];point[2]=c+poses[2];
    x=source->normal[0]/poses[15];y=source->normal[1]/poses[16];z=source->normal[2]/poses[17];
    a=(cy*x+sy*z)/poses[6];b=y/poses[7];c=(-sy*x+cy*z)/poses[8];
    normal[0]=cz*a-sz*b;normal[1]=sz*a+cz*b;normal[2]=c;
    double length=hypot(hypot(normal[0],normal[1]),normal[2]);for(unsigned r=0;r<3;++r)normal[r]/=length;
}
static void check_transformed(ayatsuri_model_instance *i,const void *owner,const float poses[27],
                              const ayatsuri_skin_result *source){
    ayatsuri_model_mesh_view view;assert(ayatsuri_model_instance_mesh_view(i,owner,1,1,&view)==AYATSURI_MODEL_OK);
    const ksn_proc_gouraud_patch *patch=view.patch;assert(patch->header.triangles==4);
    const float lighting[9]={0,0,1,0,0,1,1,1,1},material[8]={.25f,.75f,0,4,0,0,0,0};
    ayatsuri_shade_basis basis;assert(ayatsuri_shade_prepare(lighting,&basis));
    for(unsigned j=0;j<6;++j){
        double point[3],normal[3];transform_oracle(poses,source+j,point,normal);
        near(patch->vertices[j].x,point[0]+120);near(patch->vertices[j].y,point[1]+67);near(view.depth[j],point[2]);
        int16_t sn[3];for(unsigned r=0;r<3;++r)sn[r]=(int16_t)floor(normal[r]*32767+.5);
        assert(patch->vertices[j].color_a==ayatsuri_shade_sample(sn,0xf800,255,lighting,material,&basis));
    }
}
static void hierarchy_and_batches(ayatsuri_model_instance *i,const void *owner,
                                 const ayatsuri_skin_vertex *input,ayatsuri_skin_result *result){
    float original[27],poses[27];for(unsigned j=0;j<3;++j)
        assert(ayatsuri_model_instance_read_pose(i,owner,1,(uint16_t)j,original+9*j)==AYATSURI_MODEL_OK);
    memcpy(poses,original,sizeof poses);poses[5]=.4f;poses[6]=1.3f;poses[7]=.7f;poses[8]=1.2f;
    poses[13]=-.3f;poses[15]=.9f;poses[16]=1.4f;poses[17]=.8f;
    uint8_t dirty[3]={7,7,0};uint32_t colors[3]={0};double opacity[3]={1,1,1};
    ayatsuri_model_targets targets={dirty,poses,colors,opacity,3};
    assert(ayatsuri_model_instance_targets(i,owner,1,&targets)==AYATSURI_MODEL_OK);
    ayatsuri_model_mesh_geometry geometry={1,6,result};
    assert(ayatsuri_model_instance_evaluate_meshes(i,owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    check_transformed(i,owner,poses,result);
    /* A target edit after skin must not accidentally reuse the skin packet in
     * ordinary evaluation. Check cold vertices under the NEW hierarchy. */
    poses[0]+=3;
    assert(ayatsuri_model_instance_targets(i,owner,1,&targets)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(i,owner,1,camera,light)==AYATSURI_MODEL_OK&&!i->mesh_overridden);
    ayatsuri_skin_result cold[6];for(unsigned j=0;j<6;++j){memcpy(cold[j].position,input[j].position,12);memcpy(cold[j].normal,input[j].normal,12);}
    check_transformed(i,owner,poses,cold);
    memcpy(poses,original,sizeof poses);assert(ayatsuri_model_instance_targets(i,owner,1,&targets)==AYATSURI_MODEL_OK);
    ayatsuri_skin_result back[4]={{{-27,-20,9},{0,0,1}},{{43,-20,9},{0,0,1}},{{43,20,9},{0,0,1}},{{-27,20,9},{0,0,1}}};
    /* Deliberately reverse descriptors: matching is by node ID, not order. */
    ayatsuri_model_mesh_geometry both[2]={{2,4,back},geometry};
    assert(ayatsuri_model_instance_evaluate_meshes(i,owner,1,camera,light,both,2)==AYATSURI_MODEL_OK);
    ayatsuri_model_mesh_view view;
    assert(ayatsuri_model_instance_mesh_view(i,owner,1,2,&view)==AYATSURI_MODEL_OK);
    near(((const ksn_proc_gouraud_patch *)view.patch)->vertices[0].x,104);near(view.depth[0],4);
    assert(ayatsuri_model_instance_mesh_view(i,owner,1,1,&view)==AYATSURI_MODEL_OK);
    near(((const ksn_proc_gouraud_patch *)view.patch)->vertices[2].x,result[2].position[0]+131);
    /* Empty batch clears all overrides without a target or camera change. */
    assert(ayatsuri_model_instance_evaluate_meshes(i,owner,1,camera,light,NULL,0)==AYATSURI_MODEL_OK&&!i->mesh_overridden);
    check_transformed(i,owner,original,cold);
    assert(ayatsuri_model_instance_mesh_view(i,owner,1,2,&view)==AYATSURI_MODEL_OK);
    near(((const ksn_proc_gouraud_patch *)view.patch)->vertices[0].x,96);near(view.depth[0],-5);
    puts("SKIN_HIERARCHY_PASS nonuniform_parent_Rz_child_Ry inverse_transpose two_node_override empty_clear skin_targets_cold_restore");
}
static void rejection_cases(ayatsuri_model_instance *i,const void *owner,ayatsuri_skin_result *result){
    const ayatsuri_model_instance saved=*i;void *arena=malloc(i->arena_bytes);assert(arena);memcpy(arena,i->arena,i->arena_bytes);
    ayatsuri_model_mesh_geometry g[2]={{1,6,result},{1,6,result}};
#define REJECT(expected_owner,gen,entries,n,status) do { \
    assert(ayatsuri_model_instance_evaluate_meshes(i,expected_owner,gen,camera,light,entries,n)==status); \
    assert(!memcmp(i,&saved,sizeof saved)&&!memcmp(i->arena,arena,i->arena_bytes)); \
} while(0)
    unsigned stranger=0;REJECT(&stranger,1,g,1,AYATSURI_MODEL_STATE);REJECT(owner,2,g,1,AYATSURI_MODEL_STATE);
    REJECT(owner,1,NULL,1,AYATSURI_MODEL_INVALID);REJECT(owner,1,g,SIZE_MAX,AYATSURI_MODEL_INVALID);
    REJECT(owner,1,g,2,AYATSURI_MODEL_INVALID);
    g[0].node_id=0;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    g[0].node_id=3;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);g[0].node_id=1;
    g[0].vertex_count=5;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);g[0].vertex_count=6;
    g[0].vertices=NULL;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    g[0].vertices=(const void *)i->arena;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    g[0].vertices=(const void *)i;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    g[0].vertices=(const void *)((const uint8_t *)result+1);REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    g[0].vertices=result;
    REJECT(owner,1,(const void *)i->arena,1,AYATSURI_MODEL_INVALID);
    REJECT(owner,1,(const void *)((const uint8_t *)g+1),1,AYATSURI_MODEL_INVALID);
    float x=result[5].position[0];result[5].position[0]=NAN;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    result[5].position[0]=4097;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);result[5].position[0]=x;
    float z=result[5].normal[2];result[5].normal[2]=0;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);
    result[5].normal[2]=INFINITY;REJECT(owner,1,g,1,AYATSURI_MODEL_INVALID);result[5].normal[2]=z;
    /* Valid first entry plus invalid final entry must not publish a prefix. */
    g[1].node_id=2;g[1].vertex_count=3;REJECT(owner,1,g,2,AYATSURI_MODEL_INVALID);
#undef REJECT
    i->busy=true;assert(ayatsuri_model_instance_evaluate_meshes(i,owner,1,camera,light,g,1)==AYATSURI_MODEL_BUSY);i->busy=false;
    assert(!memcmp(i,&saved,sizeof saved));free(arena);
}
static void unsupported(const char *dir,const char *variant,ayatsuri_skin_result *result){
    size_t bytes;uint8_t *blob=load(dir,variant,&bytes);ayatsuri_model m;assert(ayatsuri_model_open(blob,bytes,&m)==AYATSURI_MODEL_OK);
    size_t n=ayatsuri_model_instance_bytes(&m);void *arena=malloc(n),*saved=malloc(n);assert(arena&&saved);unsigned owner=0;
    ayatsuri_model_instance i;assert(ayatsuri_model_instance_init(&m,arena,n,&owner,1,&i)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);const ayatsuri_model_instance before=i;memcpy(saved,arena,n);
    ayatsuri_model_mesh_geometry g={1,6,result};
    assert(ayatsuri_model_instance_evaluate_meshes(&i,&owner,1,camera,light,&g,1)==AYATSURI_MODEL_INVALID);
    assert(!memcmp(&i,&before,sizeof i)&&!memcmp(saved,arena,n));
    assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_OK);free(saved);free(arena);free(blob);
}
int main(int argc,char **argv){
    assert(argc==2);size_t bytes;uint8_t *blob=load(argv[1],"plain",&bytes),*original=malloc(bytes);assert(original);memcpy(original,blob,bytes);
    ayatsuri_model model;assert(ayatsuri_model_open(blob,bytes,&model)==AYATSURI_MODEL_OK);
    const size_t n=ayatsuri_model_instance_bytes(&model);void *arena=malloc(n),*other_arena=malloc(n);assert(arena&&other_arena);
    unsigned owner=1,other_owner=2;ayatsuri_model_instance actor,other;
    assert(ayatsuri_model_instance_init(&model,arena,n,&owner,1,&actor)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_init(&model,other_arena,n,&other_owner,1,&other)==AYATSURI_MODEL_OK);
    ayatsuri_skin_vertex *input=malloc(6*sizeof *input);ayatsuri_skin_result *result=malloc(6*sizeof *result);
    ayatsuri_transform *global=malloc(2*sizeof *global),*bind=malloc(2*sizeof *bind),*palette=malloc(2*sizeof *palette);
    uint8_t *storage=calloc(1,2048),*expected_storage=calloc(1,2048);float *scratch=malloc(240*8*sizeof *scratch);
    assert(input&&result&&global&&bind&&palette&&storage&&expected_storage&&scratch);
    rest_inputs(input);bind[0]=bind[1]=identity;bind[1].m[3]=-15;deform(0,global,bind,palette,input,result);
    ayatsuri_model_mesh_geometry geometry={1,6,result};
    assert(ayatsuri_model_instance_evaluate(&actor,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate(&other,&other_owner,1,camera,light)==AYATSURI_MODEL_OK);
    ksn_mesh_snapshot baseline=snapshot(storage,2048);append_model(&actor,&owner,&baseline);render(&baseline,rest,actual_z,4,1,scratch);
    rejection_cases(&actor,&owner,result);unsupported(argv[1],"holes",result);unsupported(argv[1],"sided",result);
    size_t warm_calls=calls,warm_live=live;uint32_t hashes[120];size_t max_snapshot=0;unsigned changed=0;
    for(unsigned visit=0;visit<240;++visit){
        unsigned frame=visit<120?visit:239-visit;deform(frame,global,bind,palette,input,result);
        assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
        ksn_mesh_snapshot actual=snapshot(storage,2048),expected=snapshot(expected_storage,2048);
        append_model(&actor,&owner,&actual);
        float z[6];ksn_proc_gouraud_patch patch=oracle(result,z);assert(ksn_mesh_append(&expected,&patch,z,1));
        ayatsuri_model_mesh_view back;assert(ayatsuri_model_instance_mesh_view(&other,&other_owner,1,2,&back)==AYATSURI_MODEL_OK);
        assert(ksn_mesh_append(&expected,back.patch,back.depth,2));
        assert(actual.packets.size==expected.packets.size&&!memcmp(storage,expected_storage,actual.packets.size));
        if(actual.packets.size>max_snapshot)max_snapshot=actual.packets.size;
        render(&expected,reference,reference_z,8,8,scratch);
        for(unsigned band=4;band<=8;band+=4)for(unsigned rows=1;rows<=8;rows+=7){
            render(&actual,pixels,actual_z,band,rows,scratch);
            assert(!memcmp(pixels,reference,sizeof pixels)&&!memcmp(actual_z,reference_z,sizeof actual_z));
        }
        unsigned red=0,blue=0;uint32_t hash=2166136261u;
        for(unsigned j=0;j<240*135;++j){red+=(pixels[j]&0xf800)!=0;blue+=pixels[j]==31;hash=(hash^pixels[j])*16777619u;}
        assert(red>100&&blue>100);if(!frame)assert(!memcmp(pixels,rest,sizeof pixels));
        changed+=memcmp(pixels,rest,sizeof pixels)!=0;
        if(visit<120)hashes[frame]=hash;else assert(hashes[frame]==hash);
        /* Same time still recomputes, but must produce the same snapshot. */
        assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
        actual=snapshot(storage,2048);append_model(&actor,&owner,&actual);
        assert(actual.packets.size==expected.packets.size&&!memcmp(storage,expected_storage,actual.packets.size));
        assert(!memcmp(blob,original,bytes));
    }
    assert(changed>200&&calls==warm_calls&&live==warm_live);
    printf("SKIN_RENDER_WARM frames=240 pause=240 band_rows=4,8 depth_rows=1,8 requested_calls=0 live=%zu cold=%zu arena_each=%zu instance=%zu input=240 result=144 joint_work=288 snapshot_capacity_each=2048 snapshot_used_max=%zu scratch=7680\n",live,bytes,n,sizeof actor,max_snapshot);
    deform(30,global,bind,palette,input,result);hierarchy_and_batches(&actor,&owner,input,result);
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    /* Borrow ends at evaluation, before append as well as before rendering. */
    ksn_mesh_snapshot before_poison=snapshot(expected_storage,2048);append_model(&actor,&owner,&before_poison);
    memset(result,0xa5,6*sizeof *result);
    ksn_mesh_snapshot after_poison=snapshot(storage,2048);append_model(&actor,&owner,&after_poison);
    assert(before_poison.packets.size==after_poison.packets.size&&!memcmp(storage,expected_storage,before_poison.packets.size));
    /* No override pointers are retained when restoring the old cache either. */
    assert(ayatsuri_model_instance_evaluate(&actor,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    ksn_mesh_snapshot current=snapshot(storage,2048);append_model(&actor,&owner,&current);render(&current,pixels,actual_z,4,1,scratch);
    assert(!memcmp(pixels,rest,sizeof pixels)&&!actor.mesh_overridden);
    ayatsuri_model_stats stats=actor.stats;assert(ayatsuri_model_instance_evaluate(&actor,&owner,1,camera,light)==AYATSURI_MODEL_OK);
    assert(actor.stats.geometry_updates==stats.geometry_updates);
    deform(0,global,bind,palette,input,result);
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    for(unsigned j=0;j<6;++j){result[j].normal[0]=1;result[j].normal[2]=0;}
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    ayatsuri_model_mesh_view view;assert(ayatsuri_model_instance_mesh_view(&actor,&owner,1,1,&view)==AYATSURI_MODEL_OK);
    assert(((const ksn_proc_gouraud_patch *)view.patch)->vertices[0].color_a==0x4000);
    /* A reflected skin remains single-sided; never repair winding silently. */
    palette[0]=palette[1]=identity;palette[0].m[0]=palette[1].m[0]=-1;
    assert(ayatsuri_skin_evaluate(palette,2,input,6,result)==AYATSURI_OK);
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    uint32_t cursor=0;ayatsuri_model_packet packet;
    assert(ayatsuri_model_instance_next(&actor,&owner,1,&cursor,&packet)==AYATSURI_MODEL_OK&&packet.node_id==2);
    assert(ayatsuri_model_instance_next(&actor,&owner,1,&cursor,&packet)==AYATSURI_MODEL_DONE);
    /* Valid input can fail projection; no half-evaluated packet is readable. */
    deform(30,global,bind,palette,input,result);float bad_camera[9];memcpy(bad_camera,camera,sizeof camera);bad_camera[6]=1000;
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,bad_camera,light,&geometry,1)==AYATSURI_MODEL_RANGE);
    assert(ayatsuri_model_instance_mesh_view(&actor,&owner,1,1,&view)==AYATSURI_MODEL_STATE);
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_mesh_view(&actor,&owner,1,1,&view)==AYATSURI_MODEL_OK);
    /* Failed append publishes nothing; the caller starts a fresh retry. */
    ksn_mesh_snapshot too_small=snapshot(storage,1);memset(storage,0x5a,2048);
    assert(!ksn_mesh_append(&too_small,view.patch,view.depth,1)&&!too_small.packets.size&&!too_small.work);
    for(unsigned j=0;j<2048;++j)assert(storage[j]==0x5a);
    too_small=snapshot(storage,2048);too_small.work_limit=0;
    assert(!ksn_mesh_append(&too_small,view.patch,view.depth,1)&&!too_small.packets.size&&!too_small.work);
    current=snapshot(storage,2048);append_model(&actor,&owner,&current);render(&current,reference,reference_z,8,8,scratch);
    /* Every source owner, including the model blob, dies before rasterization. */
    assert(ayatsuri_model_instance_dispose(&actor,&owner,1)==AYATSURI_MODEL_OK);
    assert(ayatsuri_model_instance_evaluate_meshes(&actor,&owner,1,camera,light,&geometry,1)==AYATSURI_MODEL_STATE);
    assert(ayatsuri_model_instance_dispose(&other,&other_owner,1)==AYATSURI_MODEL_OK);
    free(arena);free(other_arena);free(blob);free(original);free(input);free(result);free(global);free(bind);free(palette);
    memset(&model,0,sizeof model);memset(&geometry,0,sizeof geometry);
    render(&current,pixels,actual_z,4,1,scratch);
    assert(!memcmp(pixels,reference,sizeof pixels)&&!memcmp(actual_z,reference_z,sizeof actual_z));
    free(storage);free(expected_storage);free(scratch);assert(live==0);
    printf("SKIN_RENDER_PASS immutable_cold independent_instances local_TRS shading reflected_cull invalid_atomic recovery snapshot_after_source_free quota_retry requested_live=0 requested_peak=%zu\n",peak);
    return 0;
}
