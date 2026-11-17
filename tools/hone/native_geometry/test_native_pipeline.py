"""Synthetic source deformation -> full polygon normals -> part gather."""
import pathlib, subprocess, tempfile, unittest
from native_geometry import emit
from test_consumer_plan import fixture


class TestNativePipeline(unittest.TestCase):
    def test_deform_then_normals_before_part_gather(self):
        here=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp); (p/'geometry.h').write_text(emit(fixture()))
            for name in ('native_morph.h','native_dq.h','native_deform.h','native_gather.h','native_provider.h'):
                (p/name).write_text((here/name).read_text())
            (p/'test.c').write_text('''#include "geometry.h"
#include "native_morph.h"
#include "native_dq.h"
#include "native_deform.h"
#include "native_gather.h"
#include "ayatsuri/ayatsuri_model_provider.h"
#include "native_provider.h"
#include <assert.h>
#include <string.h>
int polygon_normals(const float*,size_t,const uint32_t*,const uint32_t*,const uint8_t*,size_t,size_t,double*,float*);
int main(void){
 const hone_input_mesh *m=hone_input_meshes;
 float values[2]={0,1},masks[8]={1,1,1,1,1,1,1,1};
 float positions[12],normals[18];double work[12];
 assert(hone_deform_source(m,values,masks,2,0,0,0,0,0,0,0,positions));
 assert(positions[11]==.5f);
 assert(polygon_normals(positions,4,m->polygon_offsets,m->polygon_vertices,m->polygon_smooth,2,6,work,normals));
 uint32_t p0[3]={0,1,2},p1[3]={3,4,5};hone_gather_vertex a[3],b[3];
 assert(hone_gather_part(m,p0,3,positions,normals,a));
 assert(hone_gather_part(m,p1,3,positions,normals,b));
 assert(a[0].position[0]==b[0].position[0]);
 assert(a[0].normal[2]==1 && b[0].normal[1]==1);
 assert(b[1].position[2]==.5f);
 hone_provider_mesh mesh={m,positions,normals};
 hone_provider_part nodes[2]={{0,3,p0},{0,3,p1}};
 hone_provider_context ctx={&mesh,1,nodes,2};ayatsuri_skin_result slot[3];
 assert(hone_native_provider(&ctx,1,3,slot));
 assert(slot[0].normal[1]==1 && slot[1].position[2]==.5f);
 slot[0].position[0]=42;assert(!hone_native_provider(&ctx,2,3,slot));
 assert(slot[0].position[0]==42);
 /* A failure in the final vertex must not publish any earlier results. */
 ayatsuri_skin_result saved[3];memcpy(saved,slot,sizeof slot);
 normals[17]=NAN;assert(!hone_native_provider(&ctx,1,3,slot));
 assert(!memcmp(slot,saved,sizeof slot));normals[17]=0;
 normals[16]=2;assert(!hone_native_provider(&ctx,1,3,slot));
 assert(!memcmp(slot,saved,sizeof slot));normals[16]=1;
 p1[2]=m->vertices;assert(!hone_native_provider(&ctx,1,3,slot));
 assert(!memcmp(slot,saved,sizeof slot));p1[2]=5;
 positions[3]=INFINITY;assert(!hone_native_provider(&ctx,1,3,slot));
 assert(!memcmp(slot,saved,sizeof slot));positions[3]=1;
 assert(hone_native_provider(&ctx,1,3,slot));
 assert(slot[0].normal[1]==1 && slot[1].position[2]==.5f);
 /* Full-capacity mapping, with repeated prepared corners and exact values. */
 uint32_t full[32];for(unsigned i=0;i<32;i++)full[i]=i%6;
 hone_provider_part wide={0,32,full};ctx.nodes=&wide;ctx.node_count=1;
 ayatsuri_skin_result wide_slot[32],wide_saved[32];hone_gather_vertex wide_gather[32];
 assert(hone_native_provider(&ctx,0,32,wide_slot));
 assert(hone_gather_part(m,full,32,positions,normals,wide_gather));
 for(unsigned i=0;i<32;i++)for(unsigned k=0;k<3;k++){
  assert(wide_slot[i].position[k]==wide_gather[i].position[k]);
  assert(wide_slot[i].normal[k]==wide_gather[i].normal[k]);
 }
 memcpy(wide_saved,wide_slot,sizeof wide_slot);full[31]=m->vertices;
 assert(!hone_native_provider(&ctx,0,32,wide_slot));
 assert(!memcmp(wide_saved,wide_slot,sizeof wide_slot));
 return 0;
}''')
            src=here.parent/'source_package'
            root=here.parents[2]
            for opt in ('-O0','-O2'):
                binary=p/('test'+opt)
                subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',opt,'-fstrict-aliasing','-Wstrict-aliasing=2','-ffp-contract=off','-I'+str(root/'main'),str(p/'test.c'),str(src/'rigid_dq_candidate.c'),str(src/'native_polygon_normals.c'),'-lm','-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

if __name__=='__main__': unittest.main()
