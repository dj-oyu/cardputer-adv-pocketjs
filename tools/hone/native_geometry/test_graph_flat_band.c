#include "graph_flat_band_candidate.h"
#include <assert.h>
#include <stdio.h>

static uint32_t seed = 17;
static unsigned pick(unsigned limit) {
    seed = seed*1664525u+1013904223u;
    return (seed>>8)%limit;
}
int main(void) {
    int owner;
    hone_graph_packet packets[24];
    hone_alpha_instruction codes[24][4];
    hone_alpha_program programs[24][4];
    hone_graph_band_payload p = {
        .graph={.packets=packets,.capacity=24,.owner=&owner,.generation=1,.ready=1},
        .binding=&owner,.generation=1,.span_limit=240
    };
    hone_graph_band_index index;
    hone_graph_bounds bounds[24];
    uint16_t ids[24*17], row[240], reference[240], actual[240];
    float depth[240], scratch[16];
    hone_graph_band_work w = {&owner,1,row,depth,scratch,16};
    ksn_mesh_snapshot opaque = {.enabled=true};
    unsigned success=0, overflow=0, fast=0;
    const float alphas[]={0,.2f,.8f,1,nextafterf(1,0)};
    for (unsigned trial=0; trial<2000; trial++) {
        p.graph.count=1+pick(24);
        for (unsigned j=0; j<p.graph.count; j++) {
            float z=(float)pick(5), dx=(float)pick(6);
            for (unsigned k=0; k<4; k++) {
                codes[j][k]=(hone_alpha_instruction){.op=HONE_ALPHA_CONST,.value=k?pick(17)/16.f:alphas[pick(5)]};
                programs[j][k]=(hone_alpha_program){&codes[j][k],1,0};
            }
            packets[j]=(hone_graph_packet){
                .triangle={.v={{12+dx,2,z,{0}},{36+dx,2,z,{0}},{12+dx,12,z,{0}}},
                           .alpha=programs[j],.color=programs[j]+1},.order=pick(4)};
        }
        for (unsigned j=0; j<p.graph.count; j++) {
            if (trial%3) codes[j][0].value=1;
            for (unsigned k=0; k<3; k++) {
                hone_graph_vertex *v=&packets[j].triangle.v[k];
                if (trial%2) {
                    v->x+=pick(9)*.125f;
                    v->y+=pick(9)*.125f;
                }
                v->z=trial%7 ? (float)((int)pick(7)-3)+pick(4)*FLT_EPSILON : (float)(pick(3)*1e-35f);
            }
            if (trial%5==0) {
                hone_graph_vertex v=packets[j].triangle.v[1];
                packets[j].triangle.v[1]=packets[j].triangle.v[2];
                packets[j].triangle.v[2]=v;
            }
        }
        assert(hone_graph_index_build(&index,&p.graph,trial+1,bounds,24,ids,24*17));
        unsigned y=2+pick(10);
        int expected=hone_graph_index_band_render(&opaque,&p,&index,trial+1,&w,y,1,reference);
        hone_flat_plan plan=hone_flat_prepare(&p,&index,trial+1); fast+=plan.flat; int ok=hone_flat_band_render(&opaque,&p,&plan,&w,y,1,actual);
        assert(ok==expected);
        if (ok) { assert(!memcmp(reference,actual,sizeof actual)); success++; }
        else overflow++;
    }
    p.graph.count=2;
    packets[1]=packets[0];
    packets[1].triangle.color=programs[1]+1;
    codes[0][0].value=1;
    codes[1][1].value=NAN;
    for (unsigned v=0; v<3; v++) packets[1].triangle.v[v].z=packets[0].triangle.v[v].z-2;
    assert(hone_graph_index_build(&index,&p.graph,2001,bounds,24,ids,24*17));
    hone_flat_plan plan=hone_flat_prepare(&p,&index,2001); assert(!plan.flat); assert(!hone_flat_band_preflight(&opaque,&p,&plan,&w));
    codes[1][1].value=.2f;
    plan=hone_flat_prepare(&p,&index,2001); assert(plan.flat); assert(hone_flat_band_preflight(&opaque,&p,&plan,&w));
    plan.revision=2002;
    assert(!hone_flat_band_preflight(&opaque,&p,&plan,&w));
    packets[0].triangle.v[0].x=0x1p-19f;
    assert(hone_graph_index_build(&index,&p.graph,2003,bounds,24,ids,24*17));
    plan=hone_flat_prepare(&p,&index,2003);
    assert(!plan.flat && hone_flat_band_preflight(&opaque,&p,&plan,&w));
        /* A second triangle occupies the same integer samples. Colors differ,
     * so the exact order and packet-index ties are observable in RGB565. */
    packets[0].triangle.v[0]=(hone_graph_vertex){16,2,1,{0}};
    packets[0].triangle.v[1]=(hone_graph_vertex){32,2,1,{0}};
    packets[0].triangle.v[2]=(hone_graph_vertex){16,10,1,{0}};
    packets[1]=packets[0];
    packets[1].triangle.color=programs[1]+1;
    for (unsigned k=1;k<4;k++) {
        codes[0][k].value=k==1;
        codes[1][k].value=k==2;
    }
    unsigned revision=2004;
    for (unsigned scenario=0;scenario<4;scenario++,revision++) {
        packets[0].order=1;
        packets[1].order=scenario==1 ? 0 : 1;
        for (unsigned k=0;k<3;k++) packets[1].triangle.v[k].z=scenario>=2 ? nextafterf(1,2) : 1;
        assert(hone_graph_index_build(&index,&p.graph,revision,bounds,24,ids,24*17));
        plan=hone_flat_prepare(&p,&index,revision);
        assert(plan.flat);
        assert(hone_flat_band_render(&opaque,&p,&plan,&w,3,1,actual));
        assert(hone_graph_index_band_render(&opaque,&p,&index,revision,&w,3,1,reference));
        assert(!memcmp(actual,reference,sizeof actual));
        assert(actual[17]==(scenario==0 ? 0xf800 : 0x07e0));
    }
    p.span_limit=1;
    assert(!hone_flat_band_preflight(&opaque,&p,&plan,&w));
    assert(fast>=1300);
    assert(success && overflow);
    printf("flat raster %u admitted, matched %u rows and %u overflow rejections; hidden invalid and off-grid fallback checked\n",fast,success,overflow);
}
