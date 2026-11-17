import pathlib, subprocess, tempfile, unittest
from native_geometry import emit
from test_consumer_plan import fixture


class TestNativeGather(unittest.TestCase):
    def test_shared_source_vertex_and_sparse_loops(self):
        package=fixture()
        mesh=package['meshes'][0]
        mesh['corner_normals'] += [[0,0,1]]*4
        mesh['polygons'][1]['loops']=[7,8,9]
        mesh['triangles'][1]['loops']=[7,8,9]
        here=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as tmp:
            path=pathlib.Path(tmp)
            (path/'geometry.h').write_text(emit(package))
            (path/'native_gather.h').write_text((here/'native_gather.h').read_text())
            (path/'test.c').write_text('''#include "geometry.h"
#include "native_gather.h"
#include <assert.h>
#include <string.h>
int main(void) {
 const hone_input_mesh *m=hone_input_meshes;
 float positions[12]={0,0,0,1,0,0,0,1,0,0,0,1};
 float normals[18]={0,0,1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0};
 uint32_t part[2]={0,3}; hone_gather_vertex result[2];
 assert(m->source_corner_count==10 && m->polygon_corner_count==6);
 assert(hone_gather_part(m,part,2,positions,normals,result));
 assert(result[0].position[0]==result[1].position[0]);
 assert(result[0].normal[2]==1 && result[1].normal[1]==1);
 hone_gather_vertex saved[2]; memcpy(saved,result,sizeof result);
 part[1]=m->vertices; assert(!hone_gather_part(m,part,2,positions,normals,result));
 assert(!memcmp(saved,result,sizeof result));
 part[1]=3; normals[11]=NAN; assert(!hone_gather_part(m,part,2,positions,normals,result));
 assert(!memcmp(saved,result,sizeof result));
 uint32_t full[32]; hone_gather_vertex block[32];
 for (unsigned i=0;i<32;i++) full[i]=0;
 normals[11]=0; assert(hone_gather_part(m,full,32,positions,normals,block));
 assert(!hone_gather_part(m,full,33,positions,normals,block));
 return 0;
}''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',str(path/'test.c'),'-lm','-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
            rejected=subprocess.run(['cc','-std=c11','-ffast-math','-c',str(path/'test.c'),'-o',str(path/'unsafe.o')],capture_output=True)
            self.assertNotEqual(rejected.returncode,0)

if __name__=='__main__': unittest.main()
