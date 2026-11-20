#include "graph_compact_tile_candidate.h"
#include <stdio.h>
#include <assert.h>
static uint32_t state=7;
static unsigned take(unsigned limit) {state=state*1664525u+1013904223u;return (state>>8)%limit;}

int main(void) {
    int owner=0;
    hone_projected_packet packets[12];
    hone_projected_material colors[12]={0};
    hone_compact_bounds bounds[12];
    uint16_t ids[204],actual[8*240],expected[8*240],row[240];
    hone_compact_tile_work work;
    hone_projected_snapshot s={0};
    assert(hone_projected_snapshot_init(&s,&owner,1,packets,12,colors,12));
    hone_graph_packet graph[12];
    hone_alpha_instruction constants[12][4];
    hone_alpha_program programs[12][4];
    hone_graph_band_payload payload={.graph={.packets=graph,.capacity=12,.owner=&owner,.generation=1,.ready=1},
                                    .binding=&owner,.generation=1,.span_limit=240};
    float depth[240],scratch[4];
    hone_graph_band_work gw={&owner,1,row,depth,scratch,4};
    ksn_mesh_snapshot opaque={.enabled=true};
    hone_compact_index compact;
    hone_graph_band_index ref;
    hone_graph_bounds ref_bounds[12];
    uint16_t ref_ids[204];
    unsigned off_grid=0;
    for (unsigned trial=1;trial<=3000;trial++) {
        assert(hone_projected_begin(&s,&owner,1,trial));
        s.count=1+take(12);
        payload.graph.count=s.count;
        for (unsigned j=0;j<s.count;j++) {
            for (unsigned k=0;k<4;k++) {
                constants[j][k]=(hone_alpha_instruction){.op=HONE_ALPHA_CONST,.value=k?take(17)/16.f:1};
                programs[j][k]=(hone_alpha_program){&constants[j][k],1,0};
            }
            assert(hone_projected_material_admit(programs[j],programs[j]+1,&colors[j]));
            const float xy[6]={0,1,236,1,3,46};
            graph[j]=(hone_graph_packet){.order=take(4),.triangle={.alpha=programs[j],.color=programs[j]+1}};
            packets[j]=(hone_projected_packet){.material=j,.order=graph[j].order};
            for (unsigned k=0;k<3;k++) {
                unsigned v=(j%2 && k)?3-k:k;
                const float z=trial%2 ? (float)((int)take(7)-3) : 1+take(3)*FLT_EPSILON;
                const float x=xy[v*2]+(v==0 && (trial+j)%2 ? 0x1p-21f : take(4)*.125f);
                const float y=xy[v*2+1]+take(4)*.125f;
                graph[j].triangle.v[k]=(hone_graph_vertex){x,y,z,{0}};
                packets[j].xyz[k*3]=x;
                packets[j].xyz[k*3+1]=y;
                packets[j].xyz[k*3+2]=z;
            }
        }
        assert(hone_projected_seal(&s));
        assert(hone_compact_build(&compact,&s,bounds,12,ids,204));
        off_grid+=compact.off_grid;
        assert(hone_graph_index_build(&ref,&payload.graph,trial,ref_bounds,12,ref_ids,204));
        unsigned y=take(42),h=1+take(8);
        assert(hone_graph_index_band_render(&opaque,&payload,&ref,trial,&gw,y,h,expected));
        assert(hone_compact_tile_render(&compact,&owner,1,trial,&work,y,h,actual));
        assert(!memcmp(actual,expected,h*240*sizeof *actual));
        assert(!hone_compact_tile_render(&compact,&owner,1,trial+1,&work,y,1,actual));
    }
    assert(!hone_compact_tile_render(&compact,&owner,1,3000,&work,135,1,actual));
    assert(!hone_compact_tile_render(&compact,&owner,1,3000,&work,10,1,(uint16_t*)packets));
    assert(!hone_compact_build(&compact,&s,bounds,12,ids,0));
    assert(!compact.source);
    packets[0].xyz[2]=NAN;
    assert(!hone_compact_build(&compact,&s,bounds,12,ids,204));
    assert(off_grid);
    printf("compact tile parity: 3000 random bands, %u off-grid packets, stale, alias, and capacity rejection\n",off_grid);
}
