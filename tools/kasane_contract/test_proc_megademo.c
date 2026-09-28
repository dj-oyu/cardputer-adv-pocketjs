#include "ksn_proc_plan.h"
#include "proc_megademo.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t scalar_pixels[KSN_PROC_W * KSN_PROC_H];
static uint16_t plan_pixels[KSN_PROC_W * KSN_PROC_H];
static uint16_t debug_pixels[KSN_PROC_W * KSN_PROC_H];

static uint64_t hash_pixels(const uint16_t *pixels) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++) {
        h ^= pixels[i] & 255u; h *= UINT64_C(1099511628211);
        h ^= pixels[i] >> 8;   h *= UINT64_C(1099511628211);
    }
    return h;
}

static void equal_vm(const ksn_proc_vm *a,const ksn_proc_vm *b) {
    assert(a->status==b->status && a->pc==b->pc && a->last_pc==b->last_pc);
    assert(a->steps==b->steps && a->depth==b->depth);
    assert(memcmp(a->reg,b->reg,sizeof a->reg)==0);
    assert(memcmp(a->loop,b->loop,sizeof a->loop)==0);
    assert(a->pen_x==b->pen_x && a->pen_y==b->pen_y && a->pen_valid==b->pen_valid);
    assert(a->frame->ready==b->frame->ready && a->frame->count==b->frame->count);
    assert(a->frame->raster_steps==b->frame->raster_steps);
    assert(memcmp(a->frame->segments,b->frame->segments,
                  a->frame->count*sizeof a->frame->segments[0])==0);
}

static unsigned visible_nonbackdrop(const uint16_t *pixels,uint16_t backdrop) {
    unsigned n=0;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++) n += pixels[i]!=backdrop;
    return n;
}

int main(void) {
    uint64_t first_hash[3]={0},last_hash[3]={0};
    unsigned phase_frames[3]={0}, registered_fused_sites=0, total_segments=0;
    ksn_proc_plan cached[3][PROC_MEGA_LAYERS];
    for(unsigned frame=0;frame<PROC_MEGA_FRAMES;frame++) {
        const unsigned phase=proc_mega_phase(frame);
        const uint16_t backdrop=proc_mega_backdrop(frame);
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)
            scalar_pixels[i]=plan_pixels[i]=debug_pixels[i]=backdrop;
        unsigned frame_segments=0;
        for(unsigned layer=0;layer<PROC_MEGA_LAYERS;layer++) {
            ksn_proc_inst code[KSN_PROC_CODE];
            ksn_proc_program program;
            float input[KSN_PROC_INPUTS];
            ksn_proc_plan *plan=&cached[phase][layer];
            ksn_proc_vm scalar,compiled,debug;
            ksn_proc_frame sf,pf,df;
            assert(proc_mega_build(frame,layer,code,&program,input));
            assert(program.count>0 && program.count<=KSN_PROC_CODE);
            if((frame % 16u)==0) {
                assert(ksn_proc_plan_prepare(plan,&program));
                registered_fused_sites+=plan->fused_count;
            } else {
                assert(plan->valid && plan->program.count==program.count);
                assert(memcmp(plan->code,code,program.count*sizeof code[0])==0);
            }
            if(layer==3) assert(plan->fused_count>=2);
            assert(ksn_proc_begin(&scalar,&program,input,&sf)==KSN_PROC_RUNNING);
            assert(ksn_proc_plan_begin(&compiled,plan,input,&pf)==KSN_PROC_RUNNING);
            assert(ksn_proc_plan_begin(&debug,plan,input,&df)==KSN_PROC_RUNNING);
            assert(ksn_proc_run(&scalar)==KSN_PROC_DONE);
            assert(ksn_proc_plan_run(&compiled,plan,false)==KSN_PROC_DONE);
            assert(ksn_proc_plan_run(&debug,plan,true)==KSN_PROC_DONE);
            equal_vm(&scalar,&compiled); equal_vm(&scalar,&debug);
            assert(pf.count>0 && pf.count<=KSN_PROC_SEGMENTS);
            assert(pf.raster_steps<=KSN_PROC_RASTER_STEPS);
            assert(compiled.steps<KSN_PROC_STEPS);
            frame_segments+=pf.count;
            assert(ksn_proc_render_band(&sf,scalar_pixels,0,KSN_PROC_H));
            assert(ksn_proc_render_band(&pf,plan_pixels,0,KSN_PROC_H));
            assert(ksn_proc_render_band(&df,debug_pixels,0,KSN_PROC_H));
        }
        assert(frame_segments>=110); /* multiple animated structures, not a static line sample */
        total_segments+=frame_segments;
        assert(memcmp(scalar_pixels,plan_pixels,sizeof scalar_pixels)==0);
        assert(memcmp(scalar_pixels,debug_pixels,sizeof scalar_pixels)==0);
        assert(visible_nonbackdrop(plan_pixels,backdrop)>1200);
        const uint64_t hash=hash_pixels(plan_pixels);
        if(!phase_frames[phase]) first_hash[phase]=hash;
        else assert(hash!=last_hash[phase]);
        last_hash[phase]=hash;
        phase_frames[phase]++;
    }
    for(unsigned p=0;p<3;p++) {
        assert(phase_frames[p]==16);
        assert(first_hash[p]!=last_hash[p]);
        printf("phase %u first=%016llx last=%016llx\n",p,
               (unsigned long long)first_hash[p],(unsigned long long)last_hash[p]);
    }
    assert(first_hash[0]!=first_hash[1] && first_hash[1]!=first_hash[2]);
    assert(registered_fused_sites>=6);
    printf("proc megademo: %u frames, %u segments, %u registered fusion sites, scalar/plan/debug identical\n",
           PROC_MEGA_FRAMES,total_segments,registered_fused_sites);
    return 0;
}
