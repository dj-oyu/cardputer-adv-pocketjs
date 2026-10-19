#define _POSIX_C_SOURCE 200809L
#include "derby_runner_affine.h"
#include "ui/kasane/ksn_proc_plan.h"
#include "ui/kasane/ksn_proc_points_pie.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ksn_proc_vm vm;
static ksn_proc_frame reference,candidate,before;
static const float colors[3]={0xc460,0x237f,0xa76f};
static unsigned max_error,accepted,fallback,changed,model_batches;
static unsigned temporal,natural_moves,candidate_moves,extra_toggles,missing_moves,max_temporal_error;
static unsigned sampled_pixels,max_changed_pixels,pixel_samples;
static float min_input[8],max_input[8];
static float samples[4096][8];static unsigned sample_count;
static ksn_proc_rom_plan *plan;
static uint16_t old_pixels[240*135],new_pixels[240*135];
static const char *picture;
static uint64_t now(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static ksn_proc_status old_run(const float in[8]){
    ksn_proc_status s=ksn_proc_rom_plan_begin(&vm,plan,in,&reference);
    return s==KSN_PROC_RUNNING?ksn_proc_rom_plan_run(&vm,plan,false):s;
}
static unsigned compare(void){
    assert(reference.count==13&&candidate.count==13);
    unsigned diff=0;
    for(unsigned i=0;i<13;i++){
        const ksn_proc_segment *a=&reference.segments[i],*b=&candidate.segments[i];
        assert(a->color==b->color);
        const int aa[4]={a->x0,a->y0,a->x1,a->y1},bb[4]={b->x0,b->y0,b->x1,b->y1};
        for(unsigned k=0;k<4;k++){
            const unsigned e=(unsigned)abs(aa[k]-bb[k]);assert(e<=1);
            if(e>max_error)max_error=e;
            diff+=e!=0;
        }
    }
    return diff;
}
static void model_parity(const float in[8]){
    static const _Alignas(16) int16_t x[16]={-16,0,48,64,80,64,32,32,48};
    static const _Alignas(16) int16_t y[16]={16,0,0,-32,-16,-16,0,-16,-32};
    _Alignas(16) int16_t ox[16],oy[16];
    const int16_t s=(int16_t)lroundf(in[2]*2560.0f);
    const KsnProcAffineQ14 c={s,0,0,s,(int32_t)lroundf(in[0]*16384)+8192,(int32_t)lroundf(in[1]*16384)+8192};
    ksn_proc_points_affine_pie((KsnProcPointDst){ox,oy},(KsnProcPointSrc){x,y},9,&c);
    const unsigned seg[9]={0,1,2,3,4,4,9,9,12};
    const bool end[9]={false,false,false,false,false,true,false,true,false};
    for(unsigned i=0;i<9;i++){
        const ksn_proc_segment *p=&candidate.segments[seg[i]];
        assert(ox[i]==(end[i]?p->x1:p->x0)&&oy[i]==(end[i]?p->y1:p->y0));
    }
    ++model_batches;
}
static void pixels(void){
    memset(old_pixels,0,sizeof old_pixels);memset(new_pixels,0,sizeof new_pixels);
    ksn_proc_render_band(&reference,old_pixels,0,135);
    ksn_proc_render_band(&candidate,new_pixels,0,135);
    unsigned diff=0;for(unsigned i=0;i<240*135;i++)diff+=old_pixels[i]!=new_pixels[i];
    ++pixel_samples;sampled_pixels+=diff;
    if(diff>max_changed_pixels){
        max_changed_pixels=diff;
        FILE *f=fopen(picture,"wb");assert(f);fprintf(f,"P6\n720 135\n255\n");
        for(unsigned y=0;y<135;y++)for(unsigned panel=0;panel<3;panel++)for(unsigned x=0;x<240;x++){
            const unsigned at=y*240+x;
            uint16_t p=panel==0?old_pixels[at]:panel==1?new_pixels[at]:old_pixels[at]!=new_pixels[at]?0xf81f:0;
            const unsigned char rgb[3]={(unsigned char)(((p>>11)&31)*255/31),(unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};
            assert(fwrite(rgb,1,3,f)==3);
        }
        fclose(f);
    }
}
static bool check(const float in[8],unsigned serial){
    assert(old_run(in)==KSN_PROC_DONE);
    KsnProcPointsDecision d;
    const KsnProcPointsPolicy uncalibrated={true,0};
    if(!derby_runner_affine(in,colors,&uncalibrated,&candidate,&d)){
        ++fallback;candidate=reference;return false;
    }
    assert(d.backend==KSN_PROC_POINTS_SCALAR);++accepted;
    const unsigned diff=compare();changed+=diff!=0;
    model_parity(in);
    if(serial%32==0)pixels();
    for(unsigned k=0;k<8;k++){
        if(in[k]<min_input[k])min_input[k]=in[k];
        if(in[k]>max_input[k])max_input[k]=in[k];
    }
    if(sample_count<4096)memcpy(samples[sample_count++],in,8*sizeof(float));
    return true;
}
static void boundaries(void){
    for(unsigned i=0;i<20000;i++){
        float in[8]={-400+(float)(i%1601)*.5f,-200+(float)(i%1001)*.5f,
                     (float)(i%8193)/1024,80,100,102,108,110};
        check(in,i);
    }
    const float ties[]={-100.5f,-.5f,0,.5f,100.5f,719.0f};
    for(unsigned i=0;i<sizeof ties/sizeof *ties;i++){
        float in[8]={ties[i],0,0,2,0,1,2,3};check(in,i);
    }
    const float scales[]={nextafterf(0,1),nextafterf(8,0),8,1.0f/5120,
                          nextafterf(1.0f/5120,0),nextafterf(1.0f/5120,1)};
    for(unsigned i=0;i<sizeof scales/sizeof *scales;i++){
        float in[8]={-10.5f,40.5f,scales[i],100,-10,-5,0,5};check(in,i);
    }
    for(unsigned k=0;k<8;k++)for(unsigned j=0;j<4;j++){
        float in[8]={100,60,1,80,100,102,108,110};
        const float bad[]={NAN,INFINITY,-INFINITY,100000};in[k]=bad[j];
        memset(&candidate,0xa5,sizeof candidate);before=candidate;
        assert(!derby_runner_affine(in,colors,NULL,&candidate,NULL));
        assert(!memcmp(&candidate,&before,sizeof candidate));
    }
    float in[8]={100,60,nextafterf(8,INFINITY),80,100,102,108,110};
    assert(!derby_runner_affine(in,colors,NULL,&candidate,NULL));
    in[2]=-.001f;assert(!derby_runner_affine(in,colors,NULL,&candidate,NULL));
    in[2]=1;in[0]=719;assert(!derby_runner_affine(in,colors,NULL,&candidate,NULL));
}
typedef struct {bool valid;unsigned frame;ksn_proc_segment old[13],current[13];} Previous;
static Previous prev[8];
static void motion(unsigned horse,unsigned frame){
    Previous *p=&prev[horse];
    if(p->valid&&frame==p->frame+1){
        for(unsigned j=0;j<13;j++){
            const ksn_proc_segment *a=&reference.segments[j],*b=&candidate.segments[j],*c=&p->old[j],*d=&p->current[j];
            const int aa[4]={a->x0,a->y0,a->x1,a->y1},bb[4]={b->x0,b->y0,b->x1,b->y1};
            const int cc[4]={c->x0,c->y0,c->x1,c->y1},dd[4]={d->x0,d->y0,d->x1,d->y1};
            for(unsigned k=0;k<4;k++){
                const int old=aa[k]-cc[k],cur=bb[k]-dd[k];
                const unsigned e=(unsigned)abs(cur-old);assert(e<=2);
                ++temporal;natural_moves+=old!=0;candidate_moves+=cur!=0;
                extra_toggles+=old==0&&cur!=0;missing_moves+=old!=0&&cur==0;
                if(e>max_temporal_error)max_temporal_error=e;
            }
        }
    }
    p->valid=true;p->frame=frame;
    memcpy(p->old,reference.segments,sizeof p->old);memcpy(p->current,candidate.segments,sizeof p->current);
}
int main(int argc,char **argv){
    assert(argc==3);picture=argv[2];const ksn_proc_rom_entry *entry=NULL;
    for(unsigned i=0;i<ksn_proc_rom_plans_count;i++)if(!strcmp(ksn_proc_rom_plans[i].name,"derby.runner"))entry=&ksn_proc_rom_plans[i];
    assert(entry&&entry->params==3);
    plan=calloc(1,ksn_proc_rom_plan_bytes(3));assert(plan);plan->params=3;
    memcpy(plan->args,colors,sizeof colors);assert(ksn_proc_rom_plan_prepare(plan,entry));
    for(unsigned i=0;i<8;i++){min_input[i]=INFINITY;max_input[i]=-INFINITY;}
    boundaries();const unsigned boundary_accepted=accepted;
    accepted=fallback=changed=model_batches=0;
    sampled_pixels=max_changed_pixels=pixel_samples=sample_count=0;
    for(unsigned i=0;i<8;i++){min_input[i]=INFINITY;max_input[i]=-INFINITY;}
    FILE *f=fopen(argv[1],"r");assert(f);unsigned seq,frame,horse,visible,last_seq=0,rows=0,culled=0;
    unsigned visibility_changes=0;bool last_visible[8]={0};float in[8];
    while(fscanf(f,"%u %u %u %u %f %f %f %f %f %f %f %f",&seq,&frame,&horse,&visible,
                 &in[0],&in[1],&in[2],&in[3],&in[4],&in[5],&in[6],&in[7])==12){
        assert(horse<8);++rows;
        if(seq!=last_seq){memset(prev,0,sizeof prev);memset(last_visible,0,sizeof last_visible);last_seq=seq;}
        if(frame&&last_visible[horse]!=(bool)visible)++visibility_changes;
        last_visible[horse]=(bool)visible;
        if(!visible){++culled;prev[horse].valid=false;continue;}
        check(in,rows);motion(horse,frame);
    }
    assert(feof(f));fclose(f);assert(rows>100000&&accepted>10000&&fallback>0&&temporal>100000);
    assert(visibility_changes>0&&pixel_samples>0);
    const unsigned reps=100000;volatile unsigned sink=0;
    uint64_t start=now();for(unsigned i=0;i<reps;i++){assert(old_run(samples[i%sample_count])==KSN_PROC_DONE);sink+=reference.count;}
    const uint64_t vm_ns=now()-start;start=now();
    for(unsigned i=0;i<reps;i++){assert(derby_runner_affine(samples[i%sample_count],colors,NULL,&candidate,NULL));sink+=candidate.count;}
    const uint64_t int_ns=now()-start;
    printf("AFFINE_INPUT_RANGE {\"min\":[");
    for(unsigned i=0;i<8;i++)printf("%s%.9g",i?",":"",(double)min_input[i]);
    printf("],\"max\":[");for(unsigned i=0;i<8;i++)printf("%s%.9g",i?",":"",(double)max_input[i]);puts("]}");
    printf("AFFINE_RESULT {\"rows\":%u,\"culled\":%u,\"accepted\":%u,\"fallback\":%u,\"boundary_accepted\":%u,\"changed_runner_draws\":%u,\"max_endpoint_error_px\":%u,\"temporal_coordinates\":%u,\"float_moves\":%u,\"candidate_moves\":%u,\"extra_toggles\":%u,\"missing_moves\":%u,\"max_temporal_delta_error_px\":%u,\"visibility_changes\":%u,\"pixel_samples\":%u,\"sum_changed_pixels\":%u,\"max_changed_pixels_per_sample\":%u,\"scalar_batches\":%u,\"pie_model_batches\":%u,\"pie_batches\":0,\"host_vm_ns_per_draw\":%.1f,\"host_affine_ns_per_draw\":%.1f}\n",
           rows,culled,accepted,fallback,boundary_accepted,changed,max_error,temporal,natural_moves,candidate_moves,extra_toggles,missing_moves,max_temporal_error,visibility_changes,pixel_samples,sampled_pixels,max_changed_pixels,accepted,model_batches,(double)vm_ns/reps,(double)int_ns/reps);
    assert(sink==reps*26);free(plan);return 0;
}
