import pathlib, subprocess, tempfile, unittest
from native_geometry import emit
from test_consumer_plan import fixture


class TestNativeDeform(unittest.TestCase):
    def test_source_once_morph_then_skin(self):
        package=fixture(); mesh=package['meshes'][0]
        for key in mesh['shape_keys']['blocks']: key['positions']=list(key['positions'])
        mesh['vertices'].append([9,8,7])
        for key in mesh['shape_keys']['blocks']: key['positions'].append([9,8,7])
        mesh['vertex_groups']=['joint','bad']
        mesh['weights']=[[[0,1]],[[0,1]],[[0,1]],[[1,1]],[]]
        here=pathlib.Path(__file__).parent
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp)
            (p/'geometry.h').write_text(emit(package))
            for name in ('native_morph.h','native_dq.h','native_deform.h'):
                (p/name).write_text((here/name).read_text())
            (p/'test.c').write_text('''#include "geometry.h"
#include "native_morph.h"
#include "native_dq.h"
#include "native_deform.h"
#include <assert.h>
int main(void){
 const hone_input_mesh *m=hone_input_meshes;
 float values[2]={0,1},masks[10]={1,1,1,1,1,1,1,1,1,1},out[15];
 uint32_t map[2]={0,1};
 double joints[14]={1,0,0,0,2,0,0,1,0,0,0,2,0,0};
 double identity[12]={1,0,0,0,0,1,0,0,0,0,1,0};
 assert(hone_deform_source(m,values,masks,2,1,map,2,joints,2,identity,identity,out));
 assert(out[0]==2 && out[9]==2 && out[11]==.5f && out[12]==9);
 masks[7]=0;assert(hone_deform_source(m,values,masks,2,0,0,0,0,0,0,0,out));
 assert(out[9]==0 && out[11]==1);
 masks[7]=NAN;assert(!hone_deform_source(m,values,masks,2,0,0,0,0,0,0,0,out));
 masks[7]=1; joints[7]=NAN; out[0]=-42;
 assert(!hone_deform_source(m,values,masks,2,1,map,2,joints,2,identity,identity,out));
 assert(out[0]==2); /* scratch may contain a partial candidate */
 hone_input_mesh bad=*m; uint32_t wrong[5]={1,1,2,4,UINT32_MAX};
 bad.source_representative=wrong;
 assert(!hone_deform_source(&bad,values,masks,2,0,0,0,0,0,0,0,out));
 bad=*m; bad.morphs=0;
 assert(hone_deform_source(&bad,0,0,0,0,0,0,0,0,0,0,out));
 assert(out[0]==0 && out[11]==1 && out[14]==7);
 return 0;
}''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',str(p/'test.c'),str(here.parent/'source_package'/'rigid_dq_candidate.c'),'-lm','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__': unittest.main()
