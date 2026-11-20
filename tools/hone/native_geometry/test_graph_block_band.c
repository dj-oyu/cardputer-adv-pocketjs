#include "graph_block_band_candidate.h"
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
    unsigned success=0, overflow=0;
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
        assert(hone_graph_index_build(&index,&p.graph,trial+1,bounds,24,ids,24*17));
        unsigned y=2+pick(10);
        int expected=hone_graph_index_band_render(&opaque,&p,&index,trial+1,&w,y,1,reference);
        int ok=hone_graph_block_band_render(&opaque,&p,&index,trial+1,&w,y,1,actual);
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
    assert(!hone_graph_block_band_preflight(&opaque,&p,&index,2001,&w));
    codes[1][1].value=.2f;
    assert(hone_graph_block_band_preflight(&opaque,&p,&index,2001,&w));
    assert(!hone_graph_block_band_preflight(&opaque,&p,&index,2002,&w));
    assert(success && overflow);
    printf("block raster matched %u rows and %u overflow rejections; hidden invalid rejected\n",success,overflow);
}
